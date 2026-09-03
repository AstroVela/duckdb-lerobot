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
    meta/tasks.parquet
    data/chunk-{chunk_index:03d}/file-{file_index:03d}.parquet
    videos/{video_key}/chunk-{chunk_index:03d}/file-{file_index:03d}.mp4

## Query model

The public API is episode-first:

    SELECT * FROM lerobot_episodes('hf://datasets/org/dataset');
    SELECT task_index, task FROM lerobot_tasks('hf://datasets/org/dataset');
    SELECT * FROM lerobot_info('hf://datasets/org/dataset');

The episode query returns one row per episode, the task query returns the
current task-index mapping, and the info query returns the authoritative info
record. Callers filter episode metadata before the
extension expands frame Parquet rows. This prevents a query that selects a
small set of episodes from eagerly scanning or decoding the entire dataset.

The initial SQL interface for that second phase is:

    SELECT * FROM lerobot_episode_frames(root, [episode_index, ...]);

The ordered training interface is:

    SELECT *
    FROM lerobot_video_windows(
      root,
      [struct_pack(request_id := 7, episode_index := 12, frame_index := 45)],
      delta_timestamps := [-0.2, -0.1, 0.0]
    );

It preserves duplicate requests and duplicate target frames. `request_ordinal`
and `delta_ordinal` describe the caller's logical order; physical output order
is intentionally unspecified because decode work is repartitioned by MP4 shard.
Each delta must be an integer frame offset at the dataset FPS within the same
`1e-4`-second default tolerance used by LeRobot. Offsets outside an episode are
clamped using episode `length` and marked with `is_padding`. The clamped
`target_frame_index` is then joined back to the frame Parquet data so video
alignment uses its real timestamp rather than reconstructing time as
`frame_index / fps`.

For large or dynamically generated target sets, the relation form is preferred:

    SELECT *
    FROM lerobot_video_targets(
      root,
      (SELECT request_id, episode_index, frame_index, video_key, delta_index
       FROM training_targets
       WHERE split = 'train'),
      delta_timestamps := [-0.2, -0.1, 0.0]
    );

The five named input columns are cast by DuckDB and consumed through unified
typed vectors. One row selects one camera/delta pair, avoiding the list API's
bind-time materialization and cross product. Duplicate rows remain duplicate
decode targets. Physical output is shard/time ordered; `target_ordinal` is a
unique rejoin key, while `request_id`, `video_key`, and `delta_index` retain the
caller's semantic identity. Filters and projections inside the input subquery
are optimized before routing and decode.

Non-video temporal features use the corresponding relation operator:

    WITH targets AS (
      SELECT *
      FROM lerobot_temporal_targets(
        root,
        (SELECT request_id, episode_index, frame_index, delta_index
         FROM training_targets),
        delta_timestamps := [-0.2, -0.1, 0.0]
      )
    )
    SELECT targets.*, frames."observation.state", frames.action, tasks.task
    FROM targets
    JOIN lerobot_frames(root) frames
      ON frames.episode_index = targets.episode_index
     AND frames.frame_index = targets.target_frame_index
    JOIN lerobot_tasks(root) tasks USING (task_index);

The four named input columns are cast to `BIGINT` and consumed as DuckDB typed
vectors. Delta timestamps must be integer frame offsets at the dataset FPS;
exact half-frame values use Python-compatible ties-to-even rounding, shared
with both video temporal APIs. The operator looks up the requested episode's
cached `length`, clamps the target into `[0, length - 1]`, and emits
`is_padding`. It does not read feature values itself, so the following native
Parquet join keeps projection and filter pushdown for arbitrary state/action
schemas. `target_ordinal` distinguishes exact duplicate relation rows.
`lerobot_tasks` directly scans the current v3 `meta/tasks.parquet` schema
(`task_index`, `task`); no legacy schema inference or fallback is performed.

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

