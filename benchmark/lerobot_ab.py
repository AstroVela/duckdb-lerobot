#!/usr/bin/env python3
"""Reproducible DuckDB/Daft/LeRobot video decode A/B benchmark.

Each invocation runs one engine in its native Python environment and writes a
self-describing JSON result. Keeping engines in separate processes avoids
dependency/version conflicts. Use the ``compare`` subcommand to verify pixel
hashes and print timing ratios.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import statistics
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable


def sql_string(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def machine_info() -> dict[str, Any]:
    return {
        "hostname": platform.node(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "python": platform.python_version(),
        "cpu_count": os.cpu_count(),
    }


def image_bytes_and_shape(value: Any) -> tuple[bytes, list[int]]:
    """Normalize PIL/NumPy/Torch images to contiguous uint8 HWC bytes."""
    import numpy as np

    if hasattr(value, "detach"):
        value = value.detach().cpu().numpy()
    array = np.asarray(value)
    if array.ndim != 3:
        raise ValueError(f"expected a 3-D image, received shape {array.shape}")
    if array.shape[0] in (1, 3, 4) and array.shape[-1] not in (1, 3, 4):
        array = np.moveaxis(array, 0, -1)
    if array.dtype != np.uint8:
        if np.issubdtype(array.dtype, np.floating):
            array = np.rint(np.clip(array, 0.0, 1.0) * 255.0).astype(np.uint8)
        else:
            array = array.astype(np.uint8)
    array = np.ascontiguousarray(array)
    return array.tobytes(), list(array.shape)


def make_row(
    episode_index: int, frame_index: int, video_key: str, image: Any
) -> dict[str, Any]:
    pixels, shape = image_bytes_and_shape(image)
    return {
        "episode_index": int(episode_index),
        "frame_index": int(frame_index),
        "video_key": video_key,
        "shape": shape,
        "sha256": hashlib.sha256(pixels).hexdigest(),
    }


def canonical_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return sorted(
        rows,
        key=lambda row: (row["episode_index"], row["frame_index"], row["video_key"]),
    )


def time_callable(
    function: Callable[[], list[dict[str, Any]]], warmups: int, repeats: int
) -> tuple[list[float], list[dict[str, Any]]]:
    for _ in range(warmups):
        function()
    durations: list[float] = []
    expected: list[dict[str, Any]] | None = None
    for _ in range(repeats):
        started = time.perf_counter()
        rows = canonical_rows(function())
        durations.append(time.perf_counter() - started)
        if expected is None:
            expected = rows
        elif rows != expected:
            raise RuntimeError(
                "one engine produced non-deterministic pixels between repeats"
            )
    return durations, expected or []


def find_profile_metrics(value: Any, output: dict[str, int]) -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            if key.startswith("LeRobot "):
                try:
                    output[key] = int(child)
                except (TypeError, ValueError):
                    pass
            find_profile_metrics(child, output)
    elif isinstance(value, list):
        for child in value:
            find_profile_metrics(child, output)


def run_duckdb(
    args: argparse.Namespace,
) -> tuple[float, Callable[[], list[dict[str, Any]]], dict[str, Any]]:
    import duckdb

    started = time.perf_counter()
    connection = duckdb.connect()
    if args.extension:
        connection.execute(f"LOAD {sql_string(str(Path(args.extension).resolve()))}")
    else:
        connection.execute("LOAD lerobot")
    setup_seconds = time.perf_counter() - started

    cameras_sql = ", ".join(
        f"({sql_string(camera)}, {index})" for index, camera in enumerate(args.camera)
    )
    resize_sql = ""
    if args.width or args.height:
        if not args.width or not args.height:
            raise ValueError("--width and --height must be specified together")
        resize_sql = f", width := {args.width}, height := {args.height}"
    root = sql_string(args.dataset)
    query = f"""
        WITH selected_frames AS (
            SELECT episode_index, frame_index
            FROM lerobot_frames({root})
            WHERE episode_index = {args.episode}
            ORDER BY frame_index
            LIMIT {args.rows}
        ), targets AS (
            SELECT row_number() OVER (ORDER BY frame_index, camera_ordinal) - 1 AS request_id,
                   episode_index, frame_index, video_key, 0::BIGINT AS delta_index
            FROM selected_frames
            CROSS JOIN (VALUES {cameras_sql}) cameras(video_key, camera_ordinal)
        )
        SELECT episode_index, frame_index, video_key, width, height, image
        FROM lerobot_video_targets(
            {root},
            (SELECT request_id, episode_index, frame_index, video_key, delta_index FROM targets),
            delta_timestamps := [0.0],
            tolerance := {args.tolerance},
            cluster_gap := {args.cluster_gap},
            decode_threads := {args.decode_threads},
            max_cached_decoders := {args.max_cached_decoders},
            max_pending_targets := {args.max_pending_targets},
            max_output_bytes := {args.max_output_bytes}
            {resize_sql}
        )
        ORDER BY episode_index, frame_index, video_key
    """

    def execute() -> list[dict[str, Any]]:
        result = connection.execute(query).fetchall()
        rows = []
        for episode_index, frame_index, video_key, width, height, image in result:
            expected_bytes = int(width) * int(height) * 3
            pixels = bytes(image)
            if len(pixels) != expected_bytes:
                raise RuntimeError(
                    f"DuckDB returned {len(pixels)} bytes, expected {expected_bytes}"
                )
            rows.append(
                {
                    "episode_index": int(episode_index),
                    "frame_index": int(frame_index),
                    "video_key": video_key,
                    "shape": [int(height), int(width), 3],
                    "sha256": hashlib.sha256(pixels).hexdigest(),
                }
            )
        return rows

    extra: dict[str, Any] = {
        "duckdb_version": duckdb.__version__,
        "query": query.strip(),
    }

    def collect_profile() -> None:
        frame_rows = connection.execute(
            f"SELECT frame_index FROM lerobot_frames({root}) "
            f"WHERE episode_index = {args.episode} ORDER BY frame_index LIMIT {args.rows}"
        ).fetchall()
        frame_indices = ", ".join(str(int(row[0])) for row in frame_rows)
        cameras = ", ".join(sql_string(camera) for camera in args.camera)
        profile_path = Path(args.output).with_suffix(".profile.json").resolve()
        connection.execute("PRAGMA enable_profiling='json'")
        connection.execute(f"SET profiling_output = {sql_string(str(profile_path))}")
        connection.execute(
            f"SELECT count(image) FROM lerobot_video_frames({root}, [{args.episode}], "
            f"video_keys := [{cameras}], frame_indices := [{frame_indices}], "
            f"tolerance := {args.tolerance}, cluster_gap := {args.cluster_gap}, "
            f"decode_threads := {args.decode_threads}, max_cached_decoders := {args.max_cached_decoders}, "
            f"max_pending_targets := {args.max_pending_targets}, max_output_bytes := {args.max_output_bytes}"
            f"{resize_sql})"
        ).fetchall()
        connection.execute("PRAGMA disable_profiling")
        profile = json.loads(profile_path.read_text())
        metrics: dict[str, int] = {}
        find_profile_metrics(profile, metrics)
        extra["profile_path"] = str(profile_path)
        extra["profile_metrics"] = metrics

    extra["collect_profile"] = collect_profile
    return setup_seconds, execute, extra


def run_daft(
    args: argparse.Namespace,
) -> tuple[float, Callable[[], list[dict[str, Any]]], dict[str, Any]]:
    import daft
    from daft.datasets import lerobot

    setup_seconds = 0.0

    def execute() -> list[dict[str, Any]]:
        frame = lerobot.read(args.dataset, load_video_frames=args.camera)
        frame = (
            frame.where(daft.col("episode_index") == args.episode)
            .sort("frame_index")
            .limit(args.rows)
        )
        data = frame.collect().to_pydict()
        rows: list[dict[str, Any]] = []
        for row_index in range(len(data["frame_index"])):
            for camera in args.camera:
                rows.append(
                    make_row(
                        data["episode_index"][row_index],
                        data["frame_index"][row_index],
                        camera,
                        data[camera][row_index],
                    )
                )
        return rows

    return (
        setup_seconds,
        execute,
        {"daft_version": getattr(daft, "__version__", "unknown")},
    )


def run_lerobot(
    args: argparse.Namespace,
) -> tuple[float, Callable[[], list[dict[str, Any]]], dict[str, Any]]:
    from lerobot.datasets.lerobot_dataset import LeRobotDataset

    started = time.perf_counter()
    dataset_kwargs: dict[str, Any] = {
        "episodes": [args.episode],
        "tolerance_s": args.tolerance,
        "video_backend": args.video_backend,
        "return_uint8": True,
    }
    if args.lerobot_root:
        dataset_kwargs["root"] = args.lerobot_root
    dataset = LeRobotDataset(args.dataset, **dataset_kwargs)
    setup_seconds = time.perf_counter() - started

    def scalar(value: Any) -> int:
        return int(value.item()) if hasattr(value, "item") else int(value)

    def execute() -> list[dict[str, Any]]:
        count = min(args.rows, len(dataset))
        if hasattr(dataset, "__getitems__"):
            items = dataset.__getitems__(list(range(count)))
        else:
            items = [dataset[index] for index in range(count)]
        rows: list[dict[str, Any]] = []
        for item in items:
            for camera in args.camera:
                rows.append(
                    make_row(
                        scalar(item["episode_index"]),
                        scalar(item["frame_index"]),
                        camera,
                        item[camera],
                    )
                )
        return rows

    return setup_seconds, execute, {"video_backend": args.video_backend}


def run_command(args: argparse.Namespace) -> int:
    adapters = {"duckdb": run_duckdb, "daft": run_daft, "lerobot": run_lerobot}
    setup_seconds, execute, extra = adapters[args.engine](args)
    collect_profile = extra.pop("collect_profile", None)
    durations, rows = time_callable(execute, args.warmups, args.repeats)
    if collect_profile is not None and not args.no_profile:
        collect_profile()
    expected_rows = args.rows * len(args.camera)
    if len(rows) != expected_rows:
        raise RuntimeError(
            f"expected {expected_rows} decoded rows, received {len(rows)}"
        )
    result = {
        "schema_version": 1,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "engine": args.engine,
        "dataset": args.dataset,
        "episode": args.episode,
        "requested_frame_rows": args.rows,
        "cameras": args.camera,
        "decoded_images": len(rows),
        "warmups": args.warmups,
        "repeats": args.repeats,
        "setup_seconds": setup_seconds,
        "durations_seconds": durations,
        "median_seconds": statistics.median(durations),
        "min_seconds": min(durations),
        "configuration": {
            "tolerance": args.tolerance,
            "cluster_gap": args.cluster_gap,
            "decode_threads": args.decode_threads,
            "max_cached_decoders": args.max_cached_decoders,
            "max_pending_targets": args.max_pending_targets,
            "max_output_bytes": args.max_output_bytes,
            "width": args.width,
            "height": args.height,
        },
        "machine": machine_info(),
        "rows": rows,
        **extra,
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(
        f"{args.engine}: {len(rows)} images, median {result['median_seconds']:.3f}s "
        f"({result['median_seconds'] / len(rows):.4f}s/image) -> {output}"
    )
    return 0


def compare_command(args: argparse.Namespace) -> int:
    results = [json.loads(Path(path).read_text()) for path in args.results]
    baseline = results[0]
    baseline_rows = baseline["rows"]
    mismatch = False
    for result in results[1:]:
        if result["rows"] != baseline_rows:
            mismatch = True
            left = {
                (row["episode_index"], row["frame_index"], row["video_key"]): row
                for row in baseline_rows
            }
            right = {
                (row["episode_index"], row["frame_index"], row["video_key"]): row
                for row in result["rows"]
            }
            different = sum(
                1 for key in set(left) | set(right) if left.get(key) != right.get(key)
            )
            print(
                f"PIXEL MISMATCH: {baseline['engine']} vs {result['engine']}: {different} rows",
                file=sys.stderr,
            )
    fastest = min(result["median_seconds"] for result in results)
    for result in results:
        print(
            f"{result['engine']:<8} {result['median_seconds']:>9.3f}s  "
            f"{result['median_seconds'] / fastest:>6.2f}x fastest  "
            f"{result['decoded_images']} images"
        )
    return 1 if mismatch else 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    run = subparsers.add_parser("run", help="run one engine and write a JSON result")
    run.add_argument("--engine", choices=("duckdb", "daft", "lerobot"), required=True)
    run.add_argument(
        "--dataset",
        required=True,
        help="local root, hf:// URI, or Hugging Face repo ID",
    )
    run.add_argument(
        "--lerobot-root",
        help="local root passed to LeRobotDataset while --dataset remains its repo ID",
    )
    run.add_argument(
        "--camera",
        action="append",
        required=True,
        help="repeat for every camera to decode",
    )
    run.add_argument("--episode", type=int, default=0)
    run.add_argument("--rows", type=int, default=16)
    run.add_argument("--warmups", type=int, default=1)
    run.add_argument("--repeats", type=int, default=3)
    run.add_argument("--output", required=True)
    run.add_argument(
        "--extension", help="path to lerobot.duckdb_extension for the DuckDB engine"
    )
    run.add_argument("--video-backend", choices=("pyav", "torchcodec"), default="pyav")
    run.add_argument("--tolerance", type=float, default=1e-4)
    run.add_argument("--cluster-gap", type=float, default=10.0)
    run.add_argument("--decode-threads", type=int, default=8)
    run.add_argument("--max-cached-decoders", type=int, default=8)
    run.add_argument("--max-pending-targets", type=int, default=4096)
    run.add_argument("--max-output-bytes", type=int, default=64 * 1024 * 1024)
    run.add_argument("--width", type=int, default=0)
    run.add_argument("--height", type=int, default=0)
    run.add_argument(
        "--no-profile", action="store_true", help="skip the extra DuckDB metrics pass"
    )
    run.set_defaults(function=run_command)

    compare = subparsers.add_parser(
        "compare", help="compare JSON results and require identical pixels"
    )
    compare.add_argument("results", nargs="+")
    compare.set_defaults(function=compare_command)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if getattr(args, "rows", 1) <= 0 or getattr(args, "repeats", 1) <= 0:
        raise SystemExit("--rows and --repeats must be positive")
    return args.function(args)


if __name__ == "__main__":
    raise SystemExit(main())
