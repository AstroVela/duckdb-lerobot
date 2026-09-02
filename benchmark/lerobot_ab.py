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
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable


def sql_string(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def machine_info() -> dict[str, Any]:
    result = {
        "hostname": platform.node(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "python": platform.python_version(),
        "cpu_count": os.cpu_count(),
    }
    try:
        cpuinfo = Path("/proc/cpuinfo").read_text()
        model = next(
            line.split(":", 1)[1].strip()
            for line in cpuinfo.splitlines()
            if line.startswith("model name")
        )
        physical_cores = {
            (block_physical, block_core)
            for block in cpuinfo.split("\n\n")
            if (block_physical := _cpuinfo_field(block, "physical id")) is not None
            if (block_core := _cpuinfo_field(block, "core id")) is not None
        }
        result["cpu_model"] = model
        if physical_cores:
            result["physical_cpu_count"] = len(physical_cores)
    except (OSError, StopIteration):
        pass
    try:
        result["memory_total_bytes"] = os.sysconf("SC_PAGE_SIZE") * os.sysconf(
            "SC_PHYS_PAGES"
        )
    except (OSError, ValueError):
        pass
    return result


def _cpuinfo_field(block: str, name: str) -> str | None:
    prefix = name + "\t"
    for line in block.splitlines():
        if line.startswith(prefix):
            return line.split(":", 1)[1].strip()
    return None


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
    function: Callable[[], dict[str, int]], warmups: int, repeats: int
) -> tuple[list[float], dict[str, int]]:
    for _ in range(warmups):
        function()
    durations: list[float] = []
    expected: dict[str, int] | None = None
    for _ in range(repeats):
        started = time.perf_counter()
        summary = function()
        durations.append(time.perf_counter() - started)
        if expected is None:
            expected = summary
        elif summary != expected:
            raise RuntimeError(
                "one engine produced a non-deterministic materialization summary "
                "between repeats"
            )
    return durations, expected or {}


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


def selection_query(args: argparse.Namespace, root: str) -> str:
    episode_filter = (
        "" if args.all_episodes else f"WHERE episode_index = {args.episode}"
    )
    return (
        "SELECT episode_index, frame_index "
        f"FROM lerobot_frames({root}) {episode_filter} "
        "ORDER BY episode_index, frame_index "
        f"LIMIT {args.rows}"
    )


def selected_frames_values(selected_rows: list[dict[str, Any]]) -> str:
    if not selected_rows:
        raise RuntimeError("frame selection returned no rows")
    return ", ".join(
        f"({int(row['episode_index'])}, {int(row['frame_index'])})"
        for row in selected_rows
    )


def duckdb_decode_query(
    args: argparse.Namespace,
    selected_rows: list[dict[str, Any]],
    hash_images: bool,
    order_output: bool,
) -> str:
    cameras_sql = ", ".join(
        f"({sql_string(camera)}, {index})" for index, camera in enumerate(args.camera)
    )
    resize_sql = ""
    if args.width or args.height:
        if not args.width or not args.height:
            raise ValueError("--width and --height must be specified together")
        resize_sql = f", width := {args.width}, height := {args.height}"
    root = sql_string(args.dataset)
    image_sql = "sha256(image) AS sha256" if hash_images else "image"
    order_sql = "ORDER BY episode_index, frame_index, video_key" if order_output else ""
    return f"""
        WITH selected_frames(episode_index, frame_index) AS (
            VALUES {selected_frames_values(selected_rows)}
        ), targets AS (
            SELECT row_number() OVER (
                       ORDER BY episode_index, frame_index, camera_ordinal
                   ) - 1 AS request_id,
                   episode_index, frame_index, video_key, 0::BIGINT AS delta_index
            FROM selected_frames
            CROSS JOIN (VALUES {cameras_sql}) cameras(video_key, camera_ordinal)
        )
        SELECT episode_index, frame_index, video_key, width, height, {image_sql}
        FROM lerobot_video_targets(
            {root},
            (SELECT request_id, episode_index, frame_index, video_key, delta_index FROM targets),
            delta_timestamps := [0.0],
            tolerance := {args.tolerance},
            cluster_gap := {args.cluster_gap},
            batch_size := {args.batch_size},
            target_buffer_size := {args.target_buffer_size},
            decode_threads := {args.decode_threads},
            max_cached_decoders := {args.max_cached_decoders},
            max_pending_targets := {args.max_pending_targets},
            max_output_bytes := {args.max_output_bytes},
            codec_threads := {args.codec_threads}
            {resize_sql}
        )
        {order_sql}
    """


def duckdb_timed_query(
    args: argparse.Namespace, selected_rows: list[dict[str, Any]]
) -> str:
    decode_query = duckdb_decode_query(
        args, selected_rows, hash_images=False, order_output=False
    )
    return f"""
        SELECT count(image) AS decoded_images,
               coalesce(sum(octet_length(image)), 0) AS decoded_bytes
        FROM ({decode_query}) decoded
    """


def source_profile_queries(
    args: argparse.Namespace, selected_rows: list[dict[str, Any]]
) -> list[tuple[str, str]]:
    by_episode: dict[int, list[int]] = {}
    for row in selected_rows:
        by_episode.setdefault(int(row["episode_index"]), []).append(
            int(row["frame_index"])
        )
    cameras = ", ".join(sql_string(camera) for camera in args.camera)
    resize_sql = ""
    if args.width and args.height:
        resize_sql = f", width := {args.width}, height := {args.height}"
    root = sql_string(args.dataset)
    groups: list[tuple[list[int], list[int]]]
    frame_sets = {tuple(frame_indices) for frame_indices in by_episode.values()}
    if len(frame_sets) == 1 and by_episode:
        groups = [(list(by_episode), next(iter(by_episode.values())))]
    else:
        groups = [([episode], frames) for episode, frames in by_episode.items()]
    queries = []
    for episodes, frame_indices in groups:
        frames = ", ".join(str(frame_index) for frame_index in frame_indices)
        episode_sql = ", ".join(str(episode) for episode in episodes)
        query = (
            f"SELECT count(image) FROM lerobot_video_frames({root}, [{episode_sql}], "
            f"video_keys := [{cameras}], frame_indices := [{frames}], "
            f"tolerance := {args.tolerance}, cluster_gap := {args.cluster_gap}, "
            f"batch_size := {args.batch_size}, "
            f"target_buffer_size := {args.target_buffer_size}, "
            f"decode_threads := {args.decode_threads}, "
            f"max_cached_decoders := {args.max_cached_decoders}, "
            f"max_pending_targets := {args.max_pending_targets}, "
            f"max_output_bytes := {args.max_output_bytes}, "
            f"codec_threads := {args.codec_threads}{resize_sql})"
        )
        label = "episodes-" + "-".join(str(episode) for episode in episodes)
        queries.append((label, query))
    return queries


class DuckDBCLIConnection:
    """Small persistent JSON protocol for benchmarking a DuckDB shell build."""

    def __init__(self, executable: str, allow_unsigned: bool = False):
        self.executable = str(Path(executable).resolve())
        self.counter = 0
        command = [self.executable, "-batch"]
        if allow_unsigned:
            command.insert(1, "-unsigned")
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        if self.process.stdin is None or self.process.stdout is None:
            raise RuntimeError("failed to open DuckDB CLI pipes")
        self.process.stdin.write(".mode json\n.bail off\n")
        self.process.stdin.flush()
        self.execute("SELECT 1 AS ready")

    def execute(self, sql: str) -> list[dict[str, Any]]:
        if self.process.poll() is not None:
            raise RuntimeError(
                f"DuckDB CLI exited with status {self.process.returncode}"
            )
        self.counter += 1
        marker = f"__LEROBOT_BENCHMARK_DONE_{self.counter}__"
        statement = sql.rstrip().rstrip(";") + ";"
        marker_query = f"SELECT {sql_string(marker)} AS __lerobot_benchmark_marker;\n"
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        self.process.stdin.write(statement + "\n" + marker_query)
        self.process.stdin.flush()

        result_sets: list[list[dict[str, Any]]] = []
        diagnostics: list[str] = []
        json_buffer = ""
        while True:
            line = self.process.stdout.readline()
            if line == "":
                raise RuntimeError(
                    "DuckDB CLI closed its output before the benchmark marker; "
                    + "\n".join(diagnostics)
                )
            stripped = line.strip()
            if not stripped:
                continue
            if not json_buffer and not (stripped.startswith("[{") or stripped == "[]"):
                diagnostics.append(stripped)
                continue
            json_buffer += stripped
            try:
                parsed = json.loads(json_buffer)
            except json.JSONDecodeError:
                continue
            json_buffer = ""
            if (
                isinstance(parsed, list)
                and len(parsed) == 1
                and parsed[0].get("__lerobot_benchmark_marker") == marker
            ):
                break
            if isinstance(parsed, list):
                result_sets.append(parsed)
            else:
                diagnostics.append(stripped)

        errors = [line for line in diagnostics if "error" in line.lower()]
        if errors:
            raise RuntimeError("DuckDB CLI query failed:\n" + "\n".join(diagnostics))
        return result_sets[-1] if result_sets else []

    def close(self) -> None:
        if self.process.poll() is not None:
            return
        assert self.process.stdin is not None
        self.process.stdin.write(".quit\n")
        self.process.stdin.flush()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(timeout=5)


def configure_duckdb_connection(
    args: argparse.Namespace, execute: Callable[[str], Any]
) -> None:
    extensions = list(args.duckdb_load)
    if args.dataset.startswith(("hf://", "http://", "https://", "s3://")):
        if "httpfs" not in extensions:
            extensions.insert(0, "httpfs")
    for extension in extensions:
        execute(f"LOAD {sql_string(extension)}")
    if args.extension:
        execute(f"LOAD {sql_string(str(Path(args.extension).resolve()))}")
    else:
        execute("LOAD lerobot")


def run_duckdb_python(
    args: argparse.Namespace,
) -> tuple[
    float,
    Callable[[], dict[str, int]],
    Callable[[], list[dict[str, Any]]],
    dict[str, Any],
]:
    import duckdb

    started = time.perf_counter()
    connection = duckdb.connect()
    configure_duckdb_connection(args, connection.execute)
    version_row = connection.execute("PRAGMA version").fetchone()
    root = sql_string(args.dataset)
    selection_started = time.perf_counter()
    selected = connection.execute(selection_query(args, root)).fetchall()
    selection_seconds = time.perf_counter() - selection_started
    selected_rows = [
        {"episode_index": int(row[0]), "frame_index": int(row[1])} for row in selected
    ]
    if len(selected_rows) != args.rows:
        raise RuntimeError(
            f"requested {args.rows} frame rows, but selected {len(selected_rows)}"
        )
    timed_query = duckdb_timed_query(args, selected_rows)
    validation_query = duckdb_decode_query(
        args, selected_rows, hash_images=True, order_output=True
    )
    setup_seconds = time.perf_counter() - started

    def execute() -> dict[str, int]:
        decoded_images, decoded_bytes = connection.execute(timed_query).fetchone()
        return {
            "frame_rows": int(decoded_images) // len(args.camera),
            "decoded_images": int(decoded_images),
            "decoded_bytes": int(decoded_bytes),
        }

    def validate() -> list[dict[str, Any]]:
        result = connection.execute(validation_query).fetchall()
        return [
            {
                "episode_index": int(episode_index),
                "frame_index": int(frame_index),
                "video_key": video_key,
                "shape": [int(height), int(width), 3],
                "sha256": sha256,
            }
            for episode_index, frame_index, video_key, width, height, sha256 in result
        ]

    extra: dict[str, Any] = {
        "duckdb_version": str(version_row[0]),
        "duckdb_source_id": str(version_row[1]),
        "duckdb_python_version": duckdb.__version__,
        "selection_seconds": selection_seconds,
        "selection_strategy": "metadata-only lerobot_frames query",
        "timed_query": timed_query.strip(),
        "validation_query": validation_query.strip(),
        "timing_boundary": (
            "DuckDB executes lerobot_video_targets and consumes every BLOB with "
            "octet_length; BLOB transfer and SHA-256 are excluded"
        ),
        "validation_boundary": (
            "DuckDB replays the target query, hashes RGB BLOBs with SQL sha256, "
            "and transfers only keys, shapes, and digests"
        ),
    }

    def collect_profile() -> None:
        metrics: dict[str, int] = {}
        paths = []
        for label, profile_query in source_profile_queries(args, selected_rows):
            profile_path = (
                Path(args.output).with_suffix(f".{label}.profile.json").resolve()
            )
            connection.execute("PRAGMA enable_profiling='json'")
            connection.execute(
                f"SET profiling_output = {sql_string(str(profile_path))}"
            )
            connection.execute(profile_query).fetchall()
            connection.execute("PRAGMA disable_profiling")
            episode_metrics: dict[str, int] = {}
            find_profile_metrics(json.loads(profile_path.read_text()), episode_metrics)
            for key, value in episode_metrics.items():
                metrics[key] = metrics.get(key, 0) + value
            paths.append(str(profile_path))
        extra["profile_paths"] = paths
        extra["profile_scope"] = (
            "one source-function query per compatible selected-episode group"
        )
        extra["profile_metrics"] = metrics

    extra["collect_profile"] = collect_profile
    extra["close"] = connection.close
    return setup_seconds, execute, validate, extra


def run_duckdb_cli(
    args: argparse.Namespace,
) -> tuple[
    float,
    Callable[[], dict[str, int]],
    Callable[[], list[dict[str, Any]]],
    dict[str, Any],
]:
    started = time.perf_counter()
    connection = DuckDBCLIConnection(
        args.duckdb_cli, allow_unsigned=bool(args.extension)
    )
    configure_duckdb_connection(args, connection.execute)
    version_rows = connection.execute("PRAGMA version")
    root = sql_string(args.dataset)
    selection_started = time.perf_counter()
    selected_rows = connection.execute(selection_query(args, root))
    selection_seconds = time.perf_counter() - selection_started
    selected_rows = [
        {
            "episode_index": int(row["episode_index"]),
            "frame_index": int(row["frame_index"]),
        }
        for row in selected_rows
    ]
    if len(selected_rows) != args.rows:
        raise RuntimeError(
            f"requested {args.rows} frame rows, but selected {len(selected_rows)}"
        )
    timed_query = duckdb_timed_query(args, selected_rows)
    validation_query = duckdb_decode_query(
        args, selected_rows, hash_images=True, order_output=True
    )
    setup_seconds = time.perf_counter() - started

    def execute() -> dict[str, int]:
        result = connection.execute(timed_query)[0]
        decoded_images = int(result["decoded_images"])
        return {
            "frame_rows": decoded_images // len(args.camera),
            "decoded_images": decoded_images,
            "decoded_bytes": int(result["decoded_bytes"]),
        }

    def validate() -> list[dict[str, Any]]:
        result = connection.execute(validation_query)
        return [
            {
                "episode_index": int(row["episode_index"]),
                "frame_index": int(row["frame_index"]),
                "video_key": row["video_key"],
                "shape": [int(row["height"]), int(row["width"]), 3],
                "sha256": row["sha256"],
            }
            for row in result
        ]

    extra: dict[str, Any] = {
        "duckdb_version": version_rows[0]["library_version"],
        "duckdb_source_id": version_rows[0]["source_id"],
        "duckdb_cli": connection.executable,
        "selection_seconds": selection_seconds,
        "selection_strategy": "metadata-only lerobot_frames query",
        "timed_query": timed_query.strip(),
        "validation_query": validation_query.strip(),
        "timing_boundary": (
            "DuckDB executes lerobot_video_targets and consumes every BLOB with "
            "octet_length; BLOB transfer and SHA-256 are excluded"
        ),
        "validation_boundary": (
            "DuckDB replays the target query, hashes RGB BLOBs with SQL sha256, "
            "and transfers only keys, shapes, and digests"
        ),
        "close": connection.close,
    }

    def collect_profile() -> None:
        metrics: dict[str, int] = {}
        paths = []
        for label, profile_query in source_profile_queries(args, selected_rows):
            profile_path = (
                Path(args.output).with_suffix(f".{label}.profile.json").resolve()
            )
            profile_sql = (
                "PRAGMA enable_profiling='json'; "
                f"SET profiling_output = {sql_string(str(profile_path))}; "
                f"{profile_query}; PRAGMA disable_profiling"
            )
            connection.execute(profile_sql)
            episode_metrics: dict[str, int] = {}
            find_profile_metrics(json.loads(profile_path.read_text()), episode_metrics)
            for key, value in episode_metrics.items():
                metrics[key] = metrics.get(key, 0) + value
            paths.append(str(profile_path))
        extra["profile_paths"] = paths
        extra["profile_scope"] = (
            "one source-function query per compatible selected-episode group"
        )
        extra["profile_metrics"] = metrics

    extra["collect_profile"] = collect_profile
    return setup_seconds, execute, validate, extra


def run_duckdb(
    args: argparse.Namespace,
) -> tuple[
    float,
    Callable[[], dict[str, int]],
    Callable[[], list[dict[str, Any]]],
    dict[str, Any],
]:
    if args.duckdb_cli:
        return run_duckdb_cli(args)
    return run_duckdb_python(args)


def run_daft(
    args: argparse.Namespace,
) -> tuple[
    float,
    Callable[[], dict[str, int]],
    Callable[[], list[dict[str, Any]]],
    dict[str, Any],
]:
    import daft
    from daft.datasets import lerobot

    started = time.perf_counter()
    selection_started = time.perf_counter()
    metadata = lerobot.read(args.dataset, load_video_frames=False)
    if not args.all_episodes:
        metadata = metadata.where(daft.col("episode_index") == args.episode)
    selected = (
        metadata.select("index", "episode_index", "frame_index")
        .sort(["episode_index", "frame_index"])
        .limit(args.rows)
        .collect()
        .to_pydict()
    )
    selection_seconds = time.perf_counter() - selection_started
    selected_indices = [int(index) for index in selected["index"]]
    if len(selected_indices) != args.rows:
        raise RuntimeError(
            f"requested {args.rows} frame rows, but selected {len(selected_indices)}"
        )

    setup_seconds = time.perf_counter() - started

    def decode_frame() -> Any:
        # collect() materializes a Daft DataFrame in place. Reusing the same
        # object would turn later repeats into cached no-ops rather than decode
        # measurements, so construct a fresh lazy plan for every invocation.
        frame = lerobot.read(args.dataset, load_video_frames=args.camera)
        frame = frame.where(daft.col("index").is_in(selected_indices))
        return frame.select("episode_index", "frame_index", "index", *args.camera)

    def execute() -> dict[str, int]:
        materialized = decode_frame().collect()
        frame_rows = len(materialized)
        return {
            "frame_rows": frame_rows,
            "decoded_images": frame_rows * len(args.camera),
        }

    def validate() -> list[dict[str, Any]]:
        data = decode_frame().collect().to_pydict()
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
        validate,
        {
            "daft_version": getattr(daft, "__version__", "unknown"),
            "selection_seconds": selection_seconds,
            "selection_strategy": (
                "metadata-only sorted selection followed by an index filter "
                "that Daft pushes below the video UDF"
            ),
            "selected_index_min": min(selected_indices),
            "selected_index_max": max(selected_indices),
            "timing_boundary": (
                "Daft constructs a fresh lazy plan and collects only the selected "
                "image columns into a materialized DataFrame; to_pydict and "
                "SHA-256 are excluded"
            ),
            "validation_boundary": (
                "Daft replays a fresh plan, converts collected images to contiguous "
                "uint8 HWC bytes in Python, and hashes those bytes"
            ),
        },
    )