`lerobot_info`, `lerobot_episodes`, and `lerobot_tasks` are native C++ table
functions. Following DuckDB Iceberg's implementation pattern, each clones a
registered DuckDB scan function set and replaces only its `MultiFileReader`.
The LeRobot reader normalizes the dataset root and expands it to the fixed v3
metadata path. It reads the authoritative totals before expanding Parquet
files. When the relevant total is zero, its custom bind publishes the strict
v3 schema without opening a file; a positive total with no matching file is an
error. As with DuckDB Iceberg's custom root reader, scans that remain on this
reader explicitly do not support plan serialization rather than delegating to
the native serializer, which would persist expanded files and lose
dataset-root metadata semantics.

`lerobot_frames` is a bind-replacement table function. It loads the immutable
base route cache, takes its deduplicated file list resolved from the
authoritative `info.json.data_path` template and episode chunk/file indices,
and binds that list directly as a native `parquet_scan`. No `data/` glob or
alternate-layout fallback exists. The resulting scan remains DuckDB's own
logical get, preserving schema inference, parallel reads, projection and filter
pushdown, join dynamic filters, footer caching, and row-group pruning. Its
function set retains the native Parquet overloads and named parameters; the
bind replacement forwards those parameters after consuming only `refresh`.
For a zero-frame dataset, the replacement falls through to the same LeRobot
`MultiFileReader` empty bind and derives the frame schema from
`info.json.features`.

`lerobot_episode_frames` is a bind-replacement table function. It constructs a
relational `episode_index IN (...)` predicate over a native `parquet_scan`.
Before constructing that relation it resolves the selected episode indices
through the cached v3 metadata route table, so the Parquet scan receives only
the distinct data shards that can contain those episodes. The optimizer still
pushes the row predicate into those files. An empty or unknown episode set uses
one known shard only to bind the output schema, then produces no rows; negative
or NULL indices are rejected during binding.

The base route cache stores the authoritative `codebase_version`, `data_path`,
`video_path`, `fps`, total counts, empty-dataset schemas, and sorted video
feature keys from `meta/info.json`, a
sorted compact episode-to-length/data-file index, and one copy of each resolved
data path. A second lazy cache stores compact episode/key/video-file indices,
timestamps, and one copy of each resolved MP4 path. Keeping the caches separate
means ordinary frame scans never materialize the larger episode-by-camera
table. Both database-instance `ObjectCache` entries are memory-accounted and
immutable. `info.json` size, modification time, and version tag form the
invalidation marker; callers can also pass `refresh := true` to route or cache
functions.

This is intentionally above the Parquet layer. DuckDB continues to own footer
metadata, row-group statistics, the optional `parquet_metadata_cache`, and the
external file/block cache. The extension neither parses nor duplicates those
caches. On a base route-cache miss, its metadata query projects only the four
episode length/data route columns through DuckDB's native Parquet reader. The
video cache is
not populated until `lerobot_video_routes` or
`lerobot_video_metadata_cache` is bound; that query projects only four columns
per video key plus `episode_index` and episode `length`.

Native LeRobot does not create episode, task, data, statistics, or media files
until its first episode is saved. `FORMAT lerobot` preserves that layout: a
zero-row COPY publishes `meta/info.json` with zero totals, and the readers use
that commit marker rather than manufacturing placeholder Parquet footers.

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
a gap greater than 10 seconds starts a new seek cluster. A cluster also ends
before its timestamp span, multiplied by dataset FPS and including both endpoint
frames, would exceed 20,000 estimated frames. This bounds sparse targets whose
adjacent gaps are exactly 10 seconds without shortening the supported video.

The number of queued targets is bounded as well as the size of an individual
shard buffer. If many shards each have a partial buffer, the scheduler flushes
the least-recently-touched partial buffer before reading more Parquet rows. The
source scheduler leases one decoder per shard at a time. Relation pipelines may
hold separate sessions for the same shard when DuckDB runs independent input
chunks concurrently; no FFmpeg object itself is shared. Decode runs in parallel
up to the smaller of `decode_threads` and `max_cached_decoders`.

