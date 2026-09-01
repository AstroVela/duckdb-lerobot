# LeRobot reader design

## Source of truth

The extension follows the LeRobot v3 format and its reference implementation,
not an inferred directory convention:

- [LeRobot v3 format](https://huggingface.co/docs/lerobot/lerobot-dataset-v3)
- [LeRobot metadata implementation](https://github.com/huggingface/lerobot/blob/main/src/lerobot/datasets/dataset_metadata.py)
- [Daft LeRobot reader](https://github.com/Eventual-Inc/Daft/blob/main/daft/datasets/lerobot.py)
- [AstroVela DuckDB Iceberg scan](https://github.com/AstroVela/duckdb-iceberg/blob/main/src/function/scan/iceberg_scan.cpp)

`meta/info.json` is authoritative. In particular, `codebase_version`,
`data_path`, `video_path`, `fps`, and video-valued `features` control how a
dataset is read. The familiar v3 paths are defaults, not an ABI:

    meta/episodes/chunk-{chunk_index:03d}/file-{file_index:03d}.parquet
    data/chunk-{chunk_index:03d}/file-{file_index:03d}.parquet
    videos/{video_key}/chunk-{chunk_index:03d}/file-{file_index:03d}.mp4

## Query model

The public API is episode-first:

    SELECT * FROM lerobot_episodes('hf://datasets/org/dataset');
    SELECT * FROM lerobot_info('hf://datasets/org/dataset');

The first query returns one row per episode; the second returns the
authoritative info record. Callers filter episode metadata before the
extension expands frame Parquet rows. This prevents a query that selects a
small set of episodes from eagerly scanning or decoding the entire dataset.

The initial SQL interface for that second phase is:

    SELECT * FROM lerobot_episode_frames(root, [episode_index, ...]);

`lerobot_info`, `lerobot_episodes`, and `lerobot_frames` are native C++ table
functions. Following DuckDB Iceberg's implementation pattern, each clones a
registered DuckDB scan function set and replaces only its `MultiFileReader`.
The LeRobot reader normalizes the dataset root and expands it to the v3 JSON or
Parquet glob, leaving schema inference, parallel reads, projection pushdown,
filter pushdown, and row-group pruning to DuckDB.

`lerobot_episode_frames` is a bind-replacement table function. It constructs a
relational `episode_index IN (...)` predicate over `lerobot_frames`, rather
than executing or materializing an intermediate result inside the extension.
The optimizer can therefore push that predicate into the Parquet scan. An
empty episode list becomes an empty relation, and negative or NULL indices are
rejected during binding.

The current scanner expands the standard v3 paths shown above. Reading custom
`data_path` and `video_path` templates from the authoritative info record, then
using episode metadata to prune whole frame files, is the next scanner stage.

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

The implementation follows DuckDB extension conventions and uses DuckDB's
container and ownership types rather than `std::vector` or `std::unique_ptr`.
Extension-owned syntax is limited to C++11, but the CMake project inherits its
actual language standard from the host: official DuckDB currently requires
C++17, whereas the Vane-compatible DuckDB tree can still build the extension
with C++11.
