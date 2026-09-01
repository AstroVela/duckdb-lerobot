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

Video alignment is a separate metadata-only phase:

    SELECT *
    FROM lerobot_video_routes(
      root,
      [episode_index, ...],
      video_keys := ['observation.images.front', ...]
    );

If `video_keys` is omitted, every feature whose `dtype` is `video` in
`info.json` is selected. The result contains the episode and key, resolved full
MP4 path, chunk/file indices, episode-local `from_timestamp` and
`to_timestamp`, and dataset FPS. Missing all-NULL camera metadata for an
episode is skipped; a partially NULL route is rejected as corrupt metadata.
The function does not open the MP4.

`lerobot_info`, `lerobot_episodes`, and `lerobot_frames` are native C++ table
functions. Following DuckDB Iceberg's implementation pattern, each clones a
registered DuckDB scan function set and replaces only its `MultiFileReader`.
The LeRobot reader normalizes the dataset root and expands it to the v3 JSON or
Parquet glob, leaving schema inference, parallel reads, projection pushdown,
filter pushdown, and row-group pruning to DuckDB.

`lerobot_episode_frames` is a bind-replacement table function. It constructs a
relational `episode_index IN (...)` predicate over a native `parquet_scan`.
Before constructing that relation it resolves the selected episode indices
through the cached v3 metadata route table, so the Parquet scan receives only
the distinct data shards that can contain those episodes. The optimizer still
pushes the row predicate into those files. An empty or unknown episode set uses
one known shard only to bind the output schema, then produces no rows; negative
or NULL indices are rejected during binding.

The base route cache stores the authoritative `codebase_version`, `data_path`,
`video_path`, `fps`, and sorted video feature keys from `meta/info.json`, a
sorted compact episode-to-data-file index, and one copy of each resolved data
path. A second lazy cache stores compact episode/key/video-file indices,
timestamps, and one copy of each resolved MP4 path. Keeping the caches separate
means ordinary frame scans never materialize the larger episode-by-camera
table. Both database-instance `ObjectCache` entries are memory-accounted and
immutable. `info.json` size, modification time, and version tag form the
invalidation marker; callers can also pass `refresh := true` to route or cache
functions.

This is intentionally above the Parquet layer. DuckDB continues to own footer
metadata, row-group statistics, the optional `parquet_metadata_cache`, and the
external file/block cache. The extension neither parses nor duplicates those
caches. On a base route-cache miss, its metadata query projects only the three
data route columns through DuckDB's native Parquet reader. The video cache is
not populated until `lerobot_video_routes` or
`lerobot_video_metadata_cache` is bound; that query projects only four columns
per video key plus `episode_index`.

## Metadata required for a v3 episode

- `episode_index`, `length`, and task fields;
- `data/chunk_index` and `data/file_index`;
- `dataset_from_index` and `dataset_to_index`;
- for each camera, `videos/{key}/chunk_index`,
  `videos/{key}/file_index`, `from_timestamp`, and `to_timestamp`.

## Video decode

MP4 shards contain several episodes. `lerobot_video_routes` implements the
episode-to-shard and timestamp-range portion of the mapping. A requested frame
is then located by:

    absolute_timestamp = videos/{key}/from_timestamp + frame.timestamp

It must never be addressed by `frame_index` alone. `lerobot_video_frames`
uses a streaming DuckDB query for the three alignment columns from only the
routed data shards, then expands the requested camera routes one target at a
time. A global scheduler logically partitions those targets by MP4 shard while
placing them into fixed-size buffers. Each buffer is sorted independently, and
a gap greater than 10 seconds starts a new seek cluster.

The number of queued targets is bounded as well as the size of an individual
shard buffer. If many shards each have a partial buffer, the scheduler flushes
the least-recently-touched partial buffer before reading more Parquet rows. Only
one worker can lease a given shard at a time, while distinct shards decode in
parallel up to `max_open_shards`.

The DuckDB file handle, FFmpeg container, codec, frames, and RGB conversion
context stay open when a decoder is returned between buffers. A monotonic next
buffer within `cluster_gap` continues the existing forward decode; a backward
target, a larger gap, or an in-buffer cluster boundary seeks backward to the
preceding keyframe. Once a decoded timestamp crosses a target, the decoder
picks the closer of that frame and its predecessor; ties select the predecessor.
A match farther away than the configured tolerance is rejected. The default
tolerance is `0.5 / fps`, matching LeRobot and Daft. A no-progress budget of
20,000 decoded frames prevents corrupt timestamp metadata from turning into an
unbounded scan, while resetting after every matched target permits long dense
episodes to stream normally.

The table function emits at most 16 rows per call by default. Target buffers
hold at most 256 entries by default, and a global LRU retains at most 8 idle or
leased decoder sessions. Encountering an additional shard closes the
least-recently-used idle decoder before opening the replacement. Each row
includes the episode-local timestamp, absolute requested timestamp, actual
decoded timestamp, dimensions, three channels, and an interleaved HWC RGB24
`BLOB`.
Optional width and height use the same two-stage pixel contract as Daft: PyAV
RGB24 conversion at native dimensions followed by Pillow-compatible
nearest-neighbour resize. A `frame_indices` named argument filters the native
Parquet timestamp query before decode; callers should prefer it over an outer
SQL filter for sparse sampling.

FFmpeg reads through a seekable custom `AVIOContext` backed by DuckDB's
`FileSystem`. This preserves `hf://` and other filesystem extensions, secrets,
and any caching supplied by the host filesystem instead of teaching FFmpeg
about every remote URI scheme. Decoder sessions move between the synchronized
idle LRU and an exclusive worker lease; FFmpeg objects are never shared by two
workers, so decoding needs no global codec lock.

There is no decoded-frame cache. The dataset and video-route `ObjectCache`
entries remain the small reusable control plane, DuckDB owns remote byte/block
and Parquet caches, and Vane/model actors decide whether decoded tensors are
worth retaining.

## Compatibility

The first native scanner targets v3. v2.0/v2.1 support is a separate adapter:
those datasets use `meta/episodes.jsonl` and episode-per-file media, so they
must not be silently routed through the v3 scanner.

The implementation follows DuckDB extension conventions and uses DuckDB's
container and ownership types rather than `std::vector` or `std::unique_ptr`.
Extension-owned syntax is limited to C++11, but the CMake project inherits its
actual language standard from the host: official DuckDB currently builds as
C++17 and the current Vane tree as C++20. A separate C++11 syntax-only check of
the extension translation units against Vane's headers enforces the portable
subset without incorrectly forcing the whole host engine to C++11.
