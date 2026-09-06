#!/usr/bin/env python3
"""Validate concurrent mixed-resolution COPY with an independent FFmpeg decoder.

Only the Python standard library, DuckDB, ffprobe and ffmpeg are required.
All COPY statements run in one process to exercise repeated codec lifecycles.
"""

import argparse
import array
import csv
import io
import json
import struct
import subprocess
import sys
from pathlib import Path

# HWC shapes change between COPY statements as well as between cameras.
LAYOUTS = (
    {"front": (64, 96, 3), "wrist": (96, 128, 3), "depth": (80, 112, 1)},
    {"front": (96, 128, 3), "wrist": (64, 96, 3), "depth": (112, 80, 1)},
    {"front": (80, 112, 3), "wrist": (64, 128, 3), "depth": (96, 64, 1)},
)
# Narrow depth clips also use gradients spanning all 4096 codes. A width of
# 66 checks the boundary where inter prediction remains enabled.
CASES = tuple((layout, "tiles") for layout in LAYOUTS) + tuple(
    ({"depth": shape}, "ramp")
    for shape in ((64, 16, 1), (64, 32, 1), (96, 64, 1), (96, 66, 1))
)


def quote(value: str | Path) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def run(
    command: list[str], payload: bytes | None = None, log: Path | None = None
) -> bytes:
    result = subprocess.run(command, input=payload, capture_output=True, timeout=180)
    if log is not None:
        log.write_bytes(result.stderr)
    if result.returncode:
        raise RuntimeError(
            f"{command[0]} exited with {result.returncode}:\n{result.stderr.decode(errors='replace')[-16000:]}"
        )
    return result.stdout


def query(duckdb: Path, extension: Path, sql: str) -> list[list[str]]:
    output = run(
        [str(duckdb), "-unsigned", "-csv", "-noheader", "-batch"],
        f"LOAD {quote(extension)}; {sql}".encode(),
    )
    return list(csv.reader(io.StringIO(output.decode())))


def little_endian_shorts(values: list[int]) -> bytes:
    data = array.array("H", values)
    if sys.byteorder != "little":
        data.byteswap()
    return data.tobytes()


