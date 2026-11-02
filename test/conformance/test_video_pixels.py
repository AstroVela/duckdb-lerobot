#!/usr/bin/env python3
"""Compare the inline H.264 fixture with same-build FFmpeg RGB and NEAREST pixels.

The reference decoder is the FFmpeg CLI. Pillow independently resizes the RGB
pixels, including non-integral ratios, without a native reader call.
Python, Pillow and FFmpeg are required.
"""

import argparse
import hashlib
import json
import re
import subprocess
import tempfile
from pathlib import Path

from PIL import Image


def quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def command(argv, payload=None):
    result = subprocess.run(argv, input=payload, capture_output=True, timeout=120)
    assert result.returncode == 0, result.stderr.decode(errors="replace")
    return result.stdout


def query(args, sql):
    return json.loads(
        command(
            [str(args.duckdb), "-unsigned", "-json", "-batch"],
            (f"LOAD {quote(args.extension)}; " + sql).encode(),
        )
    )


def nearest(rgb, width, height):
    return Image.frombytes("RGB", (16, 16), rgb).resize((width, height), Image.Resampling.NEAREST).tobytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", type=Path, required=True)
    parser.add_argument("--extension", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()
    print(command([args.ffmpeg, "-version"]).decode(), flush=True)
    source = Path(__file__).resolve().parents[1] / "sql/lerobot_video_frames.test"
    setup = source.read_text().split("# Compare temporal APIs", 1)[0]
    statements = re.findall(r"^statement ok\n(.*?)(?=\n\n|\Z)", setup, re.MULTILINE | re.DOTALL)
    assert len(statements) == 7, len(statements)
    with tempfile.TemporaryDirectory(prefix="lerobot-video-pixels-") as temporary:
        base = Path(temporary)
        sql = "\n".join(statements).replace("{TEST_DIR}", str(base))
        command(
            [str(args.duckdb), "-unsigned", "-batch"],
            (f"LOAD {quote(args.extension)}; " + sql).encode(),
        )
        dataset = base / "lerobot_video_frames"
        pixels = command(
            [
                args.ffmpeg,
                "-v",
                "error",
                "-i",
                str(dataset / "video-000.mp4"),
                "-sws_flags",
                "bilinear",
                "-pix_fmt",
                "rgb24",
                "-f",
                "rawvideo",
                "pipe:1",
            ]
        )
        assert len(pixels) == 4 * 16 * 16 * 3, len(pixels)
        frames = [pixels[i * 768 : (i + 1) * 768] for i in range(4)]
        assert len(set(frames)) == 4
        assert all(len(set(frame)) > 32 for frame in frames), "fixture must have spatial variation"
        for width, height in ((16, 16), (8, 6), (7, 9), (23, 19)):
            rows = query(
                args,
                "SELECT frame_index, width, height, channels, decoded_timestamp, hex(image) AS pixels "
                f"FROM lerobot_video_frames({quote(dataset)}, [0, 1], "
                "video_keys := ['observation.images.front'], frame_indices := [3, 0, 2, 1], "
                f"width := {width}, height := {height}, decode_threads := 2, "
                "target_buffer_size := 1, max_cached_decoders := 1) ORDER BY frame_index;",
            )
            assert len(rows) == 4, rows
            for index, row in enumerate(rows):
                assert (
                    row["frame_index"],
                    row["width"],
                    row["height"],
                    row["channels"],
                    row["decoded_timestamp"],
                ) == (
                    index,
                    width,
                    height,
                    3,
                    index / 2,
                ), row
                actual = bytes.fromhex(row["pixels"])
                expected = nearest(frames[index], width, height)
                assert actual == expected, (
                    width,
                    height,
                    index,
                    hashlib.md5(actual).hexdigest(),
                    hashlib.md5(expected).hexdigest(),
                    sum(a != b for a, b in zip(actual, expected)),
                )
            print(
                f"PASS: four selected H.264 frames, {width}x{height}, exact FFmpeg RGB + NEAREST pixels",
                flush=True,
            )


if __name__ == "__main__":
    main()
