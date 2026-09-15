#!/usr/bin/env python3
"""Validate a completed run and generate Markdown from its recorded samples."""

import argparse
import math
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from benchmarks.common import (
    SCHEMA_VERSION,
    digest,
    read_json,
    request_keys,
    validate_keys,
)  # noqa: E402

LABELS = {
    "duckdb": "DuckDB / FFmpeg",
    "daft": "Daft",
    "lerobot": "LeRobot / TorchCodec",
    "torchcodec": "DuckDB / TorchCodec",
}


def summarize(samples):
    if not samples or any(not math.isfinite(s) or s <= 0 for s in samples):
        raise ValueError("timings must be finite and positive")
    quartiles = (
        statistics.quantiles(samples, n=4, method="inclusive")
        if len(samples) > 1
        else [samples[0]] * 3
    )
    return {"median": statistics.median(samples), "iqr": quartiles[2] - quartiles[0]}


def validate(result, root):
    if result["schema_version"] != SCHEMA_VERSION or result["status"] != "complete":
        raise ValueError("only complete runs with the current schema can be reported")
    config = result["config"]
    if (
        digest(config) != result["config_sha256"]
        or digest(result["dataset"]) != result["dataset_manifest_sha256"]
    ):
        raise ValueError("configuration or dataset manifest digest differs")
    if [case["name"] for case in result["cases"]] != [
        case["name"] for case in config["cases"]
    ]:
        raise ValueError("missing or reordered cases")
    environments = {}
    for case, spec in zip(result["cases"], config["cases"], strict=True):
        if not case["pixels_equal"] or set(case["engines"]) != set(config["engines"]):
            raise ValueError(
                "comparison requires all configured engines and matching pixels"
            )
        if (
            len(case["requests"]) != spec["rows"]
            or digest(case["requests"]) != case["requests_sha256"]
        ):
            raise ValueError("request manifest differs")
        reference = None
        for engine in config["engines"]:
            record = case["engines"][engine]
            if len(record["warm_seconds"]) != config["repeats"]:
                raise ValueError("repeat count differs")
            summarize(record["warm_seconds"])
            environment = record["environment"]
            if (
                engine in environments
                and environments[engine] != environment["packages"]
            ):
                raise ValueError("package versions changed between cases")
            environments[engine] = environment["packages"]
            if environment["machine"] != result["machine"]:
                # Coordinator may use another Python, but all workers must share the same host/affinity.
                expected = {k: v for k, v in result["machine"].items() if k != "python"}
                actual = {
                    k: v for k, v in environment["machine"].items() if k != "python"
                }
                if actual != expected:
                    raise ValueError("worker hardware or CPU affinity differs")
            validation_path = (root / record["validation"]["file"]).resolve()
            if not validation_path.is_relative_to(root.resolve()):
                raise ValueError("validation file must be within the run directory")
            validation = read_json(validation_path)
            if digest(validation) != record["validation"]["sha256"]:
                raise ValueError("validation digest differs")
            validate_keys(
                [r["key"] for r in validation["records"]],
                request_keys(case["requests"], config["dataset"]["cameras"]),
            )
            if reference is not None and reference != validation:
                raise ValueError(
                    "frame keys, shapes or pixel digests differ between engines"
                )
            reference = validation
            if record["counters"] != {
                k: v for k, v in validation.items() if k != "records"
            }:
                raise ValueError("measured output counters differ from validation")


def table(result, compact=False):
    engines = result["config"]["engines"]
    headers = [LABELS[engine] for engine in engines]
    lines = [
        "| Case | " + " | ".join(headers) + " |",
        "| :--- | " + " | ".join(["---:"] * len(headers)) + " |",
    ]
    for case in result["cases"]:
        cells = []
        for engine in engines:
            summary = summarize(case["engines"][engine]["warm_seconds"])
            cells.append(
                f"{summary['median']:.3f}"
                if compact
                else f"{summary['median']:.3f} ({summary['iqr']:.3f})"
            )
        lines.append("| " + case["name"] + " | " + " | ".join(cells) + " |")
    return "\n".join(lines)


def render(result):
    config = result["config"]
    lines = [
        f"# {config['workload']} benchmark",
        "",
        "Seconds: median (IQR). Lower is better.",
        "",
        table(result),
        "",
        f"- Repetitions: {config['repeats']}; warmups: {config['warmups']} after a separately recorded first call.",
        f"- Batch size: {config['batch_size']}; threads: {config['threads']}; affinity: {result['machine']['affinity']}.",
        f"- CPU: {result['machine'].get('cpu_model', 'unknown')}; memory: {result['machine'].get('memory_bytes')} bytes.",
        f"- Dataset: `{config['dataset']['repo_id']}` at `{config['dataset'].get('revision', 'synthetic')}`.",
        f"- Dataset manifest SHA-256: `{result['dataset_manifest_sha256']}`.",
        f"- Extension source: `{result['extension']['source_commit']}`; binary SHA-256: `{result['extension']['sha256']}`.",
        f"- Suite source: `{result['source']['git_commit']}`; dirty: `{result['source']['git_dirty']}`; file manifest SHA-256: `{result['source']['files_sha256']}`.",
        f"- Timing: {result['timing']}",
        f"- Cache: {result['cache_policy']}",
        "- Correctness: every requested frame key, order, shape and pixel digest matches across engines.",
        "- Multi-file data is derived by copying each episode's full source MP4 into a separate file; content diversity is unchanged.",
        "",
        "## Software",
        "",
        "| Engine | Packages |",
        "| :--- | :--- |",
    ]
    for engine in config["engines"]:
        packages = result["cases"][0]["engines"][engine]["environment"]["packages"]
        names = (
            "duckdb",
            "daft",
            "lerobot",
            "torch",
            "torchcodec",
            "av",
            "numpy",
            "pyarrow",
        )
        versions = ", ".join(
            f"{name} {packages[name]}" for name in names if name in packages
        )
        lines.append(f"| {LABELS[engine]} | {versions} |")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--update-readme", type=Path, help="replace the marked compact video table"
    )
    args = parser.parse_args()
    result = read_json(args.results)
    validate(result, args.results.parent)
    report = render(result)
    if args.output:
        args.output.write_text(report)
    else:
        print(report, end="")
    if args.update_readme:
        if (
            result["config"]["workload"] != "video_read"
            or result["config"]["dataset"]["kind"] == "synthetic"
        ):
            parser.error("README table requires the full video-read workload")
        text = args.update_readme.read_text()
        begin, end = "<!-- benchmark-table:start -->", "<!-- benchmark-table:end -->"
        if text.count(begin) != 1 or text.count(end) != 1:
            parser.error("README must have exactly one benchmark marker pair")
        head, tail = text.split(begin)
        _, tail = tail.split(end)
        args.update_readme.write_text(
            head + begin + "\n\n" + table(result, compact=True) + "\n\n" + end + tail
        )


if __name__ == "__main__":
    main()
