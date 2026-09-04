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
import tempfile
import time
from pathlib import Path
from statistics import median
from typing import Any

SCHEMA_VERSION = 1


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


def process_counters(pid: int) -> dict[str, int]:
    result: dict[str, int] = {}
    try:
        for line in Path(f"/proc/{pid}/io").read_text().splitlines():
            key, value = line.split(":", 1)
            result[key] = int(value.strip())
    except (OSError, ValueError):
        pass
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmHWM:"):
                result["peak_rss_bytes"] = int(line.split()[1]) * 1024
                break
    except (OSError, ValueError, IndexError):
        pass
    return result


def run_sql_measured(cli: Path, sql: str) -> tuple[float, dict[str, int]]:
    started = time.perf_counter()
    with tempfile.TemporaryFile(mode="w+t") as sql_input, tempfile.TemporaryFile(
        mode="w+t"
    ) as process_output:
        sql_input.write(sql)
        sql_input.seek(0)
        process = subprocess.Popen(
            [str(cli), "-batch"],
            stdin=sql_input,
            stdout=process_output,
            stderr=subprocess.STDOUT,
            text=True,
        )
        counters: dict[str, int] = {}
        try:
            while True:
                for key, value in process_counters(process.pid).items():
                    counters[key] = max(value, counters.get(key, 0))
                if process.poll() is not None:
                    break
                time.sleep(0.01)
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


def query_csv(cli: Path, sql: str) -> list[list[str]]:
    result = subprocess.run(
        [str(cli), "-noheader", "-csv", "-c", sql],
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
    )
    if route_rows != [[str(episodes), "true"]]:
        raise RuntimeError(f"invalid video routes: {route_rows}")

    file_rows = query_csv(
        cli,
        f"SELECT count(*) FROM glob(" f"{sql_string(str(root / 'videos/**/*.mp4'))})",
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
    args.work_dir.mkdir(parents=True, exist_ok=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.run_dir = Path(
        tempfile.mkdtemp(prefix="duckdb-lerobot-write-", dir=args.work_dir)
    )

    result: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "machine": {
            "platform": platform.platform(),
            "processor": platform.processor(),
            "cpu_count": os.cpu_count(),
        },
        "configuration": {
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
    small_rchar = median(
        sample["process_io"].get("rchar", 0) for sample in small_samples
    )
    small_wchar = median(
        sample["process_io"].get("wchar", 0) for sample in small_samples
    )
    result["episode_scaling"] = {
        "small_episodes": args.episodes,
        "large_episodes": args.episodes * 2,
        "small_rchar_median": small_rchar,
        "large_rchar": larger["process_io"].get("rchar", 0),
        "rchar_ratio": (
            larger["process_io"].get("rchar", 0) / small_rchar if small_rchar else None
        ),
        "small_wchar_median": small_wchar,
        "large_wchar": larger["process_io"].get("wchar", 0),
        "wchar_ratio": (
            larger["process_io"].get("wchar", 0) / small_wchar if small_wchar else None
        ),
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
