#!/usr/bin/env python3
"""Compare SQL FFmpeg → Torch and SQL targets → TorchCodec, through NCHW batches.

Install ./python and compatible Torch/TorchCodec before running. Both paths
include SQL selection, materialization and torch.stack. Pixel hashes are checked
in a separate pass. Local CPU only; no network or GPU timing claims.
"""

import argparse
import hashlib
import importlib
import json
import platform
import statistics
import time
from pathlib import Path

import duckdb
import torch

from duckdb_lerobot import TorchCodecReader


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--extension", type=Path, required=True)
    parser.add_argument("--camera", required=True)
    parser.add_argument("--rows", type=int, nargs="+", default=[16, 100, 632])
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    importlib.import_module("torchcodec.decoders")  # Load dependencies outside timing.
    if min(*args.rows, args.batch_size, args.repeats, args.threads) <= 0:
        parser.error("rows, batch-size, repeats and threads must be positive")
    torch.set_num_threads(args.threads)
    dataset = str(args.dataset.resolve())
    requests = """
        SELECT row_number() OVER (ORDER BY episode_index, frame_index) AS request_id,
               episode_index, frame_index, ? AS video_key, 0::BIGINT AS delta_index
        FROM (SELECT episode_index, frame_index FROM lerobot_scan(?)
              ORDER BY episode_index, frame_index LIMIT ?) selected
        ORDER BY episode_index, frame_index
    """
    from importlib.metadata import version

    output = {
        "versions": {name: version(name) for name in ["duckdb", "torch", "torchcodec"]},
        "platform": platform.platform(),
        "dataset": dataset,
        "extension_sha256": hashlib.sha256(args.extension.read_bytes()).hexdigest(),
        "adapter_sha256": hashlib.sha256(
            Path(importlib.import_module("duckdb_lerobot.reader").__file__).read_bytes()
        ).hexdigest(),
        "configuration": vars(args),
        "scope": "CPU SQL selection through contiguous uint8 NCHW Torch batches; excludes hashes/imports",
        "cache": "First call and warm medians separate; OS cache not flushed; decoder cache persists per reader",
        "cases": [],
    }
    for count in args.rows:
        params = [args.camera, dataset, count]
        connections = {}
        for engine in ["sql_ffmpeg", "sql_torchcodec"]:
            con = duckdb.connect(
                config={"allow_unsigned_extensions": True, "threads": args.threads}
            )
            con.execute(
                "LOAD '" + str(args.extension.resolve()).replace("'", "''") + "'"
            )
            connections[engine] = con
        reader = TorchCodecReader(
            connections["sql_torchcodec"], dataset, batch_size=args.batch_size
        )
        native_sql = f"""
            SELECT episode_index, target_frame_index, image, height, width
            FROM lerobot_video_targets(?, ({requests}), codec_threads := 1)
            ORDER BY target_ordinal
        """

        def execute(engine, validate=False):
            hashes = []
            frames = 0
            total_bytes = 0
            if engine == "sql_torchcodec":
                batches = reader.batches(requests, params)
            else:
                result = connections[engine].execute(native_sql, [dataset, *params])

                def native_batches():
                    while rows := result.fetchmany(args.batch_size):
                        yield rows

                batches = native_batches()
            for batch in batches:
                if engine == "sql_torchcodec":
                    images = batch.images
                    keys = [
                        (t.episode_index, t.target_frame_index) for t in batch.targets
                    ]
                else:
                    images = [
                        torch.frombuffer(bytearray(blob), dtype=torch.uint8)
                        .reshape(height, width, 3)
                        .permute(2, 0, 1)
                        for _, _, blob, height, width in batch
                    ]
                    keys = [(episode, frame) for episode, frame, *_ in batch]
                tensors = torch.stack(images).contiguous()
                assert tensors.dtype == torch.uint8 and tensors.device.type == "cpu"
                frames += len(tensors)
                total_bytes += tensors.numel()
                if validate:
                    for key, tensor in zip(keys, tensors):
                        hashes.append(
                            (
                                *key,
                                list(tensor.shape),
                                hashlib.sha256(tensor.numpy().tobytes()).hexdigest(),
                            )
                        )
            assert frames == count, (frames, count)
            return {"frames": frames, "bytes": total_bytes, "hashes": hashes}

        try:
            first = {}
            samples = {engine: [] for engine in connections}
            for engine in connections:
                start = time.perf_counter()
                execute(engine)
                first[engine] = time.perf_counter() - start
            # Alternate order on each repeat to reduce systematic order bias.
            for repeat in range(args.repeats):
                for engine in list(connections)[:: 1 if repeat % 2 == 0 else -1]:
                    start = time.perf_counter()
                    execute(engine)
                    samples[engine].append(time.perf_counter() - start)
            reference = execute("sql_ffmpeg", validate=True)
            candidate = execute("sql_torchcodec", validate=True)
            assert reference == candidate, "Frame keys, dimensions or pixels differ"
            case = {
                "rows": count,
                "first_seconds": first,
                "warm_seconds": samples,
                "median_seconds": {
                    engine: statistics.median(values)
                    for engine, values in samples.items()
                },
                "pixels_equal": True,
                "validation": reference,
            }
            output["cases"].append(case)
            print(count, case["median_seconds"], "pixels equal", flush=True)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(output, indent=2, default=str) + "\n")
        finally:
            reader.close()
            for con in connections.values():
                con.close()


if __name__ == "__main__":
    main()
