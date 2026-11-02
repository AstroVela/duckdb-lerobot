#!/usr/bin/env python3
"""Compare LeRobot video routing and decoding through native files and Lance blobs."""

import argparse
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

import lance
import pyarrow as pa
from lance.blob import Blob, BlobArray
from lerobot_lancedb.convert import convert


def quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def run(args, sql, error=None):
    loads = f"LOAD {quote(args.extension)}; LOAD {quote(args.lance_extension)}; SET threads={args.threads}; "
    result = subprocess.run(
        [str(args.duckdb), "-unsigned", "-json", "-batch"],
        input=loads + sql,
        capture_output=True,
        text=True,
        timeout=120,
    )
    if error is not None:
        assert result.returncode != 0 and error in result.stderr, (
            result.stdout,
            result.stderr,
        )
        return
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout) if result.stdout.strip() else []


def compare(args, parquet, dataset, cameras=2, target_camera="camera.front"):
    calls = (
        "lerobot_video_routes({}, [0, 1])",
        "lerobot_video_frames({}, [0, 1])",
        "lerobot_video_windows({}, [{request_id: 10, episode_index: 0, frame_index: 0}, "
        "{request_id: 20, episode_index: 1, frame_index: 2}], delta_timestamps := [-0.1, 0.0, 0.1])",
        "lerobot_video_targets({}, (SELECT * FROM (VALUES "
        "(10, 0, 0, 'camera.front', 0), (20, 1, 2, 'camera.wrist', 2), "
        "(30, 1, 0, 'camera.front', 1)) t(request_id, episode_index, frame_index, video_key, delta_index)), "
        "delta_timestamps := [-0.1, 0.0, 0.1])",
    )
    for call in calls:
        if "video_targets" in call:
            call = call.replace("camera.front", target_camera)
        query = "SELECT * EXCLUDE(video_path) FROM " + call + " ORDER BY ALL;"
        expected = run(args, query.replace("{}", quote(parquet), 1))
        actual = run(args, query.replace("{}", quote(dataset), 1))
        assert actual == expected, (call, expected, actual)
        assert actual, call
        if "video_frames" in call:
            assert len(actual) == 6 * cameras, len(actual)
    paths = run(
        args,
        f"SELECT DISTINCT video_path FROM lerobot_video_routes({quote(dataset)}, [0, 1]);",
    )
    assert len(paths) == cameras and all(row["video_path"].startswith("lance-blob://") for row in paths), paths


def replace_blobs(dataset, kind, external_dir, stable=False):
    path = dataset / "videos.lance"
    ds = lance.dataset(path)
    rows = ds.to_table(
        columns=["video_key", "chunk_index", "file_index", "file_size"],
        with_row_id=True,
    ).to_pylist()
    row_ids = [row.pop("_rowid") for row in rows]
    payloads = []
    for blob in ds.take_blobs("video_bytes", ids=row_ids):
        with blob:
            payloads.append(blob.read())
    if kind == "external":
        external_dir.mkdir()
        values = []
        for index, payload in enumerate(payloads):
            target = external_dir / f"video-{index}.bin"
            target.write_bytes(b"prefix!" + payload + b"!suffix")
            values.append(Blob.from_uri(target.as_uri(), 7, len(payload)))
    else:
        values = payloads
    blobs = BlobArray.from_pylist(values)
    threshold = max(map(len, payloads)) + 1
    metadata = {
        "lance-encoding:blob-inline-size-threshold": str(threshold if kind == "inline" else 0),
        "lance-encoding:blob-dedicated-size-threshold": str(threshold if kind in ("inline", "packed") else 1),
        "lance-encoding:blob-pack-file-size-threshold": str(threshold * 4),
    }
    table = pa.Table.from_pylist(rows).append_column(pa.field("video_bytes", blobs.type, metadata=metadata), blobs)
    # Overwrite preserves existing field metadata, including blob thresholds.
    # These disposable fixtures need a new dataset to select each storage kind.
    shutil.rmtree(path)
    lance.write_dataset(
        table,
        path,
        data_storage_version="2.2",
        enable_stable_row_ids=stable,
        max_rows_per_file=1,
        allow_external_blob_outside_bases=True,
    )
    rewritten = lance.dataset(path)
    assert len(rewritten.get_fragments()) == len(rows)
    descriptors = lance.dataset(path).to_table(columns=["video_bytes"]).to_pylist()
    expected_kind = {"inline": 0, "packed": 1, "dedicated": 2, "external": 3}[kind]
    assert all(row["video_bytes"]["kind"] == expected_kind for row in descriptors), descriptors