def run_lerobot(
    args: argparse.Namespace,
) -> tuple[
    float,
    Callable[[], dict[str, int]],
    Callable[[], list[dict[str, Any]]],
    dict[str, Any],
]:
    from lerobot.datasets.lerobot_dataset import LeRobotDataset

    started = time.perf_counter()
    dataset_kwargs: dict[str, Any] = {
        "tolerance_s": args.tolerance,
        "video_backend": args.video_backend,
        "return_uint8": True,
    }
    if not args.all_episodes:
        dataset_kwargs["episodes"] = [args.episode]
    if args.lerobot_root:
        dataset_kwargs["root"] = args.lerobot_root
    if args.revision:
        dataset_kwargs["revision"] = args.revision
    dataset = LeRobotDataset(args.dataset, **dataset_kwargs)
    count = min(args.rows, len(dataset))
    if count != args.rows:
        raise RuntimeError(f"requested {args.rows} frame rows, but selected {count}")
    setup_seconds = time.perf_counter() - started

    def scalar(value: Any) -> int:
        return int(value.item()) if hasattr(value, "item") else int(value)

    def load_items() -> list[dict[str, Any]]:
        if hasattr(dataset, "__getitems__"):
            return dataset.__getitems__(list(range(count)))
        return [dataset[index] for index in range(count)]

    def execute() -> dict[str, int]:
        items = load_items()
        return {
            "frame_rows": len(items),
            "decoded_images": len(items) * len(args.camera),
        }

    def validate() -> list[dict[str, Any]]:
        items = load_items()
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

    try:
        from importlib.metadata import version

        lerobot_version = version("lerobot")
    except Exception:
        lerobot_version = "unknown"
    return (
        setup_seconds,
        execute,
        validate,
        {
            "video_backend": args.video_backend,
            "lerobot_version": lerobot_version,
            "lerobot_root": (
                str(Path(args.lerobot_root).resolve()) if args.lerobot_root else None
            ),
            "selection_seconds": 0.0,
            "selection_strategy": (
                "native dataset episode subset followed by positional prefix selection"
            ),
            "timing_boundary": (
                "LeRobot returns selected uint8 Torch image tensors; NumPy conversion "
                "and SHA-256 are excluded"
            ),
            "validation_boundary": (
                "LeRobot replays the native batch read, converts tensors to "
                "contiguous uint8 HWC bytes in Python, and hashes those bytes"
            ),
        },
    )