The DuckDB file handle, FFmpeg container, codec, frames, and RGB conversion
context stay open when a decoder is returned between buffers. A monotonic next
buffer within `cluster_gap` continues the existing forward decode only when the
combined FPS-scaled cluster span remains within 20,000 frames; a backward target,
a larger gap, a span overflow, or an in-buffer cluster boundary seeks backward
to the preceding keyframe. Once a decoded timestamp crosses a target, the
decoder picks the closer of that frame and its predecessor; ties select the
predecessor.
A match farther away than the configured tolerance is rejected. The default
tolerance is `1e-4` seconds, matching current LeRobot; callers can explicitly
select a wider tolerance for approximate legacy media. Seeking uses the video
stream's own time base and starts one tick before the target, then aligns on
frame PTS with best-effort timestamps only as a fallback. The decoded-frame
counter resets per seek. Its limit is derived from the estimated cluster work
plus a 20,000-frame emergency margin, leaving room for keyframe preroll while
still detecting corrupt routing metadata or pathological decode progress.

The table function emits at most 16 rows and 64 MiB of image data per call by
default. Target buffers hold at most 256 entries by default, and a global LRU
retains at most 8 idle or leased decoder sessions. Decode worker count, open
decoder count, queued target count, per-call image bytes, and FFmpeg codec
threads are independent controls. Encountering an additional shard closes the
least-recently-used idle decoder before opening the replacement. Each row
includes the episode-local timestamp, absolute requested timestamp, actual
decoded timestamp, dimensions, channels, and an HWC `BLOB`. RGB rows contain
three interleaved uint8 channels. Depth rows contain one little-endian float32
channel dequantized to millimetres by default or metres when
`depth_output_unit := 'm'` is requested.
Optional width and height use the same two-stage pixel contract as Daft: PyAV
RGB24 conversion at native dimensions followed by Pillow-compatible
nearest-neighbour resize. A `frame_indices` named argument filters the native
Parquet timestamp query before decode; callers should prefer it over an outer
SQL filter for sparse sampling.

Depth routes are identified only by the canonical `info.is_depth_map` marker.
Their one-channel `gray12le` codes are inverted with the exact
`video.depth_min`, `video.depth_max`, `video.shift`, and `video.use_log` values
stored in `info.json`. Missing parameters, legacy markers, other pixel formats,
and out-of-range 12-bit codes are rejected instead of receiving inferred
defaults or a color conversion.

FFmpeg reads through a seekable custom `AVIOContext` backed by DuckDB's
`FileSystem`. This preserves `hf://` and other filesystem extensions, secrets,
and any caching supplied by the host filesystem instead of teaching FFmpeg
about every remote URI scheme. Decoder sessions move between the synchronized
idle LRU and an exclusive worker lease; FFmpeg objects are never shared by two
workers, so decoding needs no global codec lock.

The timestamp-producing Parquet result is fetched by a single producer without
holding the scheduler mutex; decoder workers can finish and claim ready shards
concurrently with remote reads. Partial shard buffers have an O(1) LRU and are
flushed under the global pending-target budget. Projection pushdown is carried
into execution: metadata-only columns, counts, and explicitly requested resize
dimensions do not open FFmpeg. RGB conversion is skipped unless `image` is
projected, and repeated targets selecting the same decoded frame reuse the most
recent conversion.

The standard source functions also accept DuckDB output-filter pushdown and
evaluate the pushed expressions against typed output vectors. Their internal
Parquet result is read through `UnifiedVectorFormat`, and RGB bytes are appended
directly to `BLOB` vectors. The relation function receives upstream filters in
its native child plan; DuckDB currently does not pass output `TableFilterSet`
objects to table-in/out operators, so output predicates remain a normal
downstream filter there.

Decoder work is observable through DuckDB's query profiler: targets, acquires,
cache hits, opens, evictions, decoder seeks, underlying AVIO seeks, bytes read,
decoded frames, RGB conversions, and conversion fan-out hits are query-local
atomic counters. These metrics distinguish open/seek amplification from codec
and pixel-conversion cost instead of relying on wall time alone.

