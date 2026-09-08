#!/usr/bin/env python3
"""Benchmark bounded multi-camera COPY and one-pass video shard assembly."""

from __future__ import annotations

import argparse
import csv
import io
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from statistics import median
from typing import Any

SCHEMA_VERSION = 2


def sql_string(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def sql_identifier(value: str) -> str:
    return '"' + value.replace('"', '""') + '"'


def camera_names(camera_count: int) -> list[str]:
    return [f"observation.images.camera_{index}" for index in range(camera_count)]


def feature_json(cameras: list[str], height: int, width: int) -> str:
    return json.dumps(
        {
            camera: {
                "dtype": "video",
                "shape": [height, width, 3],
                "names": ["height", "width", "channels"],
            }
            for camera in cameras
        },
        separators=(",", ":"),
    )


def copy_sql(
    destination: Path,
    camera_count: int,
    episodes: int,
    frames_per_episode: int,
    fps: int,
    height: int,
    width: int,
    duckdb_threads: int,
    video_workers: int,
) -> str:
    cameras = camera_names(camera_count)
    pixels = height * width
    camera_columns = []
    for index, camera in enumerate(cameras):
        # Multiplication by an odd number permutes the 24-bit color space. The
        # combined frame/camera ordinal therefore yields deterministic, widely
        # separated colors without repeating within realistic benchmark sizes.
        color = (
            f"((row_index * {camera_count} + {index}) * 2654435761 + 104729) "
            "% 16777216"
        )
        camera_columns.append(
            f"from_hex(repeat(printf('%06x', ({color})::UBIGINT), {pixels})) "
            f"AS {sql_identifier(camera)}"
        )
    columns = ",\n               ".join(camera_columns)
    return f"""
SET threads={duckdb_threads};
COPY (
    SELECT floor(row_index / {frames_per_episode})::BIGINT AS episode_index,
           'write benchmark'::VARCHAR AS task,
           {columns}
    FROM range({episodes * frames_per_episode}) frames(row_index)
    ORDER BY episode_index, row_index
) TO {sql_string(str(destination))} (
    FORMAT lerobot,
    FPS {fps},
    FEATURES {sql_string(feature_json(cameras, height, width))},
    VIDEO_FILES_SIZE_IN_MB 1000000,
    MAX_VISUAL_FRAME_BYTES {height * width * 3},
    VIDEO_WORKERS {video_workers},
    ENCODER_THREADS {duckdb_threads}
);
"""


def run_sql_measured(
    cli: Path, sql: str, extension: Path | None = None
) -> tuple[float, dict[str, int | None]]:
    started = time.perf_counter()
    with tempfile.TemporaryFile(mode="w+t") as sql_input, tempfile.TemporaryFile(
        mode="w+t"
    ) as process_output:
        if extension:
            sql_input.write(f"LOAD {sql_string(str(extension))};\n")
        sql_input.write(sql)
        sql_input.seek(0)
        process = subprocess.Popen(
            [str(cli), *(["-unsigned"] if extension else []), "-batch"],
            stdin=sql_input,
            stdout=process_output,
            stderr=subprocess.STDOUT,
            text=True,
        )
        counters: dict[str, int | None] = {
            "fs_input_blocks": None,
            "fs_output_blocks": None,
            "peak_rss_bytes": None,
        }
        try:
            if hasattr(os, "wait4"):
                # Reap this exact process once. Sampling /proc can miss the
                # final writes, and a zombie's counters may be inaccessible.
                # These are filesystem block counters, not rchar/wchar bytes.
                _, status, usage = os.wait4(process.pid, 0)
                process.returncode = os.waitstatus_to_exitcode(status)
                counters["fs_input_blocks"] = usage.ru_inblock
                counters["fs_output_blocks"] = usage.ru_oublock
                if sys.platform == "linux":
                    counters["peak_rss_bytes"] = usage.ru_maxrss * 1024
                elif sys.platform == "darwin":
                    counters["peak_rss_bytes"] = usage.ru_maxrss
            else:
                process.wait()
        except BaseException:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            raise
        elapsed = time.perf_counter() - started
        if process.returncode:
            process_output.seek(0)
            output = process_output.read()
            raise RuntimeError(f"DuckDB exited with {process.returncode}:\n{output}")
    return elapsed, counters


def io_scaling(small_samples: list[dict], larger: dict) -> dict:
    result = {}
    for key in ("fs_input_blocks", "fs_output_blocks"):
        values = [sample["process_io"].get(key) for sample in small_samples]
        small = (
            median(values)
            if values and all(value is not None for value in values)
            else None
        )
        large = larger["process_io"].get(key)
        result[f"small_{key}_median"] = small
        result[f"large_{key}"] = large
        result[f"{key}_ratio"] = large / small if small and large is not None else None
    return result


def query_csv(cli: Path, sql: str, extension: Path | None = None) -> list[list[str]]:
    if extension:
        sql = f"LOAD {sql_string(str(extension))};\n" + sql
    result = subprocess.run(
        [
            str(cli),
            *(["-unsigned"] if extension else []),
            "-noheader",
            "-csv",
            "-c",
            sql,
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return list(csv.reader(io.StringIO(result.stdout)))


def validate_dataset(
    cli: Path,
    root: Path,
    camera_count: int,
    episodes: int,
    frames_per_episode: int,
    fps: int,
    height: int,
    width: int,
    extension: Path | None = None,
) -> list[list[str]]:
    cameras = camera_names(camera_count)
    episode_duration = frames_per_episode / fps
    route_checks = []
    for camera in cameras:
        prefix = f"videos/{camera}"
        route_checks.append(
            f"abs({sql_identifier(prefix + '/from_timestamp')} - "
            f"episode_index * {episode_duration}) < 1e-6"
        )
        route_checks.append(
            f"abs({sql_identifier(prefix + '/to_timestamp')} - "
            f"(episode_index + 1) * {episode_duration}) < 1e-6"
        )
    route_rows = query_csv(
        cli,
        f"SELECT count(*), bool_and({' AND '.join(route_checks)}) "
        f"FROM lerobot_episodes({sql_string(str(root))})",
        extension,
    )
    if route_rows != [[str(episodes), "true"]]:
        raise RuntimeError(f"invalid video routes: {route_rows}")

    file_rows = query_csv(
        cli,
        f"SELECT count(*) FROM glob(" f"{sql_string(str(root / 'videos/**/*.mp4'))})",
        extension,
    )
    if file_rows != [[str(camera_count)]]:
        raise RuntimeError(f"expected one shard per camera: {file_rows}")

    camera_values = ",".join(sql_string(camera) for camera in cameras)
    episode_indices = ",".join(str(index) for index in range(episodes))
    frame_indices = ",".join(str(index) for index in range(frames_per_episode))
    decoded = query_csv(
        cli,
        f"SELECT episode_index, frame_index, video_key, width, height, channels, "
        f"octet_length(image), md5(image) FROM lerobot_video_frames("
        f"{sql_string(str(root))}, [{episode_indices}], "
        f"video_keys := [{camera_values}], "
        f"frame_indices := [{frame_indices}]) "
        f"ORDER BY episode_index, frame_index, video_key",
        extension,
    )
    expected_rows = camera_count * episodes * frames_per_episode
    if len(decoded) != expected_rows:
        raise RuntimeError(
            f"expected {expected_rows} decoded samples, got {len(decoded)}"
        )
    for row in decoded:
        if row[3:7] != [str(width), str(height), "3", str(width * height * 3)]:
            raise RuntimeError(f"invalid decoded frame contract: {row}")
    return [[row[0], row[1], row[2], row[7]] for row in decoded]


def directory_bytes(root: Path) -> int:
    return sum(path.stat().st_size for path in root.rglob("*") if path.is_file())


def run_case(
    args: argparse.Namespace,
    camera_count: int,
    episodes: int,
    video_workers: int,
    label: str,
    repeat: int,
) -> tuple[dict[str, Any], list[list[str]]]:
    destination = args.run_dir / (f"{camera_count}cam-{episodes}ep-{label}-{repeat}")
    if destination.exists():
        raise RuntimeError(f"benchmark destination already exists: {destination}")
    elapsed, counters = run_sql_measured(
        args.duckdb_cli,
        copy_sql(
            destination,
            camera_count,
            episodes,
            args.frames_per_episode,
            args.fps,
            args.height,
            args.width,
            args.threads,
            video_workers,
        ),
        args.extension,
    )
    signature = validate_dataset(
        args.duckdb_cli,
        destination,
        camera_count,
        episodes,
        args.frames_per_episode,
        args.fps,
        args.height,
        args.width,
        args.extension,
    )
    measurement = {
        "seconds": elapsed,
        "dataset_bytes": directory_bytes(destination),
        "process_io": counters,
    }
    if not args.keep_output:
        shutil.rmtree(destination)
    return measurement, signature


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb-cli", type=Path, required=True)
    parser.add_argument(
        "--extension",
        type=Path,
        help="explicit local extension to load in every CLI process",
    )
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--episodes", type=int, default=8)
    parser.add_argument("--frames-per-episode", type=int, default=30)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--width", type=int, default=128)
    parser.add_argument("--height", type=int, default=128)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--keep-output", action="store_true")
    args = parser.parse_args()

    positive = (
        args.episodes,
        args.frames_per_episode,
        args.fps,
        args.width,
        args.height,
        args.threads,
        args.repeats,
    )
    if any(value <= 0 for value in positive):
        parser.error("all numeric arguments must be positive")
    if args.episodes < 2 or args.frames_per_episode < 2:
        parser.error("--episodes and --frames-per-episode must be at least 2")
    args.duckdb_cli = args.duckdb_cli.resolve()
    args.work_dir = args.work_dir.resolve()
    args.output = args.output.resolve()
    if not args.duckdb_cli.is_file():
        parser.error(f"DuckDB CLI does not exist: {args.duckdb_cli}")
    if args.extension:
        args.extension = args.extension.resolve()
        if not args.extension.is_file():
            parser.error(f"extension does not exist: {args.extension}")
    args.work_dir.mkdir(parents=True, exist_ok=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.run_dir = Path(
        tempfile.mkdtemp(prefix="duckdb-lerobot-write-", dir=args.work_dir)
    )

    result: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "measurement": {
            "method": "wait4" if hasattr(os, "wait4") else "wall_time_only",
            "scope": "whole CLI process, including COPY finalize and shutdown; excludes validation",
            "io_units": "kernel filesystem block counters; cache-dependent, not logical read/write bytes",
            "unavailable": "null; ratios are also null for zero denominators",
        },
        "machine": {
            "platform": platform.platform(),
            "processor": platform.processor(),
            "cpu_count": os.cpu_count(),
        },
        "configuration": {
            "duckdb_cli": str(args.duckdb_cli),
            "extension": str(args.extension) if args.extension else None,
            "episodes": args.episodes,
            "frames_per_episode": args.frames_per_episode,
            "fps": args.fps,
            "width": args.width,
            "height": args.height,
            "duckdb_threads": args.threads,
            "repeats": args.repeats,
        },
        "camera_cases": {},
    }

    for camera_count in (2, 4):
        case: dict[str, Any] = {"serial": [], "parallel": []}
        baseline_signature: list[list[str]] | None = None
        for repeat in range(args.repeats):
            serial, signature = run_case(
                args, camera_count, args.episodes, 1, "serial", repeat
            )
            parallel, parallel_signature = run_case(
                args,
                camera_count,
                args.episodes,
                min(camera_count, args.threads),
                "parallel",
                repeat,
            )
            if signature != parallel_signature:
                raise RuntimeError(
                    f"decoded pixels differ for {camera_count} cameras at repeat {repeat}"
                )
            if baseline_signature is not None and signature != baseline_signature:
                raise RuntimeError("decoded pixel signature changed between repeats")
            baseline_signature = signature
            case["serial"].append(serial)
            case["parallel"].append(parallel)
        serial_median = median(item["seconds"] for item in case["serial"])
        parallel_median = median(item["seconds"] for item in case["parallel"])
        case["serial_median_seconds"] = serial_median
        case["parallel_median_seconds"] = parallel_median
        case["speedup"] = serial_median / parallel_median
        result["camera_cases"][str(camera_count)] = case

    larger, _ = run_case(
        args,
        4,
        args.episodes * 2,
        min(4, args.threads),
        "scaling",
        0,
    )
    small_samples = result["camera_cases"]["4"]["parallel"]
    result["episode_scaling"] = {
        "small_episodes": args.episodes,
        "large_episodes": args.episodes * 2,
        **io_scaling(small_samples, larger),
    }

    if not args.keep_output:
        args.run_dir.rmdir()
    else:
        result["generated_datasets"] = str(args.run_dir)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