def run_command(args: argparse.Namespace) -> int:
    adapters = {"duckdb": run_duckdb, "daft": run_daft, "lerobot": run_lerobot}
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    setup_seconds, execute, validate, extra = adapters[args.engine](args)
    collect_profile = extra.pop("collect_profile", None)
    close = extra.pop("close", None)
    try:
        durations, timed_summary = time_callable(execute, args.warmups, args.repeats)
        validation_started = time.perf_counter()
        rows = canonical_rows(validate())
        validation_seconds = time.perf_counter() - validation_started
        if collect_profile is not None and not args.no_profile:
            collect_profile()
    finally:
        if close is not None:
            close()
    expected_rows = args.rows * len(args.camera)
    if len(rows) != expected_rows:
        raise RuntimeError(
            f"expected {expected_rows} decoded rows, received {len(rows)}"
        )
    if timed_summary.get("frame_rows") != args.rows:
        raise RuntimeError(
            f"timed execution materialized {timed_summary.get('frame_rows')} frame "
            f"rows, expected {args.rows}"
        )
    if timed_summary.get("decoded_images") != expected_rows:
        raise RuntimeError(
            "timed execution materialized "
            f"{timed_summary.get('decoded_images')} images, expected {expected_rows}"
        )
    decode_median = statistics.median(durations)
    result = {
        "schema_version": 3,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "engine": args.engine,
        "dataset": args.dataset,
        "revision": args.revision,
        "episode": None if args.all_episodes else args.episode,
        "all_episodes": args.all_episodes,
        "requested_frame_rows": args.rows,
        "cameras": args.camera,
        "decoded_images": len(rows),
        "warmups": args.warmups,
        "repeats": args.repeats,
        "cache_state": args.cache_state,
        "setup_seconds": setup_seconds,
        "decode_durations_seconds": durations,
        "decode_median_seconds": decode_median,
        "decode_min_seconds": min(durations),
        "timed_summary": timed_summary,
        "validation_seconds": validation_seconds,
        "timing_scope": (
            "decode and native-engine image materialization after target selection; "
            "excludes per-image SHA-256 validation"
        ),
        "validation_scope": (
            "one additional decode/materialization pass, canonical ordering, and "
            "per-image SHA-256; see validation_boundary for engine-specific work"
        ),
        # Backward-compatible aliases used by existing result consumers.
        "durations_seconds": durations,
        "median_seconds": decode_median,
        "min_seconds": min(durations),
        "configuration": {
            "tolerance": args.tolerance,
            "cluster_gap": args.cluster_gap,
            "batch_size": args.batch_size,
            "target_buffer_size": args.target_buffer_size,
            "decode_threads": args.decode_threads,
            "max_cached_decoders": args.max_cached_decoders,
            "max_pending_targets": args.max_pending_targets,
            "max_output_bytes": args.max_output_bytes,
            "codec_threads": args.codec_threads,
            "width": args.width,
            "height": args.height,
        },
        "machine": machine_info(),
        "rows": rows,
        **extra,
    }
    output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(
        f"{args.engine}: {len(rows)} images, decode median {decode_median:.3f}s "
        f"({decode_median / len(rows):.4f}s/image), validation "
        f"{validation_seconds:.3f}s -> {output}"
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
    medians = [
        result.get("decode_median_seconds", result["median_seconds"])
        for result in results
    ]
    fastest = min(medians)
    for result, median in zip(results, medians):
        print(
            f"{result['engine']:<8} {median:>9.3f}s decode  "
            f"{median / fastest:>6.2f}x fastest  "
            f"{result['decoded_images']} images  "
            f"validation {result.get('validation_seconds', float('nan')):.3f}s"
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
        "--revision", help="immutable Hugging Face revision passed to native LeRobot"
    )
    run.add_argument(
        "--camera",
        action="append",
        required=True,
        help="repeat for every camera to decode",
    )
    run.add_argument("--episode", type=int, default=0)
    run.add_argument(
        "--all-episodes",
        action="store_true",
        help="select the first --rows frames globally instead of one episode",
    )
    run.add_argument("--rows", type=int, default=16)
    run.add_argument("--warmups", type=int, default=1)
    run.add_argument("--repeats", type=int, default=3)
    run.add_argument("--output", required=True)
    run.add_argument(
        "--cache-state",
        choices=("cold-process", "warm-process", "unspecified"),
        default="unspecified",
        help="label the intended cache state in the result",
    )
    run.add_argument(
        "--duckdb-cli",
        help="official DuckDB shell to use instead of importing Python duckdb",
    )
    run.add_argument(
        "--duckdb-load",
        action="append",
        default=[],
        help="extra DuckDB extension to LOAD before lerobot (repeatable)",
    )
    run.add_argument(
        "--extension", help="path to lerobot.duckdb_extension for the DuckDB engine"
    )
    run.add_argument("--video-backend", choices=("pyav", "torchcodec"), default="pyav")
    run.add_argument("--tolerance", type=float, default=1e-4)
    run.add_argument("--cluster-gap", type=float, default=10.0)
    run.add_argument("--batch-size", type=int, default=16)
    run.add_argument("--target-buffer-size", type=int, default=256)
    run.add_argument("--decode-threads", type=int, default=8)
    run.add_argument("--max-cached-decoders", type=int, default=8)
    run.add_argument("--max-pending-targets", type=int, default=4096)
    run.add_argument("--max-output-bytes", type=int, default=64 * 1024 * 1024)
    run.add_argument("--codec-threads", type=int, default=1)
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
