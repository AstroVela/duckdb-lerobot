#!/usr/bin/env python3
"""Compare decoded data, not compressed bytes, with the pinned LeRobot reader/writer."""

import argparse
import importlib.metadata
import json
import subprocess
import tempfile
from pathlib import Path

import numpy as np
import torch
from PIL import Image
from lerobot.configs.video import DepthEncoderConfig
from lerobot.datasets.depth_utils import dequantize_depth, quantize_depth
from lerobot.datasets.lerobot_dataset import LeRobotDataset

from test_bidirectional import LEROBOT_VERSION, run_duckdb, sql_quote

SIZE = 64
FEATURES = {
    "rgb": {"dtype": "video", "shape": (SIZE, SIZE, 3), "names": ["height", "width", "channels"]},
    "depth": {
        "dtype": "video",
        "shape": (SIZE, SIZE, 1),
        "names": ["height", "width", "channels"],
        "info": {"is_depth_map": True},
    },
}
QUANTIZERS = (
    {"depth_min": 0.01, "depth_max": 10.0, "shift": 3.5, "use_log": True},
    {"depth_min": 0.5, "depth_max": 6.5, "shift": 1.25, "use_log": True},
    {"depth_min": 0.5, "depth_max": 6.5, "shift": 0.0, "use_log": False},
)


def raw_frame(index: int) -> dict[str, np.ndarray]:
    y, x = np.indices((SIZE, SIZE))
    rgb = np.stack(
        ((x * 3 + index * 15) % 256, (y * 3 + index * 10) % 256, ((x + y) * 2 + index * 5) % 256), axis=-1
    ).astype(np.uint8)
    depth = (750 + x * 40 + y * 20 + index * 150).astype(np.uint16)[..., None]
    return {"rgb": rgb, "depth": depth}


def create_reference(root: Path, quantizer: dict) -> None:
    dataset = LeRobotDataset.create(
        repo_id="conformance/visual-reference",
        root=root,
        fps=10,
        features=FEATURES,
        video_backend="pyav",
        encoder_threads=1,
        # The reference's default open GOPs fail its own random seeks at short
        # concatenated episode boundaries. Keep lossless codes, use closed GOPs.
        depth_encoder=DepthEncoderConfig(
            **quantizer, extra_options={"x265-params": "lossless=1:open-gop=0:pools=none:frame-threads=1"}
        ),
    )
    for episode in range(2):
        for frame in range(3):
            dataset.add_frame({"task": f"task {episode}", **raw_frame(episode * 3 + frame)})
        dataset.save_episode()
    dataset.finalize()


def create_extension(duckdb: Path, extension: Path, root: Path, quantizer: dict, codec: str) -> None:
    values = []
    for episode in range(2):
        for frame in range(3):
            pixels = raw_frame(episode * 3 + frame)
            values.append(
                f"({episode}, {frame}, 'task {episode}', "
                f"from_hex('{pixels['rgb'].tobytes().hex()}'), "
                f"from_hex('{pixels['depth'].astype('<u2').tobytes().hex()}'))"
            )
    # Keep large BLOB literals off argv; pass SQL through standard input.

    sql = (
        f"LOAD {sql_quote(extension)}; SET threads=2; COPY (SELECT episode::BIGINT AS episode_index, "
        f"task, rgb, depth FROM (VALUES {','.join(values)}) t(episode, frame, task, rgb, depth) "
        f"ORDER BY episode, frame) TO {sql_quote(root)} (FORMAT lerobot, FPS 10, VIDEO_WORKERS 1, "
        f"ENCODER_THREADS 1, FEATURES {sql_quote(json.dumps(FEATURES))}, RGB_CODEC {sql_quote(codec)}, "
        f"DEPTH_MIN {quantizer['depth_min']}, DEPTH_MAX {quantizer['depth_max']}, "
        f"DEPTH_SHIFT {quantizer['shift']}, DEPTH_USE_LOG {str(quantizer['use_log']).lower()});"
    )
    subprocess.run([str(duckdb), "-unsigned", "-batch"], input=sql, text=True, capture_output=True, check=True)


