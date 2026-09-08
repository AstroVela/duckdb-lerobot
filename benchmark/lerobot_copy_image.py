#!/usr/bin/env python3
"""Measure image COPY and independently check every PNG with Pillow."""

import argparse
import hashlib
import importlib.metadata
import json
import platform
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# Share the independent fixture and validator with the conformance job.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "test/conformance"))
from test_image_copy import copy_sql, quote, validate, write_input  # noqa: E402

CASES = {
    "tiny": ([(16, 16)], 2048, False),
    "small": ([(120, 160)], 256, False),
    "vga": ([(480, 640)], 64, False),
    "vga-noise": ([(480, 640)], 64, True),
    "mixed": ([(48, 64), (120, 160)], 256, False),
}


def run_once(
    args: argparse.Namespace,
    work: Path,
    source: Path,
    shapes: list,
    frames: int,
    noise: bool,
    extension: Path,
) -> dict:
    profile = work / "profile.json"
    root = work / "dataset"
    sql = (
        f"LOAD {quote(extension)}; SET threads={args.threads}; SET memory_limit={quote(args.memory_limit)}; "
        f"SET temp_directory={quote(work / 'spill')}; "
        f"CREATE TEMP TABLE input AS SELECT * FROM read_parquet({quote(source)}); "
        f"SET enable_profiling='json'; SET profiling_output={quote(profile)}; "
        + copy_sql(root, shapes)
        + "PRAGMA disable_profiling;"
    )
    command = [str(args.duckdb), "-unsigned", "-batch"]
    rss_file = work / "rss.txt"
    has_rss = platform.system() == "Linux" and Path("/usr/bin/time").is_file()
    if has_rss:
        command = ["/usr/bin/time", "-f", "%M", "-o", str(rss_file), *command]
    start = time.perf_counter()
    result = subprocess.run(
        command, input=sql, text=True, capture_output=True, timeout=180
    )
    process_seconds = time.perf_counter() - start
    if result.returncode:
        raise RuntimeError(result.stderr)
    data = json.loads(profile.read_text())
    assert data["query_name"].lstrip().upper().startswith("COPY")
    return {
        "copy_seconds": data["latency"],
        "process_seconds": process_seconds,
        "peak_process_rss_bytes": int(rss_file.read_text()) * 1024 if has_rss else None,
        "peak_connection_buffer_bytes": data["system_peak_buffer_memory"],
        "peak_temp_bytes": data["system_peak_temp_dir_size"],
        **validate(root, shapes, frames, args.episodes, noise),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", type=Path, required=True)
    parser.add_argument("--extension", type=Path, required=True)
    parser.add_argument(
        "--baseline-extension",
        type=Path,
        help="alternate before/after run order and verify identical hashes",
    )
    parser.add_argument("--cases", nargs="+", choices=CASES, default=list(CASES))
    parser.add_argument("--episodes", type=int, default=2)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--memory-limit", default="512MB")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if min(args.episodes, args.repeats, args.threads) < 1:
        parser.error("episodes, repeats and threads must be positive")
    args.duckdb = args.duckdb.resolve()
    args.extension = args.extension.resolve()
    if args.baseline_extension:
        args.baseline_extension = args.baseline_extension.resolve()
    report = {
        "extension_sha256": hashlib.sha256(args.extension.read_bytes()).hexdigest(),
        "baseline_extension_sha256": (
            hashlib.sha256(args.baseline_extension.read_bytes()).hexdigest()
            if args.baseline_extension
            else None
        ),
        "duckdb_sha256": hashlib.sha256(args.duckdb.read_bytes()).hexdigest(),
        "platform": platform.platform(),
        "dependencies": {
            name: importlib.metadata.version(name)
            for name in ("numpy", "pillow", "pyarrow")
        },
        "parameters": {
            key: str(value) if isinstance(value, Path) else value
            for key, value in vars(args).items()
        },
        "scope": "synthetic local data, fresh process per repeat, OS cache not flushed",
        "timing_scope": "COPY latency excludes input materialization and Pillow validation; process time includes input load and shutdown",
        "memory_scope": "GNU time DuckDB process peak includes input load and shutdown, excludes Python; buffer/temp peaks are cumulative",
        "scenarios": [],
    }
    with tempfile.TemporaryDirectory(prefix="lerobot-png-benchmark-") as directory:
        for case in args.cases:
            shapes, frames, noise = CASES[case]
            source = Path(directory) / f"{case}.parquet"
            write_input(source, shapes, frames, args.episodes, noise)
            binaries = [("after", args.extension)]
            if args.baseline_extension:
                binaries.insert(0, ("before", args.baseline_extension))
            measurements = {label: [] for label, _ in binaries}
            for repeat in range(args.repeats):
                for label, extension in binaries if repeat % 2 == 0 else binaries[::-1]:
                    with tempfile.TemporaryDirectory(dir=directory) as run_dir:
                        measurements[label].append(
                            run_once(
                                args,
                                Path(run_dir),
                                source,
                                shapes,
                                frames,
                                noise,
                                extension,
                            )
                        )
            runs = measurements["after"]
            for field in ("png_sha256", "statistics_sha256"):
                assert (
                    len(
                        {run[field] for group in measurements.values() for run in group}
                    )
                    == 1
                ), field
            scenario = {
                "case": case,
                "shapes_hw": shapes,
                "frames_per_episode": frames,
                "noise": noise,
                "median_copy_seconds": statistics.median(
                    run["copy_seconds"] for run in runs
                ),
                "runs": runs,
            }
            if args.baseline_extension:
                scenario["baseline_runs"] = measurements["before"]
                scenario["median_baseline_copy_seconds"] = statistics.median(
                    run["copy_seconds"] for run in measurements["before"]
                )
                scenario["speedup"] = (
                    scenario["median_baseline_copy_seconds"]
                    / scenario["median_copy_seconds"]
                )
                scenario["run_order"] = (
                    "before/after on even repeats, after/before on odd repeats"
                )
            report["scenarios"].append(scenario)
            print(f"{case}: {scenario['median_copy_seconds']:.4f} s", flush=True)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
