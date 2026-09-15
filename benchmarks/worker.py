#!/usr/bin/env python3
"""Persistent isolated worker; the coordinator serializes all measured operations."""

import contextlib
import importlib
import importlib.metadata
import json
import sys
import time
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from benchmarks.common import digest, machine_info, read_json  # noqa: E402


def emit(value):
    print(json.dumps(value, allow_nan=False), flush=True)


def main():
    job = read_json(sys.argv[1])
    adapter = None
    try:
        start = time.perf_counter()
        # Libraries may print diagnostics. Only this protocol writes to stdout.
        with contextlib.redirect_stdout(sys.stderr):
            workload = importlib.import_module(
                "benchmarks.workloads." + job["config"]["workload"]
            )
            if job["config"]["workload"] == "tensor_batch":
                import torch

                torch.set_num_threads(job["config"]["threads"])
            module = importlib.import_module("benchmarks.adapters." + job["engine"])
            adapter = module.Adapter(job)
            if job["engine"] in {"torchcodec", "lerobot"}:
                from torchcodec._core.ops import get_ffmpeg_library_versions

                adapter.metadata["ffmpeg_libraries"] = get_ffmpeg_library_versions()
            elif job["engine"] == "daft":
                import av

                adapter.metadata["ffmpeg_libraries"] = av.library_versions
        setup_seconds = time.perf_counter() - start
        packages = {
            p.metadata["Name"]: p.version for p in importlib.metadata.distributions()
        }
        emit(
            {
                "ok": True,
                "setup_seconds": setup_seconds,
                "packages": packages,
                "adapter": adapter.metadata,
                "machine": machine_info(),
            }
        )
        for line in sys.stdin:
            op = json.loads(line)["op"]
            if op == "close":
                break
            if op not in {"measure", "validate"}:
                raise ValueError("unknown worker operation")
            with contextlib.redirect_stdout(sys.stderr):
                start = time.perf_counter()
                result = workload.consume(adapter, job, validate=op == "validate")
                seconds = time.perf_counter() - start
            if op == "measure":
                result.pop("records")
                emit({"ok": True, "seconds": seconds, "counters": result})
            else:
                emit(
                    {
                        "ok": True,
                        "validation": result,
                        "validation_sha256": digest(result),
                    }
                )
    except Exception as error:
        traceback.print_exc(file=sys.stderr)
        emit({"ok": False, "error": f"{type(error).__name__}: {error}"})
        return 1
    finally:
        if adapter is not None:
            with contextlib.redirect_stdout(sys.stderr):
                adapter.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