There is no persistent or cross-shard decoded-frame cache. A decoder retains
only its most recent RGB conversion to coalesce repeated targets. The dataset
and video-route `ObjectCache` entries remain the small reusable control plane,
DuckDB owns remote byte/block and Parquet caches, and Vane/model actors decide
whether decoded tensors are worth retaining.

## Native write and commit model

`COPY ... (FORMAT lerobot)` is a registered DuckDB `CopyFunction`. Its input
contract mirrors `LeRobotDataset.add_frame`: every row carries
`episode_index`, `task`, and all user-defined features, while the writer owns
`timestamp`, per-episode `frame_index`, global `index`, and `task_index`.
Episodes must arrive contiguously in ascending order from zero. This makes an
episode boundary explicit in the streaming DuckDB input without adding a
Python-side object protocol.

The implementation follows the useful part of the Iceberg COPY architecture:
data files are produced first and the authoritative metadata is committed
last. All output initially goes to a UUID-named sibling staging directory.
Episode data and metadata are delegated to DuckDB's registered native Parquet
COPY implementation with Snappy compression; every episode collection is one
row group. File-size projection rotates only between episodes, so an episode
is never split across data or video shards. `meta/info.json` is written last in
the staging tree, then a single directory rename publishes the destination.
Any bind, encode, Parquet, or metadata error recursively removes the staging
tree and leaves no readable partial dataset.

Raw image and video frames are spooled per feature under that staging tree
instead of being retained in an episode-sized vector. At an episode boundary,
the writer closes the spools, computes visual statistics by positional reads of
only the rounded-linspace sample, and encodes each camera sequentially with one
raw frame buffer. Numeric statistics scan the episode's buffer-managed column
collection twice per dimension batch: once for the NumPy reduction tree and
once for the 5000-bin histograms. During the first reduction scan, signed and
unsigned integer extrema are also collected as original-typed DuckDB values.
Episode Parquet `min`/`max` use PyArrow-compatible `BIGINT` leaves, except that
`uint64` uses `UBIGINT` to cover its complete domain; aggregate JSON remains
exact beyond `2^53`. Integer mean, standard deviation, and quantiles
deliberately remain floating point. Features wider than 64 dimensions are
processed in fixed batches, preserving the same value order while capping
histogram and reduction scratch space.
`MAX_VISUAL_FRAME_BYTES` (64 MiB by default) bounds each extension-owned raw
frame allocation; raw-frame scratch memory is therefore independent of episode
length and camera count, apart from DuckDB's input/output chunks and
buffer-managed pages.

LeRobot's control-plane rules remain extension-owned: canonical path
templates, task indexing, episode routes, metadata chunk/file indices, and
per-episode plus aggregate statistics. The 5000-bin quantile estimator follows
the native NumPy float32 edge and float64 interpolation semantics, including
the 128-element pairwise reduction tree, rounded-linspace visual sampling, and
the conservative cross-episode quantile envelope. DuckDB still owns Parquet
encoding and footer metadata; the extension does not build a second footer or
row-group index.

Image and video input columns contain raw HWC frames as BLOBs. RGB is uint8
RGB24. A depth feature is marked by `info.is_depth_map` and is either
little-endian uint16 millimetres or float32 metres, inferred once and enforced
for the whole dataset. RGB video uses LeRobot's AV1/yuv420p/GOP-2/CRF-30/
preset-12 defaults. Depth uses the native 12-bit logarithmic quantizer followed
by lossless HEVC gray12le. Per-episode MP4s are stream-copy concatenated, and
their cumulative durations become the episode route boundaries.

The writer is deliberately strict and create-only. It does not infer legacy
schemas, append to an existing root, overwrite output, change codecs when an
encoder is missing, or publish remote object-store paths without a catalog
commit protocol.

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
