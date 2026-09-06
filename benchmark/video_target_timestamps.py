#!/usr/bin/env python3
"""Measure real video_targets batches separately from video decoding.

Synthetic local Parquet shards have exact binary timestamps. Each scenario
runs in a fresh database, without flushing OS caches. A native join supplies
the result oracle and a query-planning comparison, not a replacement decoder.
"""

import argparse
import csv
import hashlib
import io
import json
import shutil
import statistics
import subprocess
import tempfile
from pathlib import Path

from timestamp_lookup import quote


def run(cli, sql):
    result = subprocess.run(
        [str(cli), "-unsigned", "-batch", "-csv", "-noheader"],
        input=sql,
        text=True,
        capture_output=True,
    )
    if result.returncode:
        raise RuntimeError(result.stderr)
    return list(csv.reader(io.StringIO(result.stdout)))


def metrics(profile):
    result = {}
    for key, value in profile.get("extra_info", {}).items():
        if key.startswith("LeRobot "):
            result[key] = int(value)
    for child in profile.get("children", []):
        for key, value in metrics(child).items():
            result[key] = result.get(key, 0) + value
    return result


def fixture(cli, root, rows, shards, video):
    (root / "meta/episodes").mkdir(parents=True)
    (root / "data").mkdir()
    length = rows // shards
    (root / "meta/info.json").write_text(
        json.dumps(
            dict(
                codebase_version="v3.0",
                fps=32,
                total_episodes=shards,
                total_frames=rows,
                total_tasks=0,
                data_path="data/file-{file_index:03d}.parquet",
                video_path="video-{file_index:03d}.mp4",
                features={"camera": {"dtype": "video"}},
            )
        )
    )
    sql = [
        f"COPY (SELECT i::BIGINT episode_index, {length}::BIGINT length, "
        '0::BIGINT "data/chunk_index", i::BIGINT "data/file_index", '
        '0::BIGINT "videos/camera/chunk_index", i::BIGINT "videos/camera/file_index", '
        '0.0::DOUBLE "videos/camera/from_timestamp", '
        f'{(length - 1) / 32.0}::DOUBLE "videos/camera/to_timestamp" '
        f"FROM range({shards}) t(i)) TO {quote(root / 'meta/episodes/routes.parquet')} (FORMAT parquet);"
    ]
    for shard in range(shards):
        sql.append(
            f"COPY (SELECT {shard}::BIGINT episode_index, i::BIGINT frame_index, "
            f"i / 32.0 AS timestamp FROM range({length}) t(i)) "
            f"TO {quote(root / f'data/file-{shard:03d}.parquet')} "
            "(FORMAT parquet, ROW_GROUP_SIZE 2048);"
        )
        if video:
            shutil.copyfile(video, root / f"video-{shard:03d}.mp4")
    run(cli, "\n".join(sql))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", required=True, type=Path)
    parser.add_argument("--extension", required=True, type=Path)
    parser.add_argument("--rows", type=int, default=1_000_000)
    parser.add_argument("--shards", type=int, nargs="+", default=[1, 4])
    parser.add_argument("--targets", type=int, nargs="+", default=[4, 4096, 32768])
    parser.add_argument(
        "--patterns",
        nargs="+",
        choices=["dense", "sparse", "repeated"],
        default=["dense", "sparse", "repeated"],
    )
    parser.add_argument("--batch-size", type=int, default=2048)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--memory-limit", default="512MB")
    parser.add_argument(
        "--video",
        type=Path,
        help="Optional 32 fps clip with at least rows/shards frames",
    )
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if (
        min(
            args.rows,
            args.batch_size,
            args.repeats,
            args.threads,
            *args.shards,
            *args.targets,
        )
        < 1
    ):
        parser.error("sizes, repeats and threads must be positive")
    if any(args.rows % shards for shards in args.shards):
        parser.error("rows must be divisible by every shard count")
    cli = args.duckdb.resolve()
    extension = args.extension.resolve()
    report = {
        "schema_version": 1,
        "duckdb": run(cli, "SELECT version();")[0][0],
        "extension_sha256": hashlib.sha256(extension.read_bytes()).hexdigest(),
        "parameters": {
            key: str(value) if isinstance(value, Path) else value
            for key, value in vars(args).items()
        },
        "scope": "local synthetic shards; fresh database per scenario; OS cache not flushed; aggregate consumes every timestamp",
        "buffer_metric_scope": "cumulative connection high-water mark, including earlier variants and native query working memory",
        "video_sha256": (
            hashlib.sha256(args.video.read_bytes()).hexdigest() if args.video else None
        ),
        "scenarios": [],
    }
    with tempfile.TemporaryDirectory(prefix="lerobot-target-timestamps-") as directory:
        workspace = Path(directory)
        for shards in args.shards:
            root = workspace / f"shards-{shards}"
            fixture(cli, root, args.rows, shards, args.video)
            length = args.rows // shards
            for targets in args.targets:
                for pattern in args.patterns:
                    position = {
                        "dense": f"i % {args.rows}",
                        "sparse": f"(i * 7919) % {args.rows}",
                        "repeated": f"(i % {shards * 4}) * {max(1, length // 4)} % {args.rows}",
                    }[pattern]
                    requested = (
                        f"SELECT i::BIGINT request_id, p // {length} episode_index, p % {length} frame_index, "
                        f"'camera'::VARCHAR video_key, 0::BIGINT delta_index FROM "
                        f"(SELECT i, ({position})::BIGINT p FROM range({targets}) t(i))"
                    )
                    relation = (
                        f"lerobot_video_targets({quote(root)}, ({requested}), "
                        f"max_pending_targets := {args.batch_size}, decode_threads := {args.threads})"
                    )
                    queries = {
                        "native_join": f"SELECT count(*), sum(timestamp) FROM ({requested}) r LEFT JOIN "
                        f"read_parquet({quote(root / 'data/*.parquet')}) f USING (episode_index, frame_index)",
                        "metadata": f"SELECT count(*), sum(timestamp) FROM {relation}",
                    }
                    if args.video:
                        queries["decoded"] = (
                            f"SELECT count(*), sum(timestamp), sum(octet_length(image)) FROM {relation}"
                        )
                    sql = [
                        f"LOAD {quote(extension)};",
                        f"SET threads={args.threads};",
                        f"SET memory_limit={quote(args.memory_limit)};",
                        "SET enable_profiling='json';",
                    ]
                    outputs = []
                    for variant, query in queries.items():
                        for iteration in range(args.repeats + 1):
                            path = (
                                workspace
                                / f"{shards}-{targets}-{pattern}-{variant}-{iteration}.json"
                            )
                            outputs.append((variant, path))
                            sql += [f"SET profiling_output={quote(path)};", query + ";"]
                    if args.video:
                        sql += [
                            "PRAGMA disable_profiling;",
                            f"SELECT count(*), sum(timestamp), sum(octet_length(image)), "
                            f"sum(hash(request_id, episode_index, target_frame_index, timestamp, image)) FROM {relation};",
                        ]
                    results = run(cli, "\n".join(sql))
                    pixels = results.pop() if args.video else None
                    assert len(results) == len(outputs), results
                    assert all(row[:2] == results[0][:2] for row in results), results
                    scenario = dict(
                        shards=shards,
                        targets=targets,
                        pattern=pattern,
                        result=results[0][:2],
                    )
                    if pixels:
                        assert pixels[:2] == results[0][:2]
                        assert int(pixels[2]) > 0
                        scenario["untimed_pixel_validation"] = dict(
                            rows=pixels[0],
                            timestamps=pixels[1],
                            bytes=pixels[2],
                            hash_sum=pixels[3],
                        )
                    for variant in queries:
                        profiles = [
                            json.loads(path.read_text())
                            for name, path in outputs
                            if name == variant
                        ]
                        counters = [metrics(profile) for profile in profiles]
                        if variant == "metadata":
                            # DuckDB 1.5.5 does not call dynamic_to_string for
                            # table-in/out operators. Missing metrics stay
                            # unavailable, rather than being reported as zero.
                            assert all(
                                counter.get("LeRobot Decoder Opens", 0) == 0
                                for counter in counters
                            )
                        scenario[variant] = {
                            "first_seconds": profiles[0]["latency"],
                            "warm_median_seconds": statistics.median(
                                p["latency"] for p in profiles[1:]
                            ),
                            "peak_connection_buffer_bytes": max(
                                p["system_peak_buffer_memory"] for p in profiles
                            ),
                            "metrics": counters if any(counters) else None,
                        }
                    report["scenarios"].append(scenario)
                    print(
                        f"shards={shards} targets={targets} {pattern}: {scenario['metadata']['warm_median_seconds']:.6f}s",
                        flush=True,
                    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
