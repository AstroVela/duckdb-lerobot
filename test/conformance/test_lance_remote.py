#!/usr/bin/env python3
"""Exercise private Lance videos through independent HTTPFS and Lance secrets."""

import argparse
import json
import subprocess
import tempfile
from pathlib import Path

import lance
import pyarrow as pa
from lance.blob import Blob, BlobArray
from lerobot_lancedb.convert import convert

from test_session_settings import s3_server, sql_string


def run(args, sql, error=False):
    setup = (
        f"LOAD {sql_string(args.extension)}; LOAD {sql_string(args.lance_extension)}; "
        f"LOAD {sql_string(args.httpfs_extension) if args.httpfs_extension else 'httpfs'}; "
        "SET threads=4; SET http_retries=0; SET http_timeout=5; "
    )
    result = subprocess.run(
        [str(args.duckdb), "-unsigned", "-json", "-batch"],
        input=setup + sql,
        capture_output=True,
        text=True,
        timeout=120,
    )
    if error:
        assert result.returncode != 0, result.stdout
        expected_errors = (
            (error,)
            if isinstance(error, str)
            else error if isinstance(error, tuple) else ("403", "forbidden", "credential", "access denied")
        )
        assert any(word in result.stderr.lower() for word in expected_errors), result.stderr
    else:
        assert result.returncode == 0, result.stderr
    return result


