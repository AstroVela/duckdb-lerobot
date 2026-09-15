#!/usr/bin/env python3
"""Prepare immutable local datasets outside measurement; requires the duckdb environment."""

import argparse
import json
import shutil
import sys
from pathlib import Path

import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from benchmarks.common import (
    digest,
    file_hash,
    load_config,
    payload_files,
    quote,
    write_json,
)  # noqa: E402


def synthetic(root, extension):
    import duckdb

    features = json.dumps(
        {
            "observation.image": {
                "dtype": "video",
                "shape": [24, 32, 3],
                "names": ["height", "width", "channels"],
            }
        }
    )
    with duckdb.connect(config={"allow_unsigned_extensions": True}) as connection:
        connection.execute(f"LOAD {quote(extension)}")
        connection.execute(f"""
            COPY (SELECT (i // 4)::BIGINT AS episode_index, 'smoke' AS task,
                         from_hex(repeat(lpad(hex(i * 17), 2, '0') || '2040', 32 * 24))
                             AS "observation.image"
                  FROM range(12) frames(i) ORDER BY i)
            TO {quote(root)} (FORMAT lerobot, FPS 10, ENCODER_THREADS 1, FEATURES {quote(features)})
        """)


def table(root, prefix):
    return pa.concat_tables(
        [pq.read_table(p) for p in sorted((root / prefix).rglob("*.parquet"))]
    )


def multi_file(source, destination, cameras):
    """Route each existing episode to its own data/video file, preserving timestamps/pixels.

    Video containers are copied whole: this tests routing across physical files,
    not a more diverse corpus. Episode statistics remain valid and unchanged.
    """
    shutil.copytree(source / "meta", destination / "meta")
    info = json.loads((source / "meta/info.json").read_text())
    episodes = table(source, "meta/episodes")
    frames = table(source, "data")
    routes = episodes.to_pylist()
    for number, episode in enumerate(routes):
        selected = frames.filter(
            pc.equal(frames["episode_index"], episode["episode_index"])
        )
        path = destination / f"data/chunk-000/file-{number:03d}.parquet"
        path.parent.mkdir(parents=True, exist_ok=True)
        pq.write_table(selected, path)
        episode["data/chunk_index"] = 0
        episode["data/file_index"] = number
        for camera in cameras:
            prefix = f"videos/{camera}"
            original = source / info["video_path"].format(
                video_key=camera,
                chunk_index=episode[prefix + "/chunk_index"],
                file_index=episode[prefix + "/file_index"],
            )
            target = destination / f"{prefix}/chunk-000/file-{number:03d}.mp4"
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(original, target)
            episode[prefix + "/chunk_index"] = 0
            episode[prefix + "/file_index"] = number
        episode["meta/episodes/chunk_index"] = 0
        episode["meta/episodes/file_index"] = 0
    shutil.rmtree(destination / "meta/episodes")
    path = destination / "meta/episodes/chunk-000/file-000.parquet"
    path.parent.mkdir(parents=True)
    pq.write_table(pa.Table.from_pylist(routes, schema=episodes.schema), path)
    info["data_path"] = "data/chunk-{chunk_index:03d}/file-{file_index:03d}.parquet"
    info["video_path"] = (
        "videos/{video_key}/chunk-{chunk_index:03d}/file-{file_index:03d}.mp4"
    )
    info["total_videos"] = len(routes) * len(cameras)
    write_json(destination / "meta/info.json", info)


def inventory(root, cameras):
    info = json.loads((root / "meta/info.json").read_text())
    actual_cameras = [k for k, v in info["features"].items() if v["dtype"] == "video"]
    if set(cameras) != set(actual_cameras):
        raise ValueError(
            "configure all dataset video cameras; no private LeRobot camera filtering"
        )
    frames = (
        table(root, "data")
        .select(["index", "episode_index", "frame_index"])
        .to_pylist()
    )
    frames.sort(key=lambda r: r["index"])
    if [r["index"] for r in frames] != list(range(len(frames))):
        raise ValueError("benchmark requires contiguous global dataset indices")
    if len({(r["episode_index"], r["frame_index"]) for r in frames}) != len(frames):
        raise ValueError("duplicate frame keys")
    routes = {}
    for episode in table(root, "meta/episodes").to_pylist():
        routes[str(episode["episode_index"])] = [
            info["video_path"].format(
                video_key=camera,
                chunk_index=episode[f"videos/{camera}/chunk_index"],
                file_index=episode[f"videos/{camera}/file_index"],
            )
            for camera in cameras
        ]
    hashes = payload_files(root)
    return {
        "files": hashes,
        "payload_sha256": digest(hashes),
        "frames": frames,
        "video_routes": routes,
        "fps": info["fps"],
        "camera_shapes": {
            camera: info["features"][camera]["shape"] for camera in cameras
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--source",
        type=Path,
        help="optional cached snapshot; payload hashes must match the pin",
    )
    parser.add_argument(
        "--extension",
        type=Path,
        help="required only to generate the synthetic smoke fixture",
    )
    args = parser.parse_args()
    config = load_config(args.config)
    spec = config["dataset"]
    output = args.output.resolve()
    if output.exists():
        parser.error(
            "output already exists; reuse it with run.py or choose a fresh directory"
        )
    source_hashes = None
    if spec["kind"] == "huggingface":
        if args.source:
            source = args.source.resolve()
        else:
            from huggingface_hub import snapshot_download

            source = Path(
                snapshot_download(
                    spec["repo_id"],
                    repo_type="dataset",
                    revision=spec["revision"],
                    allow_patterns=["meta/**", "data/**", "videos/**"],
                )
            )
        source_hashes = payload_files(source)
        if digest(source_hashes) != spec["source_payload_sha256"]:
            raise ValueError("source snapshot differs from the pinned payload")
        for prefix in ("meta", "data", "videos"):
            shutil.copytree(source / prefix, output / "single" / prefix)
        path = output / "single/meta/info.json"
        info = json.loads(path.read_text())
        for feature in info["features"].values():
            metadata = feature.get("info", {})
            if "video.is_depth_map" in metadata:
                metadata["is_depth_map"] = metadata.pop("video.is_depth_map")
        path.write_text(json.dumps(info, indent=2) + "\n")
        if file_hash(path) != spec["normalized_info_sha256"]:
            raise ValueError("unexpected normalized metadata")
    else:
        if not args.extension:
            parser.error("--extension is required for synthetic data")
        output.mkdir(parents=True)
        synthetic(output / "single", args.extension.resolve())
    multi_file(output / "single", output / "multi", spec["cameras"])
    write_json(
        output / "manifest.json",
        {
            "schema_version": 1,
            "dataset": spec,
            "source_files": source_hashes,
            "preparation": {
                "single": "rename video.is_depth_map to is_depth_map only",
                "multi": "one data shard per episode; copy full source MP4 per episode, preserve offsets",
            },
            "variants": {
                name: inventory(output / name, spec["cameras"])
                for name in ("single", "multi")
            },
        },
    )
    print(output / "manifest.json")


if __name__ == "__main__":
    main()