def depth_values(shape: tuple[int, int, int], index: int, pattern: str) -> list[int]:
    height, width, _ = shape
    if pattern == "ramp":
        return [
            (x * 37 + y * 11 + index * 137) % 4096
            for y in range(height)
            for x in range(width)
        ]
    # Quarter-metre inputs have exact binary normalization in [1, 5] metres.
    # Each of the 12 frames differs, with an asymmetric spatial pattern.
    return [
        1000 + 250 * (1 + (x // 8 + 3 * (y // 8) + index) % 15)
        for y in range(height)
        for x in range(width)
    ]


def raw_frame(
    key: str, shape: tuple[int, int, int], index: int, depth_pattern: str
) -> bytes:
    if key == "depth":
        return little_endian_shorts(depth_values(shape, index, depth_pattern))
    height, width, _ = shape
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            if key == "front":
                pixels.extend(
                    (
                        30 + index * 14 + x // 4,
                        40 + index * 9 + y // 4,
                        100 - index * 5 + (x + y) // 8,
                    )
                )
            else:
                pixels.extend(
                    (
                        200 - index * 10 - x // 4,
                        20 + index * 15 + y // 4,
                        35 + index * 12 + (x + y) // 8,
                    )
                )
    return bytes(pixels)


def write_datasets(
    duckdb: Path,
    extension: Path,
    workspace: Path,
    codec: str,
    fps: int,
    episode_lengths: list[int],
) -> list[Path]:
    statements = [f"LOAD {quote(extension)}; SET threads = 3;"]
    roots = []
    for iteration, (layout, depth_pattern) in enumerate(CASES):
        root = workspace / f"dataset-{iteration}"
        roots.append(root)
        features = {
            key: {
                "dtype": "video",
                "shape": shape,
                "names": ["height", "width", "channels"],
                **({"info": {"is_depth_map": True}} if key == "depth" else {}),
            }
            for key, shape in layout.items()
        }
        rows = []
        identities = [
            (episode, frame)
            for episode, length in enumerate(episode_lengths)
            for frame in range(length)
        ]
        for index, (episode, frame) in enumerate(identities):
            blobs = [
                f"from_hex('{raw_frame(key, shape, index, depth_pattern).hex()}')"
                for key, shape in layout.items()
            ]
            rows.append(f"({episode}, {frame}, {','.join(blobs)})")
        columns = ", ".join(layout)
        depth_min, depth_max = (0, 4.095) if depth_pattern == "ramp" else (1, 5)
        statements.append(
            f"COPY (SELECT episode::BIGINT AS episode_index, 'mixed cameras' AS task, {columns} "
            f"FROM (VALUES {','.join(rows)}) input(episode, frame, {columns}) ORDER BY episode, frame) "
            f"TO {quote(root)} (FORMAT lerobot, FPS {fps}, FEATURES {quote(json.dumps(features))}, "
            f"VIDEO_WORKERS 3, ENCODER_THREADS 3, RGB_CODEC {quote(codec)}, RGB_CRF 18, "
            f"DEPTH_MIN {depth_min}, DEPTH_MAX {depth_max}, DEPTH_SHIFT 0, DEPTH_USE_LOG false, DEPTH_CLIP false);"
        )
    statements.append("SELECT 42;")
    sql = "\n".join(statements)
    (workspace / "write.sql").write_text(sql)
    run([str(duckdb), "-unsigned", "-batch"], sql.encode(), workspace / "writer.log")
    return roots


def validate_video(
    ffmpeg: str,
    ffprobe: str,
    path: Path,
    key: str,
    shape: tuple[int, int, int],
    depth_pattern: str,
    fps: int,
    frame_count: int,
) -> tuple[bytes, dict]:
    height, width, channels = shape
    probe = json.loads(
        run(
            [
                ffprobe,
                "-v",
                "error",
                "-threads",
                "1",
                "-select_streams",
                "v:0",
                "-show_streams",
                "-show_frames",
                "-show_entries",
                "stream=codec_name,width,height,pix_fmt,nb_frames,r_frame_rate:frame=best_effort_timestamp_time,key_frame",
                "-of",
                "json",
                str(path),
            ]
        )
    )
    assert len(probe["streams"]) == 1, (path, probe)
    stream = probe["streams"][0]
    assert (stream["width"], stream["height"]) == (width, height), (path, stream)
    assert stream["codec_name"] == ("hevc" if key == "depth" else "av1"), (path, stream)
    assert stream["pix_fmt"] == ("gray12le" if key == "depth" else "yuv420p"), (
        path,
        stream,
    )
    assert stream["r_frame_rate"] == f"{fps}/1", (path, stream)
    assert int(stream["nb_frames"]) == frame_count, (path, stream)
    frames = probe["frames"]
    assert len(frames) == frame_count, (path, len(frames))
    for index, frame in enumerate(frames):
        assert abs(float(frame["best_effort_timestamp_time"]) - index / fps) < 1e-6, (
            path,
            index,
            frame,
        )
        if key == "depth" and width <= 64:
            assert frame["key_frame"] == 1, (path, index, frame)

    pixel_format = "gray12le" if key == "depth" else "rgb24"
    decoded = run(
        [
            ffmpeg,
            "-v",
            "error",
            "-threads",
            "1",
            "-i",
            str(path),
            "-map",
            "0:v:0",
            "-an",
            "-sws_flags",
            "bilinear",
            "-pix_fmt",
            pixel_format,
            "-fps_mode",
            "passthrough",
            "-threads",
            "1",
            "-f",
            "rawvideo",
            "pipe:1",
        ]
    )
    frame_bytes = width * height * (2 if key == "depth" else channels)
    assert len(decoded) == frame_count * frame_bytes, (path, len(decoded), frame_bytes)
    max_delta = 0
    max_mean_delta = 0.0
    for index in range(frame_count):
        actual = decoded[index * frame_bytes : (index + 1) * frame_bytes]
        if key == "depth":
            # Independent integer-code oracle: levels / 16 and their products
            # by 4095 are exactly representable, including the ties-to-even case.
            values = depth_values(shape, index, depth_pattern)
            if depth_pattern == "ramp":
                # [0, 4095] mm maps to exactly the integer code range. The
                # float32 arithmetic error is far below half a code unit.
                codes = values
            else:
                codes = [round(((value - 1000) // 250) * 4095 / 16) for value in values]
            expected = little_endian_shorts(codes)
            assert actual == expected, (
                path,
                index,
                "lossless depth codes differ from source quantization",
            )
        else:
            expected = raw_frame(key, shape, index, depth_pattern)
            differences = [abs(a - b) for a, b in zip(actual, expected)]
            maximum = max(differences)
            mean = sum(differences) / len(differences)
            # RGB is lossy YUV420. These smooth patterns keep spatial, camera
            # and frame mismatches well outside these bounded tolerances.
            assert maximum <= 16 and mean <= 3, (path, index, maximum, mean)
            max_delta = max(max_delta, maximum)
            max_mean_delta = max(max_mean_delta, mean)
    return decoded, {
        "file": str(path),
        "frames": frame_count,
        "max_rgb_delta": max_delta,
        "max_rgb_mean_delta": max_mean_delta,
    }


def validate_dataset(
    duckdb: Path,
    extension: Path,
    ffmpeg: str,
    ffprobe: str,
    root: Path,
    layout: dict,
    depth_pattern: str,
    codec: str,
    fps: int,
    episode_lengths: list[int],
) -> dict:
    frame_count = sum(episode_lengths)
    episode_starts = [
        sum(episode_lengths[:episode]) for episode in range(len(episode_lengths))
    ]
    info = json.loads((root / "meta/info.json").read_text())
    assert (info["total_episodes"], info["total_frames"], info["fps"]) == (
        len(episode_lengths),
        frame_count,
        fps,
    ), info
    independent = {}
    metrics = []
    for key, shape in layout.items():
        feature = info["features"][key]
        assert feature["shape"] == list(shape), (key, feature)
        video_info = feature["info"]
        assert (
            video_info["video.height"],
            video_info["video.width"],
            video_info["video.channels"],
        ) == shape
        if key == "depth":
            assert video_info["is_depth_map"] is True
            depth_min, depth_max = (0, 4.095) if depth_pattern == "ramp" else (1, 5)
            for name, expected in {
                "depth_min": depth_min,
                "depth_max": depth_max,
                "shift": 0,
                "use_log": False,
            }.items():
                assert video_info[f"video.{name}"] == expected, (key, video_info)
            assert video_info["video.g"] == (1 if shape[1] <= 64 else 2), video_info
            assert video_info["video.extra_options"]["bf"] == 0, video_info
        else:
            assert video_info["video.preset"] == (
                12 if codec == "libsvtav1" else None
            ), video_info
            assert video_info["video.g"] == 2, video_info
        files = list((root / "videos" / key).rglob("*.mp4"))
        assert len(files) == 1, (root, key, files)
        independent[key], metric = validate_video(
            ffmpeg, ffprobe, files[0], key, shape, depth_pattern, fps, frame_count
        )
        metrics.append(metric)

        routes = query(
            duckdb,
            extension,
            f'SELECT episode_index, length, "videos/{key}/from_timestamp", "videos/{key}/to_timestamp" '
            f"FROM lerobot_episodes({quote(root)}) ORDER BY episode_index;",
        )
        assert len(routes) == len(episode_lengths), (key, routes)
        for index, (episode, length, start, end) in enumerate(routes):
            assert (int(episode), int(length)) == (index, episode_lengths[index]), (
                key,
                routes,
            )
            assert abs(float(start) - episode_starts[index] / fps) < 1e-6, (
                key,
                routes,
            )
            assert (
                abs(float(end) - (episode_starts[index] + episode_lengths[index]) / fps)
                < 1e-6
            ), (
                key,
                routes,
            )

    rows = query(
        duckdb,
        extension,
        f"SELECT episode_index, frame_index, video_key, width, height, channels, decoded_timestamp, hex(image) "
        f"FROM lerobot_video_frames({quote(root)}, {list(range(len(episode_lengths)))}) ORDER BY episode_index, frame_index, video_key;",
    )
    assert len(rows) == frame_count * len(layout), (root, len(rows))
    expected_keys = {
        (episode, frame, key)
        for episode, length in enumerate(episode_lengths)
        for frame in range(length)
        for key in layout
    }
    for episode, frame, key, width, height, channels, timestamp, blob in rows:
        identity = (int(episode), int(frame), key)
        assert identity in expected_keys, (root, identity)
        expected_keys.remove(identity)
        height_expected, width_expected, channels_expected = layout[key]
        assert (int(height), int(width), int(channels)) == (
            height_expected,
            width_expected,
            channels_expected,
        )
        index = episode_starts[int(episode)] + int(frame)
        assert abs(float(timestamp) - index / fps) < 1e-6, (root, identity, timestamp)
        actual = bytes.fromhex(blob)
        if key == "depth":
            values = depth_values(layout[key], index, depth_pattern)
            expected = struct.pack(f"<{len(values)}f", *values)
            assert actual == expected, (
                root,
                identity,
                "decoded millimetres differ from source",
            )
        else:
            size = width_expected * height_expected * channels_expected
            expected = independent[key][index * size : (index + 1) * size]
            assert len(actual) == len(expected), (root, identity, len(actual))
            assert max(abs(a - b) for a, b in zip(actual, expected)) <= 2, (
                root,
                identity,
                "readers disagree",
            )
    assert not expected_keys, expected_keys
    return {"root": str(root), "decoded_rows": len(rows), "videos": metrics}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", required=True, type=Path)
    parser.add_argument("--extension", required=True, type=Path)
    parser.add_argument("--codec", required=True, choices=("libsvtav1", "libaom-av1"))
    parser.add_argument("--fps", type=int, default=10)
    parser.add_argument(
        "--episode-lengths",
        type=int,
        nargs="+",
        default=[4, 4, 4],
        help="Positive frame counts totaling 12; use 1 2 3 6 to test unequal fragments",
    )
    parser.add_argument(
        "--workdir",
        required=True,
        type=Path,
        help="New directory for fixtures, logs and validation results",
    )
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()
    if (
        args.fps <= 0
        or any(length <= 0 for length in args.episode_lengths)
        or sum(args.episode_lengths) != 12
    ):
        parser.error(
            "FPS and episode lengths must be positive, and episode lengths must total 12"
        )
    duckdb, extension, workspace = (
        args.duckdb.resolve(),
        args.extension.resolve(),
        args.workdir.resolve(),
    )
    workspace.mkdir(parents=True, exist_ok=False)
    versions = {
        "codec": args.codec,
        "fps": args.fps,
        "episode_lengths": args.episode_lengths,
        "duckdb": query(duckdb, extension, "SELECT version();")[0][0],
        "ffmpeg": run([args.ffmpeg, "-version"]).decode().splitlines()[0],
        "ffprobe": run([args.ffprobe, "-version"]).decode().splitlines()[0],
    }
    (workspace / "versions.json").write_text(json.dumps(versions, indent=2) + "\n")
    roots = write_datasets(
        duckdb, extension, workspace, args.codec, args.fps, args.episode_lengths
    )
    results = [
        validate_dataset(
            duckdb,
            extension,
            args.ffmpeg,
            args.ffprobe,
            root,
            layout,
            pattern,
            args.codec,
            args.fps,
            args.episode_lengths,
        )
        for root, (layout, pattern) in zip(roots, CASES)
    ]
    (workspace / "validation.json").write_text(json.dumps(results, indent=2) + "\n")
    assert query(duckdb, extension, "SELECT 42;") == [["42"]]
    encodes = sum(len(layout) * len(args.episode_lengths) for layout, _ in CASES)
    frame_count = sum(result["decoded_rows"] for result in results)
    print(
        f"Mixed video conformance passed ({args.codec}): {len(CASES)} COPYs in one process, {encodes} episode/camera encodes, {frame_count} decoded frames."
    )


if __name__ == "__main__":
    main()
