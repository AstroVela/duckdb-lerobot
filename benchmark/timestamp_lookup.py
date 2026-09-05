#!/usr/bin/env python3
"""Measure timestamp lookup reuse before committing to an extension cache.

The materialized-table variant is an experiment, not a compact-array cache.
First-query timings use a new connection but do not flush OS filesystem caches.
"""

import argparse
import csv
import io
import json
import statistics
import subprocess
import tempfile
from pathlib import Path


def quote(value) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def run(cli: Path, sql: str):
    result = subprocess.run(
        [str(cli), "-batch", "-csv", "-noheader"], input=sql, text=True, capture_output=True, check=True
    )
    return list(csv.reader(io.StringIO(result.stdout)))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", required=True, type=Path)
    parser.add_argument("--rows", type=int, default=1_000_000)
    parser.add_argument("--targets", type=int, nargs="+", default=[4, 256, 8192])
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if min(args.rows, args.repeats, args.threads, *args.targets) < 1:
        parser.error("sizes, repeats and threads must be positive")
    cli = args.duckdb.resolve()
    version = run(cli, "SELECT version();")[0][0]
    report = {
        "duckdb": version,
        "rows": args.rows,
        "threads": args.threads,
        "timestamp_payload_lower_bound_bytes": args.rows * 8,
        "scope": "local synthetic shard; new connection, OS cache not flushed; SQL table prototype",
        "scenarios": [],
    }
    with tempfile.TemporaryDirectory(prefix="lerobot-timestamp-benchmark-") as directory:
        workspace = Path(directory)
        parquet = workspace / "frames.parquet"
        run(
            cli,
            f"COPY (SELECT (i // 1000)::BIGINT episode_index, (i % 1000)::BIGINT frame_index, "
            f"(i % 1000) / 32.0 AS timestamp FROM range({args.rows}) t(i)) "
            f"TO {quote(parquet)} (FORMAT parquet, ROW_GROUP_SIZE 2048);",
        )
        for targets in args.targets:
            sql = [f"SET threads={args.threads};", "SET enable_profiling='json';"]
            outputs = []
            for variant in ("parquet", "materialized"):
                if variant == "materialized":
                    build_profile = workspace / f"build-{targets}.json"
                    sql += [
                        f"SET profiling_output={quote(build_profile)};",
                        f"CREATE TEMP TABLE timestamps AS SELECT * FROM read_parquet({quote(parquet)});",
                    ]
                source = f"read_parquet({quote(parquet)})" if variant == "parquet" else "timestamps"
                for iteration in range(args.repeats + 1):
                    profile = workspace / f"{targets}-{variant}-{iteration}.json"
                    outputs.append((variant, iteration, profile))
                    sql += [
                        f"SET profiling_output={quote(profile)};",
                        "WITH requested AS (SELECT ((i * 7919) % "
                        + str(args.rows)
                        + ") // 1000 episode_index, "
                        "((i * 7919) % " + str(args.rows) + ") % 1000 frame_index "
                        f"FROM range({targets}) t(i)) "
                        f"SELECT '{variant}', {iteration}, count(frames.timestamp), sum(frames.timestamp) "
                        f"FROM requested LEFT JOIN {source} frames USING (episode_index, frame_index);",
                    ]
            results = run(cli, "\n".join(sql))
            checksums = [row[2:] for row in results if row[0] in ("parquet", "materialized")]
            assert len(checksums) == 2 * (args.repeats + 1)
            assert all(row == checksums[0] for row in checksums), checksums
            scenario = {"targets": targets}
            for variant in ("parquet", "materialized"):
                profiles = [json.loads(path.read_text()) for name, _, path in outputs if name == variant]
                scenario[variant] = {
                    "first_seconds": profiles[0]["latency"],
                    "warm_median_seconds": statistics.median(p["latency"] for p in profiles[1:]),
                    "peak_buffer_bytes": max(p["system_peak_buffer_memory"] for p in profiles),
                }
            build = json.loads(build_profile.read_text())
            scenario["materialization_seconds"] = build["latency"]
            saving = (
                scenario["parquet"]["warm_median_seconds"] - scenario["materialized"]["warm_median_seconds"]
            )
            scenario["break_even_reuses_estimate"] = build["latency"] / saving if saving > 0 else None
            report["scenarios"].append(scenario)
    rendered = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered)
    print(rendered, end="")


if __name__ == "__main__":
    main()
