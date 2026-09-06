#!/usr/bin/env python3
"""Check shard boundaries and official readback with identical encoded fragments."""

import argparse
import importlib.metadata
import json
import shutil
import subprocess
import tempfile
from pathlib import Path
from unittest.mock import patch

import numpy as np
import pyarrow.parquet as pq
from lerobot.configs.video import RGBEncoderConfig
from lerobot.datasets.lerobot_dataset import LeRobotDataset
from lerobot.datasets.video_utils import concatenate_video_files

from test_bidirectional import LEROBOT_VERSION, run_duckdb, sql_quote

SIZE = 64
FPS = 10
FEATURES = {
    "camera": {
        "dtype": "video",
        "shape": (SIZE, SIZE, 3),
        "names": ["height", "width", "channels"],
    }
}


def frame(episode: int, index: int) -> np.ndarray:
    y, x = np.indices((SIZE, SIZE))
    grey = (x + y + 30 * episode + 10 * index + 10).astype(np.uint8)
    return np.repeat(grey[..., None], 3, axis=2)


def create_duckdb(
    args: argparse.Namespace, root: Path, source_episodes: list[int], limit_bytes: int
) -> None:
    values = []
    for episode, source in enumerate(source_episodes):
        for index in range(2):
            values.append(
                f"({episode}, {index}, from_hex('{frame(source, index).tobytes().hex()}'))"
            )
    sql = (
        f"LOAD {sql_quote(args.extension)}; SET threads=2;"
        "COPY (SELECT episode::BIGINT AS episode_index, 'shard fixture' AS task, camera "
        f"FROM (VALUES {','.join(values)}) t(episode, frame, camera) ORDER BY episode, frame) "
        f"TO {sql_quote(root)} (FORMAT lerobot, FPS {FPS}, FEATURES {sql_quote(json.dumps(FEATURES))}, "
        f"RGB_CODEC 'libaom-av1', ENCODER_THREADS 1, VIDEO_WORKERS 1, CHUNKS_SIZE 2, "
        f"VIDEO_FILES_SIZE_IN_MB {limit_bytes / (1024 * 1024)!r});"
    )
    result = subprocess.run(
        [str(args.duckdb), "-unsigned", "-batch"],
        input=sql,
        text=True,
        capture_output=True,
        timeout=60,
    )
    assert result.returncode == 0, result.stderr


def create_reference(root: Path, fragments: list[Path], limit_bytes: int) -> None:
    dataset = LeRobotDataset.create(
        repo_id="conformance/shard-reference",
        root=root,
        fps=FPS,
        features=FEATURES,
        video_backend="pyav",
        rgb_encoder=RGBEncoderConfig(vcodec="libaom-av1"),
        encoder_threads=1,
        video_files_size_in_mb=limit_bytes / (1024 * 1024),
    )

    def existing_fragment(video_key: str, episode_index: int) -> Path:
        assert video_key == "camera"
        temporary = root / ".fixture-clips" / str(episode_index)
        temporary.mkdir(parents=True)
        destination = temporary / "episode.mp4"
        shutil.copyfile(fragments[episode_index], destination)
        return destination

    # Substitute only the encoder output. Official save_episode still writes
    # Parquet/metadata, decides rollover, stats the growing shard and remuxes it.
    # Exact same bytes isolate this policy difference from codec/version noise.
    with patch.object(
        dataset.writer, "_encode_temporary_episode_video", side_effect=existing_fragment
    ):
        for episode in range(len(fragments)):
            for index in range(2):
                dataset.add_frame(
                    {"task": "shard fixture", "camera": frame(episode, index)}
                )
            dataset.save_episode(parallel_encoding=False)
    dataset.finalize()


def route_files(root: Path) -> list[tuple[int, int]]:
    rows = pq.read_table(
        sorted((root / "meta/episodes").rglob("*.parquet"))
    ).to_pylist()
    rows.sort(key=lambda row: row["episode_index"])
    return [
        (row["videos/camera/chunk_index"], row["videos/camera/file_index"])
        for row in rows
    ]