def compare_readers(duckdb: Path, extension: Path, root: Path, quantizer: dict) -> None:
    # Check the write-side transform against the reference quantizer as well as
    # comparing readers. Two readers agreeing on the same wrongly encoded file
    # would otherwise hide a write-side quantization or metadata mistake.
    info = json.loads((root / "meta/info.json").read_text())["features"]["depth"]["info"]
    for name, value in quantizer.items():
        assert info[f"video.{name}"] == value, (name, info)
    for unit in ("mm", "m"):
        reference = LeRobotDataset(
            repo_id="conformance/visual",
            root=root,
            video_backend="pyav",
            return_uint8=True,
            depth_output_unit=unit,
        )
        rows = run_duckdb(
            duckdb,
            extension,
            f"SELECT episode_index, frame_index, video_key, width, height, channels, hex(image) "
            f"FROM lerobot_video_frames({sql_quote(root)}, [0,1], depth_output_unit := '{unit}') "
            "ORDER BY episode_index, frame_index, video_key;",
        )
        assert len(rows) == 12, len(rows)
        for episode, frame, key, width, height, channels, blob in rows:
            assert (int(width), int(height), int(channels)) == (SIZE, SIZE, 1 if key == "depth" else 3)
            actual = np.frombuffer(bytes.fromhex(blob), dtype="<f4" if key == "depth" else "u1")
            actual = actual.reshape(SIZE, SIZE, int(channels))
            expected = reference[int(episode) * 3 + int(frame)][key].numpy().transpose(1, 2, 0)
            if key == "depth":
                np.testing.assert_allclose(actual, expected, rtol=0, atol=1e-6 if unit == "m" else 0)
                raw = raw_frame(int(episode) * 3 + int(frame))["depth"][..., 0]
                codes = quantize_depth(raw, **quantizer, video_backend="numpy")
                # The dataset reader dequantizes Torch float32 tensors. The
                # separate NumPy branch promotes uint16 multiplication through
                # float64 and can differ by one millimetre when rounding.
                physical = dequantize_depth(torch.from_numpy(codes), **quantizer, output_unit=unit, output_tensor=False)
                np.testing.assert_allclose(
                    actual[..., 0],
                    physical.reshape(SIZE, SIZE),
                    rtol=0,
                    atol=1e-6 if unit == "m" else 0,
                    err_msg=f"{root.name}, episode={episode}, frame={frame}, unit={unit}",
                )
            else:
                # FFmpeg/PyAV builds can differ by rounding in YUV -> RGB conversion.
                np.testing.assert_allclose(actual.astype(int), expected.astype(int), rtol=0, atol=2)
        # Same windows, including padding at both episode boundaries.
        temporal = LeRobotDataset(
            repo_id="conformance/visual",
            root=root,
            video_backend="pyav",
            return_uint8=True,
            depth_output_unit=unit,
            delta_timestamps={"depth": [-0.1, 0.0, 0.1]},
        )
        rows = run_duckdb(
            duckdb,
            extension,
            f"SELECT request_id, delta_ordinal, is_padding, hex(image) FROM lerobot_video_windows("
            f"{sql_quote(root)}, [{{'request_id':0,'episode_index':0,'frame_index':0}}, "
            "{'request_id':1,'episode_index':1,'frame_index':2}], video_keys := ['depth'], "
            f"delta_timestamps := [-0.1,0.0,0.1], depth_output_unit := '{unit}') "
            "ORDER BY request_ordinal, delta_ordinal;",
        )
        assert len(rows) == 6, len(rows)
        for request, delta, padding, blob in rows:
            item = temporal[0 if request == "0" else 5]
            delta = int(delta)
            assert (padding == "true") == bool(item["depth_is_pad"][delta])
            actual = np.frombuffer(bytes.fromhex(blob), dtype="<f4").reshape(SIZE, SIZE)
            np.testing.assert_allclose(actual, item["depth"][delta, 0].numpy(), rtol=0, atol=1e-6 if unit == "m" else 0)


