#!/usr/bin/env python3
"""Check every COPY PNG independently with Pillow, including repeated COPY."""

import argparse
import hashlib
import io
import json
import subprocess
import tempfile
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
from PIL import Image


def quote(value: str | Path) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def pixels(height: int, width: int, camera: int, index: int, noise: bool = False) -> np.ndarray:
    if noise:
        return np.random.default_rng(1000 + camera * 100000 + index).integers(
            0, 256, (height, width, 3), dtype=np.uint8
        )
    y, x, channel = np.indices((height, width, 3))
    # Every frame/camera differs; spatially asymmetric, with all byte values.
    return ((x * 13 + y * 29 + channel * 71 + index * 17 + camera * 53) % 256).astype(np.uint8)


def write_input(
    path: Path,
    shapes: list[tuple[int, int]],
    frames: int,
    episodes: int,
    noise: bool = False,
) -> None:
    # Bound the Python fixture's working set independently of dataset size.
    with pq.ParquetWriter(
        path,
        pa.schema(
            [("i", pa.int64()), ("episode_index", pa.int64()), ("task", pa.string())]
            + [(f"camera_{camera}", pa.binary()) for camera in range(len(shapes))]
        ),
    ) as writer:
        for start in range(0, frames * episodes, 32):
            rows = []
            for index in range(start, min(start + 32, frames * episodes)):
                row = {
                    "i": index,
                    "episode_index": index // frames,
                    "task": "PNG reference",
                }
                for camera, (height, width) in enumerate(shapes):
                    row[f"camera_{camera}"] = pixels(height, width, camera, index, noise).tobytes()
                rows.append(row)
            writer.write_table(pa.Table.from_pylist(rows, schema=writer.schema))


def copy_sql(root: Path, shapes: list[tuple[int, int]]) -> str:
    features = {
        f"camera_{camera}": {"dtype": "image", "shape": [height, width, 3]}
        for camera, (height, width) in enumerate(shapes)
    }
    return (
        f"COPY (SELECT * EXCLUDE (i) FROM input ORDER BY i) TO {quote(root)} "
        f"(FORMAT lerobot, FPS 30, FEATURES {quote(json.dumps(features))}); "
    )


def validate(
    root: Path,
    shapes: list[tuple[int, int]],
    frames: int,
    episodes: int,
    noise: bool = False,
) -> dict:
    data_files = sorted((root / "data").rglob("*.parquet"))
    assert data_files
    compressed = hashlib.sha256()
    count = 0
    for path in data_files:
        for batch in pq.ParquetFile(path).iter_batches(batch_size=32):
            for row in batch.to_pylist():
                assert row["index"] == count
                assert row["episode_index"] == count // frames
                assert row["frame_index"] == count % frames
                for camera, (height, width) in enumerate(shapes):
                    entry = row[f"camera_{camera}"]
                    assert entry["path"].endswith(".png")
                    encoded = entry["bytes"]
                    # A fresh decoder for EVERY packet catches accidental APNG
                    # or frame dependencies, as well as stale reused buffers.
                    with Image.open(io.BytesIO(encoded)) as image:
                        assert image.format == "PNG" and image.mode == "RGB"
                        assert image.size == (width, height)
                        assert image.n_frames == 1
                        np.testing.assert_array_equal(
                            np.asarray(image),
                            pixels(height, width, camera, count, noise),
                        )
                    compressed.update(len(encoded).to_bytes(8, "little"))
                    compressed.update(encoded)
                count += 1
    assert count == frames * episodes
    info = json.loads((root / "meta/info.json").read_text())
    assert info["total_frames"] == count and info["total_episodes"] == episodes
    for camera, shape in enumerate(shapes):
        feature = info["features"][f"camera_{camera}"]
        assert feature["dtype"] == "image" and feature["shape"] == [*shape, 3]
    stats = []
    for path in sorted((root / "meta/episodes").rglob("*.parquet")):
        stats.extend(
            {key: value for key, value in row.items() if key.startswith("stats/") or key == "episode_index"}
            for row in pq.read_table(path).to_pylist()
        )
    stats.sort(key=lambda row: row["episode_index"])
    stats.append(json.loads((root / "meta/stats.json").read_text()))
    return {
        "validated_frames": count,
        "validated_images": count * len(shapes),
        "png_sha256": compressed.hexdigest(),
        "statistics_sha256": hashlib.sha256(json.dumps(stats, sort_keys=True, allow_nan=False).encode()).hexdigest(),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", type=Path, required=True)
    parser.add_argument("--extension", type=Path, required=True)
    args = parser.parse_args()
    # Cross two chunks, cross episode boundaries, exercise unaligned rows,
    # single pixels, and change shapes/content in subsequent COPY statements.
    cases = [
        ([(3, 5), (1, 1)], 2051, False),
        ([(17, 31), (64, 96)], 19, True),
        ([(64, 96), (17, 31)], 23, False),
    ]
    with tempfile.TemporaryDirectory(prefix="lerobot-png-conformance-") as directory:
        work = Path(directory)
        for threads in (1, 4):
            sql = f"LOAD {quote(args.extension.resolve())}; SET threads={threads}; "
            roots = []
            for index, (shapes, frames, noise) in enumerate(cases):
                source = work / f"input-{index}.parquet"
                write_input(source, shapes, frames, 2, noise)
                root = work / f"dataset-{threads}-{index}"
                roots.append(root)
                sql += f"CREATE OR REPLACE TEMP TABLE input AS SELECT * FROM read_parquet({quote(source)}); "
                sql += copy_sql(root, shapes)
            # All three COPY statements share one connection/database.
            result = subprocess.run(
                [str(args.duckdb.resolve()), "-unsigned", "-batch"],
                input=sql,
                text=True,
                capture_output=True,
                timeout=120,
            )
            if result.returncode:
                raise RuntimeError(result.stderr)
            for root, (shapes, frames, noise) in zip(roots, cases, strict=True):
                print(
                    f"threads={threads}, {shapes}: {validate(root, shapes, frames, 2, noise)}",
                    flush=True,
                )
    print("PNG conformance passed: 16744 independent, exact Pillow image comparisons.")


if __name__ == "__main__":
    main()