def check_frames(args: argparse.Namespace, root: Path, episodes: int) -> None:
    dataset = LeRobotDataset(
        repo_id="conformance/shard-read",
        root=root,
        download_videos=False,
        video_backend="pyav",
    )
    assert dataset.num_frames == 2 * episodes
    rows = run_duckdb(
        args.duckdb,
        args.extension,
        f"SELECT episode_index, frame_index, hex(image) FROM lerobot_video_frames({sql_quote(root)}, "
        f"{list(range(episodes))}) ORDER BY episode_index, frame_index;",
    )
    assert len(rows) == 2 * episodes
    for ordinal, (episode, index, encoded) in enumerate(rows):
        episode, index = int(episode), int(index)
        assert (episode, index) == divmod(ordinal, 2)
        actual = np.frombuffer(bytes.fromhex(encoded), dtype=np.uint8).reshape(
            SIZE, SIZE, 3
        )
        item = dataset[ordinal]
        assert (
            int(item["episode_index"]) == episode and int(item["frame_index"]) == index
        )
        expected = np.rint(item["camera"].numpy().transpose(1, 2, 0) * 255).astype(
            np.uint8
        )
        source = frame(episode, index)
        assert np.abs(actual.astype(int) - expected.astype(int)).max() <= 2
        assert np.abs(expected.astype(int) - source.astype(int)).max() <= 8


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", required=True, type=Path)
    parser.add_argument("--extension", required=True, type=Path)
    args = parser.parse_args()
    assert importlib.metadata.version("lerobot") == LEROBOT_VERSION
    with tempfile.TemporaryDirectory(prefix="lerobot-shard-boundary-") as temporary:
        workspace = Path(temporary)
        fragments = []
        for episode in range(3):
            root = workspace / f"fragment-{episode}"
            create_duckdb(args, root, [episode], 1024 * 1024)
            fragments.append(next((root / "videos").rglob("*.mp4")))
        sizes = [path.stat().st_size for path in fragments]
        prefix = workspace / "reference-prefix.mp4"
        concatenate_video_files(fragments[:2], prefix)
        reference_projection = prefix.stat().st_size + sizes[2]
        raw_projection = sum(sizes)
        assert reference_projection < raw_projection, (sizes, prefix.stat().st_size)
        threshold = (reference_projection + raw_projection) // 2
        assert threshold > sum(sizes[:2])
        reference = workspace / "reference"
        actual = workspace / "duckdb"
        create_reference(reference, fragments, threshold)
        create_duckdb(args, actual, [0, 1, 2], threshold)
        assert route_files(reference) == [(0, 0)] * 3
        assert route_files(actual) == [(0, 0), (0, 0), (0, 1)]
        check_frames(args, reference, 3)
        check_frames(args, actual, 3)
        # Exact +/- one byte boundaries come from real clips, not an arbitrary
        # hardcoded size. Both writers roll over at >= for their first pair.
        for offset in (-1, 0, 1):
            limit = sum(sizes[:2]) + offset
            reference = workspace / f"reference-boundary-{offset}"
            actual = workspace / f"duckdb-boundary-{offset}"
            create_reference(reference, fragments[:2], limit)
            create_duckdb(args, actual, [0, 1], limit)
            expected = [(0, 0), (0, 1) if offset <= 0 else (0, 0)]
            assert route_files(reference) == route_files(actual) == expected
            check_frames(args, reference, 2)
            check_frames(args, actual, 2)
        # A single episode may exceed the limit; it must remain whole. Force
        # every following episode to roll over, including a chunk boundary.
        actual = workspace / "duckdb-oversize"
        create_duckdb(args, actual, [0, 1, 2], 1)
        assert route_files(actual) == [(0, 0), (0, 1), (1, 0)]
        check_frames(args, actual, 3)
        print(
            json.dumps(
                {
                    "lerobot": LEROBOT_VERSION,
                    "fragment_sizes": sizes,
                    "official_merged_prefix_bytes": prefix.stat().st_size,
                    "difference_threshold_bytes": threshold,
                    "datasets_checked": 9,
                    "frames_checked_by_both_readers": 42,
                },
                indent=2,
            )
        )


if __name__ == "__main__":
    main()