def check_invalid_routes(args, dataset, base):
    # Descriptor structs are a read view; build replacements with the original bytes.
    ds = lance.dataset(dataset / "videos.lance")
    rows = ds.to_table(
        columns=["video_key", "chunk_index", "file_index", "file_size"],
        with_row_id=True,
    ).to_pylist()
    values = []
    for row in rows:
        with ds.take_blobs("video_bytes", ids=[row.pop("_rowid")])[0] as blob:
            values.append(blob.read())
    cases = (
        ("missing", rows[1:], values[1:], "has no route"),
        (
            "duplicate",
            rows + rows[:1],
            values + values[:1],
            "Duplicate LeRobot videos.lance route",
        ),
        ("size", [dict(row, file_size=1) for row in rows], values, "file_size"),
        ("null", rows, [None] + values[1:], "must not be NULL"),
    )
    for name, route_rows, payloads, message in cases:
        target = base / f"invalid-{name}"
        shutil.copytree(dataset, target)
        path = target / "videos.lance"
        blobs = BlobArray.from_pylist(payloads)
        table = pa.Table.from_pylist(route_rows).append_column(pa.field("video_bytes", blobs.type), blobs)
        shutil.rmtree(path)
        lance.write_dataset(table, path, data_storage_version="2.2")
        run(
            args,
            f"SELECT * FROM lerobot_video_routes({quote(target)}, [0, 1]);",
            message,
        )
    print("PASS: missing, duplicate, size-mismatched and NULL video mappings are rejected")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", type=Path, required=True)
    parser.add_argument("--extension", type=Path, required=True)
    parser.add_argument("--lance-extension", type=Path, required=True)
    parser.add_argument("--rgb-codec", choices=("libsvtav1", "libaom-av1"), default="libsvtav1")
    parser.add_argument("--threads", type=int, default=1)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="lerobot-lance-video-") as temporary:
        base = Path(temporary)
        parquet, dataset = base / "parquet", base / "lance's videos"
        features = {
            key: {
                "dtype": "video",
                "shape": [64, 64, 3],
                "names": ["height", "width", "channels"],
            }
            for key in ("camera.front", "camera.wrist")
        }
        # COPY produces two episodes sharing each camera's original MP4 shard.
        run(
            args,
            "COPY (SELECT (i // 3)::BIGINT AS episode_index, 'move' AS task, "
            "from_hex(repeat(printf('%02x', i * 31 + 20), 64*64*3)) AS \"camera.front\", "
            "from_hex(repeat(printf('%02x', i * 19 + 10), 64*64*3)) AS \"camera.wrist\" "
            f"FROM range(6) t(i) ORDER BY i) TO {quote(parquet)} "
            f"(FORMAT lerobot, FPS 10, FEATURES {quote(json.dumps(features))}, "
            f"RGB_CODEC {quote(args.rgb_codec)}, VIDEO_WORKERS 1, ENCODER_THREADS 1);",
        )
        convert(dataset, root=parquet)
        compare(args, parquet, dataset)
        check_invalid_routes(args, dataset, base)
        transaction_calls = (
            f"lerobot_scan({quote(dataset)})",
            f"lerobot_video_routes({quote(dataset)}, [0])",
            f"lerobot_video_frames({quote(dataset)}, [0])",
            f"lerobot_video_windows({quote(dataset)}, "
            "[{request_id: 1, episode_index: 0, frame_index: 0}], delta_timestamps := [0.0])",
            f"lerobot_video_targets({quote(dataset)}, "
            "(SELECT 1 AS request_id, 0 AS episode_index, 0 AS frame_index, "
            "'camera.front' AS video_key, 0 AS delta_index))",
        )
        for call in transaction_calls:
            run(args, f"BEGIN; SELECT count(*) FROM {call}; ROLLBACK;")
            run(
                args,
                f"BEGIN; CREATE SECRET video_tx (TYPE LANCE, SCOPE {quote(dataset)}, HF_TOKEN 'fixture'); "
                f"SELECT count(*) FROM {call};",
                "secret catalog views",
            )
        # Native bind replacements keep the caller's own view and need no comparison.
        run(
            args,
            f"BEGIN; CREATE SECRET video_tx (TYPE LANCE, HF_TOKEN 'fixture'); "
            f"COPY (SELECT count(*) FROM lerobot_info({quote(dataset)})) TO '/dev/null' (FORMAT CSV); ROLLBACK;",
        )
        print("PASS: explicit transactions allow equal temporary secret views and reject uncommitted secrets")
        # More than three large batches must keep the Lance filtered lookup;
        # the Parquet index would attempt to stat frames.lance as a file.
        targets = (
            f"SELECT request_id, timestamp FROM lerobot_video_targets({quote(dataset)}, "
            "(SELECT i AS request_id, 0::BIGINT AS episode_index, (i % 3)::BIGINT AS frame_index, "
            "'camera.front' AS video_key, 0::BIGINT AS delta_index FROM range(5000) t(i)), "
            "max_pending_targets := 256) ORDER BY request_id;"
        )
        result = run(args, targets)
        assert len(result) == 5000 and all(
            abs(row["timestamp"] - (row["request_id"] % 3) / 10) < 1e-6 for row in result
        )
        print("PASS: 5000 targets across repeated Lance timestamp batches")
        for kind in ("inline", "packed", "dedicated", "external"):
            copy = base / kind
            shutil.copytree(dataset, copy)
            replace_blobs(copy, kind, base / f"{kind}-external")
            compare(args, parquet, copy)
            print(f"PASS: {kind} video routes, frames, windows, targets and decoded pixels")
        stable = base / "stable-row-ids"
        shutil.copytree(dataset, stable)
        replace_blobs(stable, "dedicated", base / "stable-external", stable=True)
        compare(args, parquet, stable)
        print("PASS: multiple video fragments and stable row IDs")
        depth_parquet, depth_lance = base / "depth-parquet", base / "depth-lance"
        depth_features = dict(features)
        depth_features["camera.depth"] = {
            "dtype": "video",
            "shape": [64, 64, 1],
            "names": ["height", "width", "channels"],
            "info": {"is_depth_map": True},
        }
        run(
            args,
            "COPY (SELECT (i // 3)::BIGINT AS episode_index, 'depth' AS task, "
            "from_hex(repeat('102030', 64*64)) AS \"camera.front\", "
            "from_hex(repeat('405060', 64*64)) AS \"camera.wrist\", "
            "from_hex(repeat('e803', 64*64)) AS \"camera.depth\" FROM range(6) t(i) ORDER BY i) "
            f"TO {quote(depth_parquet)} (FORMAT lerobot, FPS 10, FEATURES {quote(json.dumps(depth_features))}, "
            f"RGB_CODEC {quote(args.rgb_codec)}, VIDEO_WORKERS 1, ENCODER_THREADS 1);",
        )
        convert(depth_lance, root=depth_parquet)
        compare(args, depth_parquet, depth_lance, cameras=3, target_camera="camera.depth")
        print("PASS: depth video routes, frames, windows and targets preserve decoded float bytes")
        print("PASS: real converter video parity, multiple cameras and shared video shards")


if __name__ == "__main__":
    main()