def last_result(output):
    decoder = json.JSONDecoder()
    remaining = output.strip()
    result = None
    while remaining:
        result, end = decoder.raw_decode(remaining)
        remaining = remaining[end:].lstrip()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", type=Path, required=True)
    parser.add_argument("--extension", type=Path, required=True)
    parser.add_argument("--lance-extension", type=Path, required=True)
    parser.add_argument("--httpfs-extension", type=Path)
    parser.add_argument("--rgb-codec", default="libaom-av1")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="lerobot-lance-remote-") as temporary:
        base = Path(temporary)
        parquet, dataset = base / "parquet", base / "lance"
        features = {"camera": {"dtype": "video", "shape": [64, 64, 3]}}
        run(
            args,
            "COPY (SELECT 0::BIGINT AS episode_index, 'move' AS task, "
            "from_hex(repeat(printf('%02x', i * 31 + 20), 64*64*3)) AS camera "
            f"FROM range(3) t(i)) TO {sql_string(parquet)} "
            f"(FORMAT lerobot, FPS 10, FEATURES {sql_string(json.dumps(features))}, "
            f"RGB_CODEC {sql_string(args.rgb_codec)}, VIDEO_WORKERS 1, ENCODER_THREADS 1);",
        )
        convert(dataset, root=parquet)
        objects = {
            f"dataset/{path.relative_to(dataset).as_posix()}": path.read_bytes()
            for path in dataset.rglob("*")
            if path.is_file()
        }
        root = "'s3://fixture-bucket/dataset'"
        calls = (
            f"lerobot_video_routes({root}, [0])",
            f"lerobot_video_frames({root}, [0])",
            f"lerobot_video_windows({root}, [{{request_id: 1, episode_index: 0, frame_index: 1}}], "
            "delta_timestamps := [-0.1, 0.0, 0.1])",
            f"lerobot_video_targets({root}, (SELECT 1 AS request_id, 0 AS episode_index, "
            "1 AS frame_index, 'camera' AS video_key, 0 AS delta_index))",
        )
        revoked = {"prefix": None}
        objects["control/revoke"] = b"revoked"

        def policy(method, path, authorized):
            if path == "/fixture-bucket/control/revoke":
                revoked["prefix"] = "/fixture-bucket/dataset/videos.lance/"
                return True
            if revoked["prefix"] and path.startswith(revoked["prefix"]):
                return False
            return authorized

        with s3_server(objects, request_access=policy) as (endpoint, counts):
            metadata_secret = (
                "CREATE OR REPLACE SECRET metadata_access (TYPE S3, SCOPE 's3://fixture-bucket/dataset/meta/', "
                "KEY_ID 'fixture-session-id', SECRET 'fixture-session-key', REGION 'us-east-1', "
                f"ENDPOINT {sql_string(endpoint)}, USE_SSL false, URL_STYLE 'path');"
            )
            lance_secret = (
                "CREATE OR REPLACE SECRET lance_access (TYPE LANCE, SCOPE 's3://fixture-bucket/dataset/', "
                "ACCESS_KEY_ID 'fixture-session-id', SECRET_ACCESS_KEY 'fixture-session-key', REGION 'us-east-1', "
                f"ENDPOINT {sql_string('http://' + endpoint)}, ALLOW_HTTP true, "
                "VIRTUAL_HOSTED_STYLE_REQUEST false);"
            )
            # Invalid defaults keep missing scoped secrets on the loopback server.
            defaults = (
                f"SET s3_endpoint={sql_string(endpoint)}; SET s3_use_ssl=false; SET s3_url_style='path'; "
                "SET s3_region='us-east-1'; SET s3_access_key_id='fixture-denied'; "
                "SET s3_secret_access_key='fixture-denied'; "
                + lance_secret.replace("lance_access", "lance_fallback")
                .replace("s3://fixture-bucket/dataset/", "s3://fixture-bucket/")
                .replace("fixture-session-id", "fixture-denied")
            )
            for cache in (False, True):
                setup = (
                    defaults + f"SET enable_external_file_cache={str(cache).lower()};" + metadata_secret + lance_secret
                )
                for call in calls:
                    query = f"SELECT * EXCLUDE(video_path) FROM {call} ORDER BY ALL;"
                    local_query = query.replace(root, sql_string(dataset))
                    expected = last_result(run(args, local_query).stdout)
                    actual = last_result(run(args, setup + query).stdout)
                    assert actual == expected, (call, expected, actual)
                    transactional = last_result(run(args, setup + "BEGIN;" + query + "ROLLBACK;").stdout)
                    assert transactional == expected, (call, expected, transactional)
                    for secret in ("metadata_access", "lance_access"):
                        run(
                            args,
                            setup + "BEGIN;" + query + f"DROP SECRET {secret};" + query,
                            # Parent-context fingerprint I/O can reject metadata
                            # access before any nested reader is initialized.
                            error=(
                                ("secret catalog views", "403", "forbidden", "access denied")
                                if secret == "metadata_access"
                                else "secret catalog views"
                            ),
                        )
                    for secret in ("metadata_access", "lance_access"):
                        # The first query warms caches in the same connection.
                        run(args, setup + query + f"DROP SECRET {secret};" + query, error=True)
                    run(
                        args,
                        setup + query + lance_secret.replace("fixture-session-id", "fixture-denied") + query,
                        error=True,
                    )
                    restored = run(
                        args,
                        setup + query + "DROP SECRET lance_access;" + lance_secret + query,
                    )
                    assert restored.returncode == 0
                    # Change only the backing service policy while credentials
                    # and object versions stay unchanged in this connection.
                    try:
                        run(
                            args,
                            setup
                            + query
                            + "SELECT content FROM read_blob('s3://fixture-bucket/control/revoke');"
                            + query,
                            error=True,
                        )
                    finally:
                        revoked["prefix"] = None
                print(f"PASS: private Lance video parity, secret and server-policy revocation, external_cache={cache}")
            assert counts["accepted"] > 0 and counts["denied"] > 0, counts

            # External payloads require their own scoped Lance secret. The
            # dataset secret intentionally cannot authorize this separate path.
            source = lance.dataset(dataset / "videos.lance")
            rows = source.to_table(
                columns=["video_key", "chunk_index", "file_index", "file_size"], with_row_id=True
            ).to_pylist()
            blobs = []
            for index, row in enumerate(rows):
                with source.take_blobs("video_bytes", ids=[row.pop("_rowid")])[0] as blob:
                    payload = blob.read()
                key = f"external/video-{index}.bin"
                objects[key] = b"prefix!" + payload + b"!suffix"
                blobs.append(Blob.from_uri(f"s3://fixture-bucket/{key}", 7, len(payload)))
            array = BlobArray.from_pylist(blobs)
            table = pa.Table.from_pylist(rows).append_column(pa.field("video_bytes", array.type), array)
            external = base / "external-videos.lance"
            lance.write_dataset(table, external, data_storage_version="2.2", allow_external_blob_outside_bases=True)
            for key in list(objects):
                if key.startswith("dataset/videos.lance/"):
                    del objects[key]
            for path in external.rglob("*"):
                if path.is_file():
                    objects[f"dataset/videos.lance/{path.relative_to(external).as_posix()}"] = path.read_bytes()
            external_secret = lance_secret.replace("lance_access", "external_access").replace(
                "s3://fixture-bucket/dataset/", "s3://fixture-bucket/external/"
            )
            for cache in (False, True):
                setup = (
                    defaults + f"SET enable_external_file_cache={str(cache).lower()};" + metadata_secret + lance_secret
                )
                for call in calls[1:]:
                    query = f"SELECT * EXCLUDE(video_path) FROM {call} ORDER BY ALL;"
                    expected = last_result(run(args, query.replace(root, sql_string(dataset))).stdout)
                    actual = last_result(run(args, setup + external_secret + query).stdout)
                    assert actual == expected, (call, expected, actual)
                    run(args, setup + query, error=True)
                    run(args, setup + external_secret + query + "DROP SECRET external_access;" + query, error=True)
                print(f"PASS: external video payloads require their scoped secret, external_cache={cache}")


if __name__ == "__main__":
    main()
