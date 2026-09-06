#!/usr/bin/env python3
"""Exercise real HTTPFS reads with connection-local settings on loopback only."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import subprocess
import tempfile
import threading
import xml.etree.ElementTree as ET
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlsplit


def sql_string(value: str | Path) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def run_sql(args: argparse.Namespace, sql: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(args.duckdb), "-unsigned", "-csv", "-noheader", "-c", sql],
        capture_output=True,
        text=True,
        timeout=45,
    )


def make_objects(args: argparse.Namespace, directory: Path) -> dict[str, bytes]:
    data = directory / "data.parquet"
    episodes = directory / "episodes.parquet"
    result = run_sql(
        args,
        "COPY (SELECT 0::BIGINT AS episode_index, i::BIGINT AS frame_index, i::FLOAT AS action, "
        f"i / 2.0 AS timestamp FROM range(2) t(i)) TO {sql_string(data)} (FORMAT PARQUET);"
        "COPY (SELECT 0::BIGINT AS episode_index, 2 AS length, "
        '0 AS "data/chunk_index", 0 AS "data/file_index", '
        '0 AS "videos/camera/chunk_index", 0 AS "videos/camera/file_index", '
        '0.0 AS "videos/camera/from_timestamp", 1.0 AS "videos/camera/to_timestamp") '
        f"TO {sql_string(episodes)} (FORMAT PARQUET);",
    )
    assert result.returncode == 0, result.stderr
    info = {
        "codebase_version": "v3.0",
        "fps": 2,
        "total_episodes": 1,
        "total_frames": 2,
        "total_tasks": 0,
        "data_path": "data.parquet",
        "video_path": "video.mp4",
        "features": {
            "camera": {"dtype": "video", "shape": [16, 16, 3]},
            "action": {"dtype": "float32", "shape": [1]},
        },
    }
    return {
        "dataset/meta/info.json": json.dumps(info).encode(),
        "dataset/meta/episodes/chunk-000/file-000.parquet": episodes.read_bytes(),
        "dataset/data.parquet": data.read_bytes(),
        "dataset/video.mp4": (Path(__file__).resolve().parents[1] / "data/lerobot/long-20701.mp4").read_bytes(),
    }


@contextmanager
def s3_server(
    objects: dict[str, bytes],
    deny_all: bool = False,
    public_paths=(),
    request_access=None,
):
    # The server accepts only a deliberately public fixture key ID. It tests
    # which context supplied the settings, not AWS's signature implementation.
    # Never retain or log the Authorization header, including on failure.
    counts = {"accepted": 0, "denied": 0}
    counts_lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_HEAD(self):
            self.respond()

        def do_GET(self):
            self.respond()

        def respond(self):
            parsed = urlsplit(self.path)
            authorized = not deny_all and (
                unquote(parsed.path) in public_paths
                or "Credential=fixture-session-id/" in self.headers.get("Authorization", "")
            )
            if request_access:
                authorized = request_access(self.command, unquote(parsed.path), authorized)
            with counts_lock:
                counts["accepted" if authorized else "denied"] += 1
            if not authorized:
                self.send_error(403, "Fixture access denied")
                return
            query = parse_qs(parsed.query)
            if "list-type" in query:
                root = ET.Element("ListBucketResult", xmlns="http://s3.amazonaws.com/doc/2006-03-01/")
                ET.SubElement(root, "IsTruncated").text = "false"
                prefix = query.get("prefix", [""])[0]
                for key, value in objects.items():
                    if key.startswith(prefix):
                        entry = ET.SubElement(root, "Contents")
                        ET.SubElement(entry, "Key").text = key
                        ET.SubElement(entry, "Size").text = str(len(value))
                        ET.SubElement(entry, "LastModified").text = "2026-01-01T00:00:00.000Z"
                        ET.SubElement(entry, "ETag").text = '"' + hashlib.sha256(value).hexdigest() + '"'
                payload = ET.tostring(root)
                self.send_response(200)
                self.send_header("Content-Type", "application/xml")
            else:
                key = unquote(parsed.path).removeprefix("/fixture-bucket/")
                if key not in objects:
                    self.send_error(404, "No fixture object")
                    return
                value = objects[key]
                payload = value
                range_header = self.headers.get("Range")
                if range_header and self.command == "GET":
                    first, last = range_header.removeprefix("bytes=").split("-", 1)
                    start = int(first)
                    end = min(int(last) if last else len(value) - 1, len(value) - 1)
                    payload = value[start : end + 1]
                    self.send_response(206)
                    self.send_header("Content-Range", f"bytes {start}-{end}/{len(value)}")
                else:
                    self.send_response(200)
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("ETag", '"' + hashlib.sha256(value).hexdigest() + '"')
                self.send_header("Last-Modified", "Thu, 01 Jan 2026 00:00:00 GMT")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(payload)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"127.0.0.1:{server.server_port}", counts
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def check_endpoint_cache(args, directory, original_objects, base):
    objects = dict(original_objects)
    info = json.loads(objects["dataset/meta/info.json"])
    info["data_path"] = "data-{file_index}.parquet"
    info["video_path"] = "video-{file_index}.mp4"
    objects["dataset/meta/info.json"] = json.dumps(info).encode()
    objects["dataset/data-0.parquet"] = objects.pop("dataset/data.parquet")
    objects["dataset/video-0.mp4"] = objects.pop("dataset/video.mp4")
    route, data = directory / "episodes-b.parquet", directory / "data-b.parquet"
    result = run_sql(
        args,
        'COPY (SELECT * REPLACE (1 AS "data/file_index", 1 AS "videos/camera/file_index") '
        f"FROM read_parquet({sql_string(directory / 'episodes.parquet')})) "
        f"TO {sql_string(route)} (FORMAT PARQUET);"
        "COPY (SELECT 0::BIGINT AS episode_index, i::BIGINT AS frame_index, "
        "11::FLOAT AS action, i / 2.0 AS timestamp FROM range(2) t(i)) "
        f"TO {sql_string(data)} (FORMAT PARQUET);",
    )
    assert result.returncode == 0, result.stderr
    other = dict(objects)
    other["dataset/meta/episodes/chunk-000/file-000.parquet"] = route.read_bytes()
    other["dataset/data-1.parquet"] = data.read_bytes()
    other["dataset/video-1.mp4"] = objects["dataset/video-0.mp4"]
    # Both stores retain both shards, so a secret that only redirects episode
    # metadata can change routing without moving the data/video objects.
    objects["dataset/data-1.parquet"] = other["dataset/data-1.parquet"]
    objects["dataset/video-1.mp4"] = other["dataset/video-1.mp4"]
    # Both endpoints serve bit-identical info.json (and thus size, mtime and
    # ETag), but route the dataset to different files. Old shard 0 remains in B.
    with s3_server(objects) as (first, _), s3_server(other) as (second, _):
        secret_scopes = {
            "SECRET": "",
            "EPISODES_SECRET": "/meta/episodes/",
            "FILE_SECRET": "/meta/episodes/chunk-000/file-000.parquet",
        }
        for scope in ("SESSION", "GLOBAL", *secret_scopes):
            setup = base + (
                f"SET GLOBAL s3_endpoint={sql_string(first)};" "SET GLOBAL s3_access_key_id='fixture-session-id';"
            )
            root = "s3://fixture-bucket/dataset"

            def change_endpoint(endpoint):
                if scope in secret_scopes:
                    suffix = secret_scopes[scope]
                    return (
                        "CREATE OR REPLACE SECRET fixture_endpoint (TYPE s3, "
                        "KEY_ID 'fixture-session-id', SECRET 'fixture-key', "
                        f"ENDPOINT {sql_string(endpoint)}, USE_SSL false, URL_STYLE 'path', "
                        f"SCOPE 's3://fixture-bucket/dataset{suffix}');"
                    )
                return f"SET {scope} s3_endpoint={sql_string(endpoint)};"

            scan = f"SELECT sum(action)::BIGINT FROM lerobot_scan({sql_string(root)});"
            video = (
                f"SELECT min(video_path) FROM lerobot_video_frames({sql_string(root)}, " "[0], frame_indices := [0]);"
            )
            sql = setup + change_endpoint(first) + scan
            expected = ([["true"]] if "SECRET" in scope else []) + [["1"]]
            if not args.skip_video:
                sql += video
                expected += [["s3://fixture-bucket/dataset/video-0.mp4"]]
            sql += change_endpoint(second)
            if "SECRET" in scope:
                expected += [["true"]]
            if scope == "FILE_SECRET":
                # Listing comes from A while this individual object comes from
                # B. HTTPFS correctly detects conflicting ETags; a warm route
                # cache must not hide that native error either.
                native = run_sql(
                    args,
                    setup
                    + change_endpoint(second)
                    + "SELECT count(*) FROM read_parquet('s3://fixture-bucket/dataset/meta/episodes/**/*.parquet');",
                )
                assert native.returncode != 0 and "ETag" in native.stderr, native.stderr
                result = run_sql(args, sql + scan)
                assert result.returncode != 0 and "ETag" in result.stderr, result.stderr
                assert list(csv.reader(io.StringIO(result.stdout))) == expected, result.stdout
                print(
                    "PASS FILE_SECRET endpoint conflict matches native ETag error",
                    flush=True,
                )
                continue
            sql += "SELECT sum(action)::BIGINT FROM read_parquet('s3://fixture-bucket/dataset/data-1.parquet');" + scan
            expected += [["22"], ["22"]]
            if not args.skip_video:
                sql += video
                expected += [["s3://fixture-bucket/dataset/video-1.mp4"]]
            # Returning to the old endpoint must restore both route caches.
            sql += change_endpoint(first) + scan
            if "SECRET" in scope:
                expected += [["true"]]
            expected += [["1"]]
            result = run_sql(args, sql)
            assert result.returncode == 0, (scope, result.stderr)
            rows = list(csv.reader(io.StringIO(result.stdout)))
            assert rows == expected, (scope, rows, expected)
            print(
                f"PASS {scope} endpoint switch invalidates data and video routes",
                flush=True,
            )
        # Repeated lookups reuse the same identity. A neighboring dataset's
        # secret must not evict this dataset's warmed route cache.
        result = run_sql(
            args,
            setup + scan + "CREATE SECRET fixture_neighbor (TYPE s3, KEY_ID 'fixture-neighbor', "
            "SECRET 'fixture-neighbor-key', SCOPE 's3://fixture-bucket/dataset-neighbor/');"
            "SELECT cached FROM lerobot_cache_info('s3://fixture-bucket/dataset') WHERE component='data';" + scan,
        )
        assert result.returncode == 0, result.stderr
        assert list(csv.reader(io.StringIO(result.stdout))) == [
            ["1"],
            ["true"],
            ["true"],
            ["1"],
        ], result.stdout
        print("PASS an unrelated secret preserves the warmed cache", flush=True)
        check_metadata_changes(args, directory, objects, other, base)


def check_metadata_changes(args, directory, first, second, base):
    empty = directory / "empty-episodes.parquet"
    result = run_sql(
        args,
        f"COPY (SELECT * FROM read_parquet({sql_string(directory / 'episodes.parquet')}) WHERE false) "
        f"TO {sql_string(empty)} (FORMAT PARQUET);",
    )
    assert result.returncode == 0, result.stderr
    metadata = "dataset/meta/episodes/chunk-000/file-000.parquet"
    replacement = "dataset/meta/episodes/chunk-000/file-001.parquet"
    for mutation in ("replace", "listing", "empty-file"):
        objects = dict(first)
        objects["control/update.txt"] = b"changed"
        if mutation == "empty-file":
            objects[replacement] = empty.read_bytes()
        changed = threading.Event()

        def access(method, path, allowed):
            if path == "/fixture-bucket/control/update.txt":
                if method == "GET" and not changed.is_set():
                    if mutation == "replace":
                        objects[metadata] = second[metadata]
                    else:
                        objects[replacement] = second[metadata]
                        if mutation == "listing":
                            del objects[metadata]
                        else:
                            objects[metadata] = empty.read_bytes()
                    changed.set()
                return True
            return allowed

        with s3_server(objects, request_access=access) as (endpoint, _):
            setup = base + (
                f"SET GLOBAL s3_endpoint={sql_string(endpoint)};" "SET GLOBAL s3_access_key_id='fixture-session-id';"
            )
            scan = "SELECT sum(action)::BIGINT FROM lerobot_scan('s3://fixture-bucket/dataset');"
            video = (
                "SELECT min(video_path) FROM lerobot_video_frames('s3://fixture-bucket/dataset', "
                "[0], frame_indices := [0]);"
            )
            sql = setup + scan + (video if not args.skip_video else "")
            expected = [["1"]] + ([["s3://fixture-bucket/dataset/video-0.mp4"]] if not args.skip_video else [])
            # The manifest bytes, ETag and timestamp stay identical. Replacing
            # a file, changing the visible file set, or filling an empty shard
            # must invalidate data and video routes together.
            sql += f"SELECT content FROM read_text('http://{endpoint}/fixture-bucket/control/update.txt');"
            sql += "SELECT max(\"data/file_index\") FROM read_parquet('s3://fixture-bucket/dataset/meta/episodes/**/*.parquet');"
            sql += scan + (video if not args.skip_video else "")
            expected += [["changed"], ["1"], ["22"]] + (
                [["s3://fixture-bucket/dataset/video-1.mp4"]] if not args.skip_video else []
            )
            result = run_sql(args, sql)
            assert result.returncode == 0, (mutation, result.stdout, result.stderr)
            assert list(csv.reader(io.StringIO(result.stdout))) == expected, (
                mutation,
                result.stdout,
            )
            print(
                f"PASS {mutation} metadata change refreshes data and video routes",
                flush=True,
            )


def check_empty_transition(args, original_objects, base, *, file_cache=False):
    objects = dict(original_objects)
    objects["control/empty.txt"] = b"empty"
    objects["control/restore.txt"] = b"restored"

    def access(method, path, allowed):
        if path == "/fixture-bucket/control/restore.txt":
            if method == "GET":
                objects.update(original_objects)
            return True
        if path == "/fixture-bucket/control/empty.txt":
            if method == "GET":
                info = json.loads(objects["dataset/meta/info.json"])
                info.update(total_episodes=0, total_frames=0, total_tasks=0)
                objects["dataset/meta/info.json"] = json.dumps(info).encode()
                for key in list(objects):
                    if "/meta/episodes/" in key:
                        del objects[key]
            return True
        return allowed

    with s3_server(objects, request_access=access) as (endpoint, _):
        setup = base + (
            f"SET GLOBAL s3_endpoint={sql_string(endpoint)};" "SET GLOBAL s3_access_key_id='fixture-session-id';"
        )
        setup += f"SET enable_external_file_cache={'true' if file_cache else 'false'};"
        scan = "SELECT count(*) FROM lerobot_scan('s3://fixture-bucket/dataset');"
        video = (
            "SELECT count(*) FROM lerobot_video_frames('s3://fixture-bucket/dataset', " "[0], frame_indices := [0]);"
        )
        result = run_sql(
            args,
            setup
            + scan
            + (video if not args.skip_video else "")
            + f"SELECT content FROM read_text('http://{endpoint}/fixture-bucket/control/empty.txt');"
            + "SELECT total_frames FROM read_json_auto('s3://fixture-bucket/dataset/meta/info.json');"
            + scan
            + (video if not args.skip_video else "")
            + f"SELECT content FROM read_text('http://{endpoint}/fixture-bucket/control/restore.txt');"
            + scan
            + (video if not args.skip_video else ""),
        )
        assert result.returncode == 0, (result.stdout, result.stderr)
        expected = [["2"]] + ([["1"]] if not args.skip_video else [])
        expected += [["empty"], ["0"], ["0"]] + ([["0"]] if not args.skip_video else [])
        expected += [["restored"], ["2"]] + ([["1"]] if not args.skip_video else [])
        assert list(csv.reader(io.StringIO(result.stdout))) == expected, result.stdout
        print(
            f"PASS empty/full manifest transitions replace warm routes (file cache {file_cache})",
            flush=True,
        )


def check_revoked_metadata_access(args, objects, base):
    # The manifest and payloads are public; only episode metadata requires the
    # fixture key. A successful info.json stat alone cannot authorize a cache hit.
    public = ["/fixture-bucket/" + key for key in objects if "/meta/episodes/" not in key] + [
        "/fixture-bucket/"
    ]  # Listing is public too; the Parquet object is not.
    with s3_server(objects, public_paths=public) as (endpoint, counts):
        setup = base + (
            f"SET GLOBAL s3_endpoint={sql_string(endpoint)};" "SET GLOBAL s3_access_key_id='fixture-global-denied';"
        )
        queries = [
            (
                "data",
                "SELECT sum(action)::BIGINT FROM lerobot_scan('s3://fixture-bucket/dataset');",
            )
        ]
        if not args.skip_video:
            queries += [
                (
                    "video",
                    "SELECT count(*) FROM lerobot_video_frames('s3://fixture-bucket/dataset', [0], frame_indices := [0]);",
                )
            ]
        for scope in ("SESSION", "EPISODES_SECRET", "FILE_SECRET"):
            if scope == "SESSION":
                authorize = "SET SESSION s3_access_key_id='fixture-session-id';"
                revoke = "RESET SESSION s3_access_key_id;"
                expected = [["1"]]
            else:
                suffix = "/meta/episodes/"
                if scope == "FILE_SECRET":
                    suffix += "chunk-000/file-000.parquet"
                authorize = (
                    "CREATE SECRET fixture_access (TYPE s3, "
                    "KEY_ID 'fixture-session-id', SECRET 'fixture-key', "
                    f"ENDPOINT {sql_string(endpoint)}, USE_SSL false, URL_STYLE 'path', "
                    f"SCOPE 's3://fixture-bucket/dataset{suffix}');"
                )
                revoke = "DROP SECRET fixture_access;"
                expected = [["true"], ["1"]]
            native = run_sql(
                args,
                setup
                + authorize
                + revoke
                + "SELECT count(*) FROM read_parquet('s3://fixture-bucket/dataset/meta/episodes/**/*.parquet');",
            )
            assert native.returncode != 0 and "403" in native.stderr, (
                scope,
                native.stderr,
            )
            for label, query in queries:
                cold = run_sql(args, setup + authorize + revoke + query)
                assert cold.returncode != 0 and "403" in cold.stderr, (
                    scope,
                    label,
                    cold.stderr,
                )
                warm = run_sql(args, setup + authorize + query + revoke + query)
                assert warm.returncode != 0 and "403" in warm.stderr, (
                    scope,
                    label,
                    warm.stdout,
                    warm.stderr,
                )
                assert list(csv.reader(io.StringIO(warm.stdout))) == expected, warm.stdout
                print(f"PASS {scope} revocation rejects warm {label} cache", flush=True)
        assert counts["denied"] > 0


def check_server_revocation(args, original_objects, base, *, manifest=False, empty=False):
    # The same process, credentials, endpoint and object bytes remain in use.
    # Only the server's policy changes, between two queries in one connection.
    revoked = threading.Event()
    revoke_kind = "object"
    objects = dict(original_objects)
    if empty:
        info = json.loads(objects["dataset/meta/info.json"])
        info.update(total_episodes=0, total_frames=0, total_tasks=0)
        objects = {"dataset/meta/info.json": json.dumps(info).encode()}
    objects["control/revoke.txt"] = b"revoked"

    def access(method, path, allowed):
        if path == "/fixture-bucket/control/revoke.txt":
            if method == "GET":
                revoked.set()
            return True
        if revoked.is_set():
            if manifest:
                return allowed and not (path.endswith("/meta/info.json") and method == "GET")
            if revoke_kind == "listing" and path == "/fixture-bucket/":
                return False
            if "/meta/episodes/" in path:
                if revoke_kind == "object" or (revoke_kind == "get" and method == "GET"):
                    return False
        return allowed

    public = ["/fixture-bucket/" + key for key in objects if "/meta/episodes/" not in key] + ["/fixture-bucket/"]
    with s3_server(objects, public_paths=public, request_access=access) as (
        endpoint,
        _,
    ):
        setup = base + (
            f"SET GLOBAL s3_endpoint={sql_string(endpoint)};" "SET GLOBAL s3_access_key_id='fixture-session-id';"
        )
        revoke = f"SELECT content FROM read_text('http://{endpoint}/fixture-bucket/control/revoke.txt');"
        queries = [
            (
                "native",
                (
                    "SELECT total_frames FROM read_json_auto('s3://fixture-bucket/dataset/meta/info.json');"
                    if manifest
                    else 'SELECT sum("data/file_index") FROM read_parquet('
                    "'s3://fixture-bucket/dataset/meta/episodes/**/*.parquet');"
                ),
                "2" if manifest and not empty else "0",
            ),
            (
                "data",
                (
                    "SELECT count(*) FROM lerobot_scan('s3://fixture-bucket/dataset');"
                    if empty
                    else "SELECT sum(action)::BIGINT FROM lerobot_scan('s3://fixture-bucket/dataset');"
                ),
                "0" if empty else "1",
            ),
        ]
        if not args.skip_video:
            queries.append(
                (
                    "video",
                    "SELECT count(*) FROM lerobot_video_frames('s3://fixture-bucket/dataset', [0], frame_indices := [0]);",
                    "0" if empty else "1",
                )
            )
        queries += [(label + "-file-cache", query, value) for label, query, value in queries if label != "native"]
        kinds = ("manifest-empty-get" if empty else "manifest-get",) if manifest else ("object", "get", "listing")
        for revoke_kind in kinds:
            for label, query, value in queries:
                revoked.clear()
                passive = (
                    "SELECT cached FROM lerobot_cache_info('s3://fixture-bucket/dataset') WHERE component='data';"
                    if label != "native"
                    else ""
                )
                cache_setting = "SET enable_external_file_cache=true;" if label.endswith("-file-cache") else ""
                result = run_sql(args, setup + cache_setting + query + revoke + passive + query)
                assert result.returncode != 0 and "403" in result.stderr, (
                    revoke_kind,
                    label,
                    result.stdout,
                    result.stderr,
                )
                expected = [[value], ["revoked"]] + ([["true"]] if passive else [])
                assert list(csv.reader(io.StringIO(result.stdout))) == expected, result.stdout
                print(
                    f"PASS server-side {revoke_kind} revocation rejects warm {label} reads",
                    flush=True,
                )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", required=True, type=Path)
    parser.add_argument("--extension", required=True, type=Path)
    parser.add_argument("--httpfs-extension", type=Path)
    parser.add_argument("--skip-video", action="store_true")
    args = parser.parse_args()
    root = "'s3://fixture-bucket/dataset'"
    queries = [
        (
            "native-json",
            "SELECT total_frames FROM read_json_auto('s3://fixture-bucket/dataset/meta/info.json');",
            [["2"]],
        ),
        (
            "native-parquet",
            "SELECT count(*) FROM read_parquet('s3://fixture-bucket/dataset/data.parquet');",
            [["2"]],
        ),
        (
            "metadata",
            f"SELECT count(*), sum(frame_index) FROM lerobot_scan({root});",
            [["2", "1"]],
        ),
    ]
    if not args.skip_video:
        queries.extend(
            [
                (
                    "video-producer",
                    f"SELECT count(*), sum(octet_length(image)) FROM lerobot_video_frames({root}, [0], frame_indices := [0]);",
                    [["1", "768"]],
                ),
                (
                    "video-targets",
                    f"SELECT count(*), sum(octet_length(image)) FROM lerobot_video_targets({root}, (SELECT 0 AS request_id, 0 AS episode_index, 0 AS frame_index, 'camera' AS video_key, 0 AS delta_index), delta_timestamps := [0.0]);",
                    [["1", "768"]],
                ),
            ]
        )
    with tempfile.TemporaryDirectory(prefix="lerobot-session-settings-") as temporary:
        directory = Path(temporary)
        objects = make_objects(args, directory)
        with s3_server(objects) as (endpoint, counts), s3_server(objects, True) as (
            denied_endpoint,
            denied_counts,
        ):
            base = (
                f"SET secret_directory={sql_string(directory / 'secrets')};"
                f"LOAD {sql_string(args.extension)};"
                f"LOAD {sql_string(args.httpfs_extension) if args.httpfs_extension else 'httpfs'};"
                "SET threads=2; SET enable_external_file_cache=false;"
                "SET GLOBAL http_retries=0; SET GLOBAL http_timeout=5;"
                "SET GLOBAL s3_use_ssl=false; SET GLOBAL s3_url_style='path';"
                "SET GLOBAL s3_region='us-east-1'; SET GLOBAL s3_session_token='';"
                "SET GLOBAL s3_secret_access_key='fixture-global-key';"
            )
            setups = {}
            for mode in ("session-auth", "session-endpoint"):
                if mode == "session-auth":
                    setup = (
                        f"SET GLOBAL s3_endpoint={sql_string(endpoint)};"
                        "SET GLOBAL s3_access_key_id='fixture-global-denied';"
                        "SET SESSION s3_access_key_id='fixture-session-id';"
                        "SET SESSION s3_secret_access_key='fixture-session-key';"
                    )
                else:
                    setup = (
                        f"SET GLOBAL s3_endpoint={sql_string(denied_endpoint)};"
                        "SET GLOBAL s3_access_key_id='fixture-session-id';"
                        f"SET SESSION s3_endpoint={sql_string(endpoint)};"
                    )
                setups[mode] = setup
                for name, query, expected in queries:
                    result = run_sql(args, base + setup + query)
                    assert result.returncode == 0, (
                        mode,
                        name,
                        result.stderr,
                        counts,
                        denied_counts,
                    )
                    rows = list(csv.reader(io.StringIO(result.stdout)))
                    assert rows == expected, (mode, name, rows)
                    print(f"PASS {mode}: {name}", flush=True)
            assert counts["accepted"] > 0 and counts["denied"] == 0, counts
            assert denied_counts == {"accepted": 0, "denied": 0}, denied_counts
            # Reusing a warm metadata cache must not preserve a previous
            # session's access after the caller explicitly removes it.
            result = run_sql(
                args,
                base + setups["session-auth"] + queries[2][1] + "RESET SESSION s3_access_key_id;" + queries[2][1],
            )
            assert result.returncode != 0 and "403" in result.stderr, result
            assert counts["denied"] > 0, counts
            print("PASS resetting session credentials revokes cached dataset access")
            check_endpoint_cache(args, directory, objects, base)
            check_revoked_metadata_access(args, objects, base)
            check_server_revocation(args, objects, base)
            check_server_revocation(args, objects, base, manifest=True)
            check_server_revocation(args, objects, base, manifest=True, empty=True)
            check_empty_transition(args, objects, base)
            check_empty_transition(args, objects, base, file_cache=True)


if __name__ == "__main__":
    main()
