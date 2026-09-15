#!/usr/bin/env python3
"""Measure numeric COPY, checking every frame and hashing all written statistics."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import platform
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

from timestamp_lookup import quote

SQL_TYPES = {"float32": "FLOAT", "float64": "DOUBLE", "int64": "BIGINT"}


def fixture_sql(width: int, dtype: str, frames: int, episodes: int) -> str:
    # Binary fractions avoid SQL/Python decimal conversion differences. Values
    # vary along both axes, including negative values and large int64 extrema.
    sql_type = SQL_TYPES[dtype]
    terms = []
    for dimension in range(width):
        term = f"((i * 17 + {dimension} * 13) % 2047 - 1023)"
        if dtype == "int64":
            term = f"(9007199254740992::BIGINT + {term})"
        else:
            term = f"({term} / 128.0)"
        terms.append(f"({term})::{sql_type}")
    value = terms[0] if width == 1 else "array_value(" + ",".join(terms) + ")"
    return (
        f"CREATE TEMP TABLE input AS SELECT i, (i // {frames})::BIGINT episode_index, "
        f"'numeric benchmark'::VARCHAR task, {value} AS action FROM range({frames * episodes}) t(i);"
    )


def canonical_hash(value: object) -> str:
    text = json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)
    return hashlib.sha256(text.encode()).hexdigest()


def run_once(args: argparse.Namespace, work: Path, width: int, dtype: str) -> dict:
    root = work / "dataset"
    profile = work / "profile.json"
    features = json.dumps({"action": {"dtype": dtype, "shape": [width]}})
    storage_type = SQL_TYPES[dtype] + (f"[{width}]" if width > 1 else "")
    sql = (
        f"LOAD {quote(args.extension)}; SET threads={args.threads}; "
        f"SET memory_limit={quote(args.memory_limit)}; "
        f"SET temp_directory={quote(work / 'spill')}; "
        + fixture_sql(width, dtype, args.frames, args.episodes)
        + f"SET enable_profiling='json'; SET profiling_output={quote(profile)}; "
        "COPY (SELECT episode_index, task, action FROM input ORDER BY i) TO "
        f"{quote(root)} (FORMAT lerobot, FPS 30, FEATURES {quote(features)}); "
        "PRAGMA disable_profiling; "
        # Validate all rows, not just a checksum or aggregate. Reconstruct the
        # fixed array because native parquet_scan returns LIST values.
        'SELECT count(*), count(*) FILTER (WHERE f."index" IS NULL OR i.i IS NULL '
        "OR f.episode_index IS DISTINCT FROM i.episode_index "
        f"OR f.frame_index IS DISTINCT FROM i.i % {args.frames} "
        f"OR f.action::{storage_type} IS DISTINCT FROM i.action) "
        f"FROM read_parquet({quote(root / 'data/**/*.parquet')}) f "
        'FULL OUTER JOIN input i ON f."index"=i.i; '
        # The only selected fields are statistics and stable episode identity.
        "SELECT to_json(s) FROM (SELECT episode_index, COLUMNS('^stats/') "
        f"FROM read_parquet({quote(root / 'meta/episodes/**/*.parquet')}) "
        "ORDER BY episode_index) s;"
    )
    (work / "input.sql").write_text(sql)
    command = [str(args.duckdb), "-unsigned", "-csv", "-noheader"]
    rss_file = work / "peak-rss.txt"
    # GNU time uses wait4's per-process high-water mark, including shutdown.
    # No /proc polling that can miss finalize or accidentally reuse an old peak.
    gnu_time = Path("/usr/bin/time")
    has_rss = platform.system() == "Linux" and gnu_time.is_file()
    if has_rss:
        command = [str(gnu_time), "-f", "%M", "-o", str(rss_file), *command]
    start = time.perf_counter()
    result = subprocess.run(command, input=sql, text=True, capture_output=True)
    process_seconds = time.perf_counter() - start
    (work / "stderr.log").write_text(result.stderr)
    if result.returncode:
        raise RuntimeError(f"numeric COPY failed: {result.stderr}")
    rows = list(csv.reader(io.StringIO(result.stdout)))
    expected_count = str(args.frames * args.episodes)
    validation = rows[-args.episodes - 1 :]
    if validation[0] != [expected_count, "0"]:
        raise AssertionError(f"unexpected COPY/frame validation: {validation[0]}")
    episode_stats = [json.loads(row[0]) for row in validation[1:]]
    if len(episode_stats) != args.episodes:
        raise AssertionError("missing episode statistics")
    dataset_stats = json.loads((root / "meta/stats.json").read_text())
    data = json.loads(profile.read_text())
    if not data["query_name"].lstrip().upper().startswith("COPY"):
        raise AssertionError("profiler did not capture the COPY statement")
    return {
        "copy_seconds": data["latency"],
        "process_seconds": process_seconds,
        "peak_process_rss_bytes": int(rss_file.read_text()) * 1024 if has_rss else None,
        "peak_connection_buffer_bytes": data["system_peak_buffer_memory"],
        "peak_temp_bytes": data["system_peak_temp_dir_size"],
        "statistics_sha256": canonical_hash([episode_stats, dataset_stats]),
        "validated_frames": int(expected_count),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", type=Path, required=True)
    parser.add_argument("--extension", type=Path, required=True)
    parser.add_argument("--widths", nargs="+", type=int, default=[14, 64, 65, 256])
    parser.add_argument(
        "--dtypes", nargs="+", choices=SQL_TYPES, default=list(SQL_TYPES)
    )
    parser.add_argument("--frames", type=int, default=10000, help="Frames per episode")
    parser.add_argument("--episodes", type=int, default=2)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--memory-limit", default="256MB")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if min(args.frames, args.episodes, args.repeats, args.threads, *args.widths) < 1:
        parser.error("sizes, threads and repeats must be positive")
    args.duckdb = args.duckdb.resolve()
    args.extension = args.extension.resolve()
    report = {
        "schema_version": 1,
        "extension_sha256": hashlib.sha256(args.extension.read_bytes()).hexdigest(),
        "platform": platform.platform(),
        "parameters": {
            key: str(value) if isinstance(value, Path) else value
            for key, value in vars(args).items()
        },
        "scope": "fresh process per repeat; local synthetic data; OS cache not flushed",
        "timing_scope": "copy_seconds excludes fixture creation and validation; process_seconds includes them",
        "memory_scope": "RSS includes fixture, COPY, validation and shutdown; connection buffer peak is cumulative",
        "scenarios": [],
    }
    with tempfile.TemporaryDirectory(prefix="lerobot-numeric-copy-") as directory:
        for dtype in args.dtypes:
            for width in args.widths:
                results = []
                for repeat in range(args.repeats):
                    work = Path(directory) / f"{dtype}-{width}-{repeat}"
                    work.mkdir()
                    results.append(run_once(args, work, width, dtype))
                if len({row["statistics_sha256"] for row in results}) != 1:
                    raise AssertionError("statistics changed between repeats")
                scenario = {
                    "dtype": dtype,
                    "width": width,
                    "median_copy_seconds": statistics.median(
                        r["copy_seconds"] for r in results
                    ),
                    "runs": results,
                }
                report["scenarios"].append(scenario)
                print(
                    f"{dtype}[{width}]: {scenario['median_copy_seconds']:.3f} s",
                    flush=True,
                )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
