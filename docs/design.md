# LeRobot reader design

## Source of truth

The extension follows the LeRobot v3 format and its reference implementation,
not an inferred directory convention:

- [LeRobot v3 format](https://huggingface.co/docs/lerobot/lerobot-dataset-v3)
- [LeRobot metadata implementation](https://github.com/huggingface/lerobot/blob/main/src/lerobot/datasets/dataset_metadata.py)
- [Daft LeRobot reader](https://github.com/Eventual-Inc/Daft/blob/main/daft/datasets/lerobot.py)

`meta/info.json` is authoritative. In particular, `codebase_version`,
`data_path`, `video_path`, `fps`, and video-valued `features` control how a
dataset is read. The familiar v3 paths are defaults, not an ABI:

    meta/episodes/chunk-{chunk_index:03d}/file-{file_index:03d}.parquet
    data/chunk-{chunk_index:03d}/file-{file_index:03d}.parquet
    videos/{video_key}/chunk-{chunk_index:03d}/file-{file_index:03d}.mp4

## Query model

The public API is episode-first:

    SELECT * FROM lerobot_episodes('hf://datasets/org/dataset');

It returns one row per episode. Callers filter episode metadata before the
extension expands frame Parquet rows. This prevents a query that selects a
small set of episodes from eagerly scanning or decoding the entire dataset.

The initial `lerobot_episodes` implementation is a C++-registered table
macro over DuckDB's native Parquet scanner. The dedicated scan will replace it
only after it preserves the same relational contract and demonstrates a
measured benefit.

## Metadata required for a v3 episode

- `episode_index`, `length`, and task fields;
- `data/chunk_index` and `data/file_index`;
- `dataset_from_index` and `dataset_to_index`;
- for each camera, `videos/{key}/chunk_index`,
  `videos/{key}/file_index`, `from_timestamp`, and `to_timestamp`.

## Video invariant

MP4 shards contain several episodes. A requested frame is located by:

    absolute_timestamp = videos/{key}/from_timestamp + frame.timestamp

It must never be addressed by `frame_index` alone. The decoder groups batch
rows by full video-file identity, sorts timestamps, clusters nearby targets,
seeks to the preceding keyframe, and decodes forward to the closest frame
within half a frame period. This is the approach Daft uses and is the contract
for Vane's future C++ FFmpeg decoder.

## Compatibility

The first native scanner targets v3. v2.0/v2.1 support is a separate adapter:
those datasets use `meta/episodes.jsonl` and episode-per-file media, so they
must not be silently routed through the v3 scanner.
