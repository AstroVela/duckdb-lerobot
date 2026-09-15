#!/usr/bin/env python3
"""Create a compact local LeRobot v3 fixture for scheduler stress tests.

Every logical video shard is a hard link to one source MP4, so a fixture can
exercise shard grouping, decoder concurrency, and LRU eviction without storing
many duplicate copies of the video bytes.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
from pathlib import Path


def sql_string(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def sql_identifier(value: str) -> str:
    return '"' + value.replace('"', '""') + '"'


def video_duration(path: Path) -> float:
    result = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-show_entries",
            "format=duration",
            "-of",
            "default=noprint_wrappers=1:nokey=1",
            str(path),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return float(result.stdout.strip())


def write_parquet_metadata(args: argparse.Namespace, output: Path) -> None:
    data_path = output / "data/chunk-000/file-000.parquet"
    episodes_path = output / "meta/episodes/chunk-000/file-000.parquet"
    camera_prefix = f"videos/{args.camera}"
    data_sql = f"""
        COPY (
            SELECT episode::BIGINT AS episode_index,
                   frame::BIGINT AS frame_index,
                   CAST(frame::DOUBLE / {args.fps} AS FLOAT) AS timestamp,
                   (episode * {args.frames_per_shard} + frame)::BIGINT AS index,
                   0::BIGINT AS task_index
            FROM range({args.shards}) episodes(episode)
            CROSS JOIN range({args.frames_per_shard}) frames(frame)
            ORDER BY episode_index, frame_index
        ) TO {sql_string(str(data_path))} (FORMAT PARQUET)
    """
    episodes_sql = f"""
        COPY (
            SELECT episode::BIGINT AS episode_index,
                   {args.frames_per_shard}::BIGINT AS length,
                   0::BIGINT AS {sql_identifier("data/chunk_index")},
                   0::BIGINT AS {sql_identifier("data/file_index")},
                   (episode * {args.frames_per_shard})::BIGINT AS dataset_from_index,
                   ((episode + 1) * {args.frames_per_shard})::BIGINT AS dataset_to_index,
                   0::BIGINT AS {sql_identifier(camera_prefix + "/chunk_index")},
                   episode::BIGINT AS {sql_identifier(camera_prefix + "/file_index")},
                   0.0::DOUBLE AS {sql_identifier(camera_prefix + "/from_timestamp")},
                   ({args.frames_per_shard}::DOUBLE / {args.fps}) AS
                       {sql_identifier(camera_prefix + "/to_timestamp")}
            FROM range({args.shards}) episodes(episode)
            ORDER BY episode_index
        ) TO {sql_string(str(episodes_path))} (FORMAT PARQUET)
    """
    subprocess.run([str(args.duckdb_cli), "-c", data_sql], check=True)
    subprocess.run([str(args.duckdb_cli), "-c", episodes_sql], check=True)


def write_info(args: argparse.Namespace, output: Path) -> None:
    info = {
        "codebase_version": "v3.0",
        "robot_type": "duckdb-lerobot-benchmark",
        "total_episodes": args.shards,
        "total_frames": args.shards * args.frames_per_shard,
        "total_tasks": 0,
        "total_videos": args.shards,
        "total_chunks": 1,
        "chunks_size": 1000,
        "fps": args.fps,
        "splits": {"train": f"0:{args.shards}"},
        "data_path": "data/chunk-{chunk_index:03d}/file-{file_index:03d}.parquet",
        "video_path": (
            "videos/{video_key}/chunk-{chunk_index:03d}/file-{file_index:03d}.mp4"
        ),
        "features": {
            args.camera: {
                "dtype": "video",
                "shape": [args.height, args.width, 3],
                "names": ["height", "width", "rgb"],
                "info": {
                    "video.height": args.height,
                    "video.width": args.width,
                    "video.codec": args.codec,
                    "video.pix_fmt": "yuv420p",
                    "is_depth_map": False,
                    "video.fps": args.fps,
                    "video.channels": 3,
                    "has_audio": False,
                },
            },
            "timestamp": {"dtype": "float32", "shape": [1], "names": None},
            "frame_index": {"dtype": "int64", "shape": [1], "names": None},
            "episode_index": {"dtype": "int64", "shape": [1], "names": None},
            "index": {"dtype": "int64", "shape": [1], "names": None},
            "task_index": {"dtype": "int64", "shape": [1], "names": None},
        },
    }
    (output / "meta/info.json").write_text(
        json.dumps(info, indent=2, sort_keys=True) + "\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb-cli", type=Path, required=True)
    parser.add_argument("--source-video", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--camera", default="observation.image")
    parser.add_argument("--shards", type=int, default=20)
    parser.add_argument("--frames-per-shard", type=int, default=500)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--codec", default="av1")
    args = parser.parse_args()

    if args.shards <= 0 or args.frames_per_shard <= 0 or args.fps <= 0:
        parser.error("--shards, --frames-per-shard, and --fps must be positive")
    source = args.source_video.resolve()
    cli = args.duckdb_cli.resolve()
    output = args.output.resolve()
    if not source.is_file():
        parser.error(f"source video does not exist: {source}")
    if not cli.is_file():
        parser.error(f"DuckDB CLI does not exist: {cli}")
    required_duration = (args.frames_per_shard - 1) / args.fps
    duration = video_duration(source)
    if duration + 1e-4 < required_duration:
        parser.error(
            f"source duration {duration:.6f}s is shorter than required "
            f"target timestamp {required_duration:.6f}s"
        )
    if output.exists():
        parser.error(f"output already exists: {output}")

    args.duckdb_cli = cli
    (output / "data/chunk-000").mkdir(parents=True)
    (output / "meta/episodes/chunk-000").mkdir(parents=True)
    video_dir = output / "videos" / args.camera / "chunk-000"
    video_dir.mkdir(parents=True)
    write_info(args, output)
    write_parquet_metadata(args, output)
    used_hard_links = True
    for shard in range(args.shards):
        target = video_dir / f"file-{shard:03d}.mp4"
        try:
            os.link(source, target)
        except OSError:
            used_hard_links = False
            shutil.copy2(source, target)

    print(
        f"created {args.shards} shards x {args.frames_per_shard} frames "
        f"at {output} (hard_links={used_hard_links})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
