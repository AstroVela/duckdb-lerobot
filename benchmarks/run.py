#!/usr/bin/env python3
"""Run a fixed workload in separate, locked engine environments."""

import argparse
import json
import os
import random
import selectors
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from benchmarks.common import (
    ROOT,
    SCHEMA_VERSION,
    digest,
    extension_info,
    load_config,
    machine_info,
    payload_files,
    read_json,
    source_info,
    verify_packages,
    write_json,
)  # noqa: E402

THREAD_ENV = (
    "OMP_NUM_THREADS",
    "MKL_NUM_THREADS",
    "OPENBLAS_NUM_THREADS",
    "RAYON_NUM_THREADS",
)


def plan_requests(frames, case, seed):
    if case["rows"] > len(frames):
        raise ValueError(
            f"{case['name']}: requested more rows than the dataset contains"
        )
    if case["order"] == "sequential":
        return frames[: case["rows"]]
    return random.Random(seed).sample(frames, case["rows"])


class Worker:
    def __init__(self, python, job_path, log_path, threads, timeout):
        env = dict(
            os.environ,
            PYTHONNOUSERSITE="1",
            HF_HUB_OFFLINE="1",
            HF_DATASETS_OFFLINE="1",
            TOKENIZERS_PARALLELISM="false",
            DAFT_PROGRESS_BAR="0",
        )
        env.pop("PYTHONPATH", None)
        for key in THREAD_ENV:
            env[key] = str(threads)
        self.log = log_path.open("w")
        self.process = subprocess.Popen(
            [str(python), "-I", str(ROOT / "benchmarks/worker.py"), str(job_path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.log,
            text=True,
            bufsize=1,
            env=env,
            cwd=ROOT,
        )
        self.timeout = timeout
        self.log_path = log_path

    def receive(self):
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout, selectors.EVENT_READ)
            if not selector.select(self.timeout):
                raise TimeoutError(f"worker timed out; see {self.log_path}")
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError(f"worker exited without a result; see {self.log_path}")
        result = json.loads(line)
        if not result["ok"]:
            raise RuntimeError(f"{result['error']}; see {self.log_path}")
        return result

    def call(self, op):
        self.process.stdin.write(json.dumps({"op": op}) + "\n")
        self.process.stdin.flush()
        return self.receive()

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.process.stdin.close()
        self.process.stdout.close()
        self.log.close()


def run(args):
    config = load_config(args.config)
    data_root = args.data_root.resolve()
    manifest = read_json(data_root / "manifest.json")
    if manifest["dataset"] != config["dataset"]:
        raise ValueError("dataset manifest and configuration differ")
    for name, variant in manifest["variants"].items():
        if payload_files(data_root / name) != variant["files"]:
            raise ValueError(f"{name}: dataset payload changed since preparation")
    extension = args.extension.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    result = {
        "schema_version": SCHEMA_VERSION,
        "status": "running",
        "started_at": datetime.now(timezone.utc).isoformat(),
        "config": config,
        "config_sha256": digest(config),
        "source": source_info(),
        "machine": machine_info(),
        "extension": extension_info(
            extension, args.extension_commit, args.extension_manifest
        ),
        "dataset": manifest,
        "dataset_manifest_sha256": digest(manifest),
        "cache_policy": "No OS cache flush; first call separate; warmups then interleaved repeats; engine caches persist within each case",
        "timing": "Local requests -> decode -> Python transfer/conversion -> contiguous output; excludes setup/imports, request planning, hashing, JSON and IPC",
        "thread_environment": {key: str(config["threads"]) for key in THREAD_ENV},
        "cases": [],
    }
    write_json(output / "results.json", result)
    rng = random.Random(config["seed"])
    try:
        for case in config["cases"]:
            variant = manifest["variants"][case["variant"]]
            requests = plan_requests(variant["frames"], case, config["seed"])
            files = sorted(
                {
                    path
                    for r in requests
                    for path in variant["video_routes"][str(r["episode_index"])]
                }
            )
            if case["variant"] == "multi" and len(files) < 2:
                raise ValueError(
                    "multi-file case must actually touch multiple video files"
                )
            measurement = {
                "name": case["name"],
                "requests": requests,
                "requests_sha256": digest(requests),
                "video_files": files,
                "engines": {},
                "run_order": [],
            }
            workers = {}
            try:
                for engine in config["engines"]:
                    job = {
                        "engine": engine,
                        "config": config,
                        "requests": requests,
                        "dataset_root": str(data_root / case["variant"]),
                        "extension": str(extension),
                        "camera_shapes": variant["camera_shapes"],
                    }
                    job_path = output / "jobs" / f"{case['name']}-{engine}.json"
                    write_json(job_path, job)
                    environment = (
                        "torchcodec" if config["workload"] == "tensor_batch" else engine
                    )
                    python = (
                        args.python.resolve()
                        if args.python
                        else args.env_root.resolve() / environment / "bin/python"
                    )
                    worker = Worker(
                        python,
                        job_path,
                        output / f"{case['name']}-{engine}.log",
                        config["threads"],
                        args.timeout,
                    )
                    workers[engine] = worker
                    worker_environment = worker.receive()
                    if not args.python:
                        verify_packages(
                            worker_environment["packages"],
                            ROOT / "benchmarks/requirements" / f"{environment}.txt",
                        )
                    if (
                        engine == "torchcodec"
                        and worker_environment["adapter"]["reader_sha256"]
                        != result["source"]["files"][
                            "python/src/duckdb_lerobot/reader.py"
                        ]
                    ):
                        raise ValueError(
                            "installed optional reader differs from the recorded source; rerun setup.py"
                        )
                    measurement["engines"][engine] = {
                        "environment": worker_environment,
                        "warm_seconds": [],
                    }
                # A fresh process/cache per engine per case. No simultaneous timed work.
                for phase, rounds in (
                    ("first", 1),
                    ("warmup", config["warmups"]),
                    ("repeat", config["repeats"]),
                ):
                    for index in range(rounds):
                        order = list(workers)
                        rng.shuffle(order)
                        measurement["run_order"].append(
                            {"phase": phase, "round": index, "engines": order}
                        )
                        for engine in order:
                            sample = workers[engine].call("measure")
                            record = measurement["engines"][engine]
                            if (
                                "counters" in record
                                and record["counters"] != sample["counters"]
                            ):
                                raise ValueError(
                                    "output counters changed between repetitions"
                                )
                            record["counters"] = sample["counters"]
                            if phase == "first":
                                record["first_seconds"] = sample["seconds"]
                            elif phase == "repeat":
                                record["warm_seconds"].append(sample["seconds"])
                reference = None
                for engine, worker in workers.items():
                    validation = worker.call("validate")
                    if reference is not None and validation["validation"] != reference:
                        raise ValueError(
                            f"{case['name']}/{engine}: frame keys, shapes or pixels differ"
                        )
                    reference = validation["validation"]
                    validation_path = (
                        Path("validation") / f"{case['name']}-{engine}.json"
                    )
                    write_json(output / validation_path, reference)
                    measurement["engines"][engine]["validation"] = {
                        "file": str(validation_path),
                        "sha256": validation["validation_sha256"],
                    }
                measurement["pixels_equal"] = True
                result["cases"].append(measurement)
                write_json(output / "results.json", result)
                print(
                    f"{case['name']}: {len(requests)} rows, {len(files)} video files; pixels equal",
                    flush=True,
                )
            finally:
                for worker in workers.values():
                    worker.close()
        # LeRobot/Daft may create caches, but they must not mutate dataset payload.
        for name, variant in manifest["variants"].items():
            if payload_files(data_root / name) != variant["files"]:
                raise ValueError(f"{name}: engine mutated dataset payload")
        result["status"] = "complete"
    except Exception as error:
        result["status"] = "failed"
        result["error"] = str(error)
        raise
    finally:
        result["finished_at"] = datetime.now(timezone.utc).isoformat()
        write_json(output / "results.json", result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--data-root", type=Path, required=True)
    parser.add_argument("--extension", type=Path, required=True)
    parser.add_argument(
        "--extension-commit", help="actual binary source SHA, not the checkout SHA"
    )
    parser.add_argument(
        "--extension-manifest",
        type=Path,
        help="distribution-manifest.json verifies the binary and records its build",
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--env-root", type=Path, default=Path("build/benchmarks/envs"))
    parser.add_argument(
        "--python", type=Path, help="use one existing environment (CI smoke only)"
    )
    parser.add_argument(
        "--timeout", type=int, default=600, help="worker operation deadline in seconds"
    )
    args = parser.parse_args()
    if not args.extension_commit and not args.extension_manifest:
        parser.error(
            "record the binary source with --extension-commit or --extension-manifest"
        )
    if args.python and load_config(args.config)["dataset"]["kind"] != "synthetic":
        parser.error(
            "--python is restricted to synthetic smoke tests; use isolated environments for measurements"
        )
    run(args)


if __name__ == "__main__":
    main()