def compare_images(duckdb: Path, extension: Path, workspace: Path) -> None:
    # Pillow-generated independent fixtures exercise alpha, endianness and float TIFF.
    rgb = raw_frame(0)["rgb"]
    rgba = np.concatenate((rgb, np.full((SIZE, SIZE, 1), 128, dtype=np.uint8)), axis=2)
    images = {
        "rgb.png": rgb,
        "rgba.png": rgba,
        "rgb-sampleformat.tiff": rgb,
        "rgba.tiff": rgba,
        "rgba-deflate.tiff": rgba,
        "depth.tiff": raw_frame(0)["depth"][..., 0],
        "depth-be.tiff": raw_frame(0)["depth"][..., 0].astype(">u2"),
        "depth-float.tiff": raw_frame(0)["depth"][..., 0].astype(np.float32) / 1000,
    }
    for name, array in images.items():
        path = workspace / name
        save_options = {}
        if name == "rgb-sampleformat.tiff":
            save_options["tiffinfo"] = {339: (1, 1, 1)}
        if name == "rgba-deflate.tiff":
            save_options["compression"] = "tiff_adobe_deflate"
        Image.fromarray(array).save(path, **save_options)
        rows = run_duckdb(
            duckdb,
            extension,
            f"SELECT d.dtype, d.channels, hex(d.image) FROM (SELECT lerobot_decode_image(content) d "
            f"FROM read_blob({sql_quote(path)}));",
        )
        assert len(rows) == 1, len(rows)
        dtype, channels, blob = rows[0]
        assert int(channels) == (array.shape[-1] if array.ndim == 3 else 1)
        dtype = {"uint8": "u1", "uint16": "<u2", "float32": "<f4"}[dtype]
        actual = np.frombuffer(bytes.fromhex(blob), dtype=dtype).reshape(array.shape)
        np.testing.assert_array_equal(actual, array)

    # Official reader consumes extension-created PNG/TIFF features in Parquet.
    root = workspace / "extension-images"
    features = {key: {**value, "dtype": "image"} for key, value in FEATURES.items()}
    raw = raw_frame(0)
    sql = (
        f"LOAD {sql_quote(extension)}; COPY (SELECT 0::BIGINT episode_index, 'image' task, "
        f"from_hex('{raw['rgb'].tobytes().hex()}') rgb, "
        f"from_hex('{raw['depth'].astype('<u2').tobytes().hex()}') depth) "
        f"TO {sql_quote(root)} (FORMAT lerobot, FPS 10, FEATURES {sql_quote(json.dumps(features))});"
    )
    subprocess.run([str(duckdb), "-unsigned", "-batch"], input=sql, text=True, capture_output=True, check=True)
    dataset = LeRobotDataset(repo_id="conformance/images", root=root, video_backend="pyav")
    item = dataset[0]
    np.testing.assert_allclose(item["rgb"].numpy().transpose(1, 2, 0), raw["rgb"] / 255, atol=1e-7)
    np.testing.assert_array_equal(item["depth"].numpy().reshape(SIZE, SIZE), raw["depth"][..., 0])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", required=True, type=Path)
    parser.add_argument("--extension", required=True, type=Path)
    args = parser.parse_args()
    assert importlib.metadata.version("lerobot") == LEROBOT_VERSION
    args.duckdb = args.duckdb.resolve()
    args.extension = args.extension.resolve()
    with tempfile.TemporaryDirectory(prefix="lerobot-visual-conformance-") as directory:
        workspace = Path(directory)
        compare_images(args.duckdb, args.extension, workspace)
        for index, quantizer in enumerate(QUANTIZERS):
            root = workspace / f"reference-{index}"
            create_reference(root, quantizer)
            compare_readers(args.duckdb, args.extension, root, quantizer)
            root = workspace / f"extension-{index}"
            create_extension(args.duckdb, args.extension, root, quantizer, "libsvtav1")
            compare_readers(args.duckdb, args.extension, root, quantizer)
        root = workspace / "extension-libaom"
        create_extension(args.duckdb, args.extension, root, QUANTIZERS[0], "libaom-av1")
        compare_readers(args.duckdb, args.extension, root, QUANTIZERS[0])
    print("Visual conformance passed: images, RGB/depth video, custom quantizers, units, and padded windows.")


if __name__ == "__main__":
    main()
