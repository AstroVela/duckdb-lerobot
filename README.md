# DuckDB LeRobot extension

`duckdb-lerobot` is an Apache-2.0 DuckDB/Vane extension for querying and
preparing [LeRobot](https://github.com/huggingface/lerobot) robot-learning
datasets from SQL. It is developed by AstroVela.

The project keeps the official `duckdb/duckdb` dependency as a submodule,
following the repository structure used by AstroVela's `duckdb-iceberg` fork.
Vane consumes release-compatible extension builds through an explicit DuckDB
version compatibility matrix.

## DuckDB version target

The `v1.5-variegata` branch exclusively targets the DuckDB 1.5 Variegata
release line. It currently pins the official DuckDB `v1.5.5` tag
(`d8cdaa33fda8df955cc76ef58a280f68f4cd43fa`), matching the pin used by the
corresponding `duckdb-iceberg` release branch. API changes for DuckDB `main`
belong on a separate extension branch rather than in compatibility shims here.

## Current status

The extension exposes native dataset-root scans for LeRobot v3 episode, task,
and frame data. `lerobot_info`, `lerobot_episodes`, and `lerobot_tasks` use bind
replacement to plan native JSON or Parquet scans with a deliberately small
parameter whitelist. `lerobot_scan` resolves the authoritative
`info.json.data_path` file list through the route cache and binds that list
directly to native `read_parquet`. DuckDB therefore retains its native
projection, filter, dynamic-filter, parallel I/O, footer-cache, and row-group
pruning paths without assuming a `data/` directory. `lerobot_scan` and
`lerobot_episodes` expose only the Parquet options `union_by_name`,
`binary_as_string`, `filename`, and `file_row_number`; `refresh` and
`episode_indices` remain LeRobot-owned.

Frame scans cache the small LeRobot control plane: `meta/info.json` plus each
episode's `length` and data-shard mapping from `meta/episodes`. The cache
resolves custom `data_path` templates and hands either the complete or selected
deduplicated file list to DuckDB before Parquet binding.

The current v3 `total_episodes`, `total_frames`, and `total_tasks` fields are
authoritative. A zero-episode dataset has only its committed `meta/info.json`;
no synthetic empty Parquet files are written. Metadata caches skip episode
I/O, while frame and episode scans derive zero-row schemas from
`info.json.features`; the task scan publishes the fixed v3 task schema.

Video routing is available as a metadata-only second stage. It resolves each
requested episode and video feature to its full MP4 path and timestamp range,
without opening or decoding the MP4. Its episode-camera routes live in a
separate lazy cache, so a frame-only query does not pay their memory or metadata
projection cost. Neither cache duplicates Parquet footers, row-group
statistics, or data blocks; those remain native DuckDB responsibilities.

Native video decode is available as the third stage. `lerobot_video_frames`
streams only the selected episodes' frame timestamps, adds each episode's video
offset, and routes targets into bounded per-MP4-shard buffers. Targets are
timestamp-sorted inside each buffer and split at gaps larger than 10 seconds or
before their FPS-scaled span exceeds 20,000 frames. Decoder sessions stay open
across buffers, continue forward while timestamps are nearby and the accumulated
cluster remains within that span, and otherwise seek backward to the preceding
keyframe. A bounded LRU pool closes least-recently-used idle decoders when more
shards are encountered than can remain open. Results are small Arrow-compatible
batches of interleaved RGB24 or single-channel float32 depth bytes, not Python
image objects.

Native dataset creation is available through DuckDB's COPY surface. `FORMAT
lerobot` delegates data, episode metadata, and task tables to DuckDB's native
Parquet writer, while the extension owns LeRobot's episode boundaries, shard
rotation, statistics, visual encoding, and final metadata commit. The writer
uses a sibling staging directory and publishes the dataset root only after all
Parquet, image/video, `stats.json`, and `info.json` work succeeds. It is
create-only and requires contiguous episodes ordered from zero. Official
LeRobot also supports resuming recording;
that append workflow is outside this COPY contract.

`lerobot_video_windows` is the training-oriented entry point. It accepts an
ordered list of `(request_id, episode_index, frame_index)` structs plus LeRobot
`delta_timestamps`. Delta values are validated against the dataset FPS, target
frames are clamped to episode `length`, and padding is reported explicitly. The
function reads each clamped frame's authoritative Parquet timestamp before
mapping it into the MP4 shard. Request and delta ordinals survive the internal
shard/time reorder, so callers can restore the requested tensor order without
discarding duplicates.

The current C++ implementation materializes at most 100,000 request/delta
pairs as SQL `VALUES` during binding, before camera expansion. A full macro
replacement must preserve these positions and the existing defaults; see the
[migration prerequisites](docs/design.md#window-macro-migration).

`lerobot_video_targets` is the scalable relation entry point. Its second
argument is a DuckDB relation with `request_id`, `episode_index`,
`frame_index`, `video_key`, and `delta_index`, plus an optional `target_id`.
Each row selects one camera and one element of the named `delta_timestamps`
list, so upstream joins, duplicate
requests, and filters remain relational instead of being boxed into a bind-time
list or expanded into a camera/window Cartesian product. The semantic keys
survive shard/time scheduling, and `target_ordinal` keeps even exact duplicates
distinct within a query. `target_ordinal` is assigned by an atomic counter as
parallel workers consume input; it is neither an input position nor stable
across executions. To restore caller order, provide an order-bearing
`target_id`. This non-NULL `BIGINT` is returned unchanged as an additional
output column, including repeated IDs; choose unique IDs when individual
occurrences need a rejoin key. Omitting it preserves the original output
schema. Padding and the resolved target frame are returned explicitly.

`lerobot_temporal_targets` applies the same LeRobot FPS validation,
Python-compatible ties-to-even integer frame-offset conversion,
episode-boundary clamp, and padding semantics without opening video. Its input
relation has `request_id`, `episode_index`, `frame_index`, and `delta_index`,
with the same optional `target_id` contract and unordered `target_ordinal`.
Join the returned `target_frame_index` to `lerobot_scan` to fetch any state,
action, or other non-video feature.
`lerobot_tasks` scans the current v3 `meta/tasks.parquet` contract directly, so
joining a frame's `task_index` yields its `task` string while retaining native
Parquet projection and filter pushdown. Legacy task layouts are not inferred.

Dataset roots are explicit: relative local paths stay local, and Hugging Face
datasets must use an `hf://datasets/...` URI. Remote `hf://` reads require the
host's `httpfs` extension; released DuckDB
builds can normally auto-install and auto-load it, or it can be loaded
explicitly before `lerobot`.

```sql
INSTALL httpfs;
LOAD httpfs;
LOAD lerobot;

-- Read all v3 episode-metadata shards through DuckDB's native Parquet scan.
SELECT * FROM lerobot_episodes('hf://datasets/lerobot/droid_1.0.1');

-- Read the current v3 task-index to task-text mapping.
SELECT task_index, task
FROM lerobot_tasks('hf://datasets/lerobot/droid_1.0.1');

-- Read the authoritative version and path-template metadata.
SELECT * FROM lerobot_info('hf://datasets/lerobot/droid_1.0.1');

-- Read all authoritative frame shards while retaining native Parquet pushdown.
SELECT episode_index, frame_index, timestamp
FROM lerobot_scan('hf://datasets/lerobot/droid_1.0.1');

-- Expand only the episode rows selected by an earlier metadata query.
SELECT *
FROM lerobot_scan(
  'hf://datasets/lerobot/droid_1.0.1',
  episode_indices := [4, 7, 12]
);

-- Resolve the cameras for those episodes without opening the video files.
-- Omit video_keys to select every video-valued feature in info.json.
SELECT episode_index, video_key, video_path,
       from_timestamp, to_timestamp, fps
FROM lerobot_video_routes(
  'hf://datasets/lerobot/droid_1.0.1',
  [4, 7, 12],
  video_keys := ['observation.images.wrist']
);

-- Decode selected frame rows. RGB image values are raw HWC RGB24. Depth image
-- values are HWC, single-channel, little-endian float32 physical depths.
-- Dimensions and the timestamp actually selected by FFmpeg are returned too.
SELECT episode_index, frame_index, video_key,
       video_timestamp, decoded_timestamp,
       width, height, channels, image
FROM lerobot_video_frames(
  'hf://datasets/lerobot/droid_1.0.1',
  [0],
  video_keys := ['observation.images.wrist'],
  frame_indices := [0, 1, 2],
  width := 320,
  height := 240
);

-- Decode an ordered temporal observation window. The scheduler may emit rows
-- in shard order; request_ordinal and delta_ordinal are the stable rejoin keys.
SELECT request_id, request_ordinal, delta_ordinal, is_padding,
       target_frame_index, decoded_timestamp, image
FROM lerobot_video_windows(
  'hf://datasets/lerobot/droid_1.0.1',
  [
    struct_pack(request_id := 1001, episode_index := 12, frame_index := 45),
    struct_pack(request_id := 1002, episode_index := 12, frame_index := 90)
  ],
  video_keys := ['observation.images.wrist'],
  delta_timestamps := [-0.2, -0.1, 0.0]
)
ORDER BY request_ordinal, delta_ordinal;

-- Feed a native relation into the decoder. Filters in this subquery are
-- planned before timestamp lookup and video decode. Every input row chooses
-- exactly one camera and one delta.
WITH targets AS (
  SELECT * FROM (VALUES
    (0::BIGINT, 1001::BIGINT, 12::BIGINT, 45::BIGINT,
     'observation.images.wrist'::VARCHAR, 0::BIGINT),
    (1::BIGINT, 1001::BIGINT, 12::BIGINT, 45::BIGINT,
     'observation.images.wrist'::VARCHAR, 1::BIGINT),
    (2::BIGINT, 1002::BIGINT, 12::BIGINT, 90::BIGINT,
     'observation.images.front'::VARCHAR, 2::BIGINT)
  ) t(target_id, request_id, episode_index, frame_index, video_key, delta_index)
)
SELECT request_id, target_id, video_key, delta_index, is_padding,
       target_frame_index, decoded_timestamp, image
FROM lerobot_video_targets(
  'hf://datasets/lerobot/droid_1.0.1',
  (SELECT * FROM targets),
  delta_timestamps := [-0.2, -0.1, 0.0]
)
ORDER BY target_id;

-- Resolve temporal state/action targets without decoding video. Every input
-- row chooses one delta and carries a stable caller-provided target_id.
WITH requests AS (
  SELECT * FROM (VALUES
    (0::BIGINT, 1001::BIGINT, 12::BIGINT, 45::BIGINT, 0::BIGINT),
    (1::BIGINT, 1001::BIGINT, 12::BIGINT, 45::BIGINT, 1::BIGINT),
    (2::BIGINT, 1001::BIGINT, 12::BIGINT, 45::BIGINT, 2::BIGINT)
  ) t(target_id, request_id, episode_index, frame_index, delta_index)
), targets AS (
  SELECT *
  FROM lerobot_temporal_targets(
    'hf://datasets/lerobot/droid_1.0.1',
    (SELECT * FROM requests),
    delta_timestamps := [-0.2, -0.1, 0.0]
  )
)
SELECT targets.request_id, targets.delta_index, targets.is_padding,
       frames."observation.state", frames.action, tasks.task
FROM targets
JOIN lerobot_scan('hf://datasets/lerobot/droid_1.0.1') frames
  ON frames.episode_index = targets.episode_index
 AND frames.frame_index = targets.target_frame_index
JOIN lerobot_tasks('hf://datasets/lerobot/droid_1.0.1') tasks USING (task_index)
ORDER BY targets.target_id;

-- Inspect existing route-cache entries without loading or validating either
-- component as a side effect.
SELECT *
FROM lerobot_cache_info('hf://datasets/lerobot/droid_1.0.1');
```

## Dataset statistics and embedded images

`lerobot_stats(root, refresh := false)` reads `meta/stats.json` as
`feature VARCHAR, stats JSON`, one row per feature. JSON preserves differing
feature shapes and exact integer extrema. Empty datasets return the same
two-column schema with zero rows without requiring a stats file. A missing or
malformed stats file in a nonempty dataset is an error.

```sql
SELECT feature, stats->'$.mean' AS mean, stats->'$.std' AS std
FROM lerobot_stats('my_dataset');

SELECT decoded.width, decoded.height, decoded.channels, decoded.dtype, decoded.image
FROM (
  SELECT lerobot_decode_image("observation.images.front".bytes) AS decoded
  FROM lerobot_scan('my_dataset')
);

-- External image paths must be resolved explicitly by the caller.
SELECT lerobot_decode_image(content) FROM read_blob('my_dataset/images/frame-000000.png');
```

`lerobot_decode_image(blob)` returns a struct containing `image BLOB`, `width`,
`height`, `channels`, and `dtype`. Bytes are contiguous HWC; supported output
dtypes are `uint8`, little-endian `uint16`, and little-endian `float32`. RGB
and RGBA retain their channels. The first TIFF page is decoded; uncompressed
uint16/float32 grayscale TIFF strips have a native precision-preserving path,
while other supported PNG/TIFF formats use FFmpeg. TIFF sample widths and formats
must be uniform across channels; RGBA TIFF uses unassociated alpha. Signed
integer and compressed float TIFFs are rejected. Non-NULL input requires an
FFmpeg-enabled build; NULL returns NULL.
Encoded input and decoded image sizes each
have a 64 MiB hard limit; these are per-image guards, not a shared memory budget.
Units are not inferred from pixel values: consult the feature's `depth_unit`
metadata. The function consumes bytes and does not follow a feature's `path`.

Video projection also has a precise contract: `image` requires decoding and
pixel conversion; `decoded_timestamp` requires decoding; `width` and `height`
require decoding only when output dimensions were not both supplied. `channels`
comes from feature metadata. Projection of metadata-only columns does not open
MP4 files.

## Native LeRobot v3 write

The COPY query supplies `episode_index`, `task`, and exactly the user features
declared by the `FEATURES` JSON object. The extension generates `timestamp`,
`frame_index`, global `index`, and `task_index`, writes one Parquet row group
per episode, and produces the canonical v3 control plane.

```sql
COPY (
  SELECT episode_index::BIGINT,
         task::VARCHAR,
         state::FLOAT[6] AS "observation.state",
         action::FLOAT[3] AS action,
         camera_rgb::BLOB AS "observation.images.front"
  FROM recorded_frames
  ORDER BY episode_index, source_frame_index
) TO '/datasets/my_robot_run' (
  FORMAT lerobot,
  FPS 30,
  FEATURES '{
    "observation.state": {
      "dtype": "float32", "shape": [6], "names": null
    },
    "action": {
      "dtype": "float32", "shape": [3], "names": null
    },
    "observation.images.front": {
      "dtype": "video", "shape": [480, 640, 3],
      "names": ["height", "width", "channels"]
    }
  }',
  ROBOT_TYPE 'my_robot',
  CHUNKS_SIZE 1000,
  DATA_FILES_SIZE_IN_MB 100,
  VIDEO_FILES_SIZE_IN_MB 200,
  METADATA_BUFFER_SIZE 10,
  MAX_VISUAL_FRAME_BYTES 67108864,
  VIDEO_WORKERS 4,
  ENCODER_THREADS 4
);
```

Visual input is an unboxed HWC frame in a DuckDB `BLOB`: RGB features require
exactly `height * width * 3` uint8 bytes. Depth features set
`info.is_depth_map` to true and require either little-endian uint16 millimetres
or float32 metres. Image features are embedded as PNG/TIFF structs in Parquet.
Video features use LeRobot's current defaults: AV1/yuv420p for RGB and
lossless HEVC/gray12le after 12-bit logarithmic quantization for depth. Depth
uses closed GOPs so independently encoded episodes remain seekable after
concatenation. The writer records the encoder's returned codec and pixel format.
Episode fragments are retained until a video shard closes and then
stream-copy concatenated once. Episode metadata records the resulting
`[from_timestamp, to_timestamp)` routes without repeatedly rewriting a growing
shard prefix.

COPY uses bounded episode memory. Numeric rows live in DuckDB's buffer-managed
collection and statistics are computed in bounded streaming passes. Raw visual
frames are appended to per-feature staging files; visual sampling and video
encoding read one frame at a time. Independent cameras are encoded by a
database-instance bounded worker pool and registered in feature order after the
whole episode batch completes. `VIDEO_WORKERS` limits active camera encoders;
`ENCODER_THREADS` is the total codec-thread budget for one COPY and is divided
across those workers. Both default to `min(4, DuckDB's thread limit)` and cannot
exceed that limit. Concurrent COPY statements share a database-wide admission
limit for the configured codec budgets within that DatabaseInstance.
Codec work deliberately does not occupy DuckDB pipeline workers; unrelated
queries can still consume their own pipeline budget at the same time. These
threads still compete for CPU and memory; this is not admission control for
all read/write queries or an exact cap on threads created internally by codecs.
The single COPY coordinator waits for each episode batch, but performs no codec
CPU work while it waits.
Integer `min`/`max` statistics use LeRobot's native-compatible `BIGINT` leaf in
episode Parquet metadata (`UBIGINT` for the complete `uint64` domain) and retain
their exact decimal value in `stats.json`; they never pass through `DOUBLE`.
Integer `mean`, `std`, and quantiles intentionally use LeRobot's floating-point
reduction contract.
`MAX_VISUAL_FRAME_BYTES` is the strict per-active-camera frame scratch-memory
contract and defaults to 64 MiB. Thus the extension-owned raw encoding scratch
is bounded by `VIDEO_WORKERS * MAX_VISUAL_FRAME_BYTES`; it is not a total
dataset or episode byte limit. Unmaterialized encoded fragments occupy staging
disk, not RAM. Statistics retain only their final output and one flattened row
proportional to the declared feature width; reductions and 5000-bin histograms
process at most 64 dimensions at a time, never an episode-sized value array.

On read, depth video codes are dequantized from the canonical
`video.depth_min`, `video.depth_max`, `video.shift`, and `video.use_log`
metadata. The `image` BLOB is HWC with one little-endian float32 value per pixel.
`depth_output_unit` defaults to `mm`, matching native LeRobot, and accepts `m`
for metres. Depth metadata must contain the canonical marker, quantizer fields,
one-channel shape, and `gray12le` pixel format; no defaults or legacy markers are
inferred.

The destination must be a new local path. There is intentionally no append,
overwrite, legacy-layout inference, or codec fallback in this strict writer.

The following encoding options are independent of worker budgets:

| COPY option | Default | Contract |
| --- | --- | --- |
| `RGB_CODEC` | `'libsvtav1'` | `'libsvtav1'` or `'libaom-av1'`; RGB AV1/yuv420p only; no automatic fallback |
| `RGB_CRF` | `30` | Integer 0–63 |
| `RGB_GOP` | `2` | Positive 32-bit integer |
| `DEPTH_MIN`, `DEPTH_MAX` | `0.01`, `10` | Finite metres, `0 <= min < max` |
| `DEPTH_SHIFT` | `3.5` | Finite metres; logarithmic mapping requires `min + shift > 0` |
| `DEPTH_USE_LOG` | `true` | Select logarithmic or linear 12-bit quantization |
| `DEPTH_CLIP` | `true` | Existing saturation behavior; `false` rejects values outside the configured range |

Depth remains lossless HEVC/gray12le and is unaffected by the RGB options.
NaN/infinite input and invalid logarithm domains are rejected even when clipping
is enabled. Quantizer parameters must remain representable in float32 in both
supported input units and pass the reader's float32 dequantization checks.
They are persisted in `info.json` for dequantization.
Selecting libaom at runtime does not change the license of the linked FFmpeg
build; distribution must be assessed against its actual enabled dependencies.

## Roadmap

See the [review decisions and remaining work](docs/review-followups.md).
The first-release gate is correctness and tested read/write compatibility.
V2 adapters, a full windows macro replacement, shared resource governance,
timestamp caching, append/remote COPY, and hardware acceleration remain
separate projects with explicit acceptance criteria. Execution controls remain
available as named parameters; moving them into SET alone is not resource governance.

## Metadata and Parquet caching

The data and video route caches are separate database-instance `ObjectCache`
entries keyed by normalized dataset root. Entries are immutable and
memory-accounted, so DuckDB can evict them. A size/mtime/version-tag fingerprint
of `meta/info.json` invalidates stale entries automatically; `refresh := true`
invalidates both entries for non-versioned or manually edited datasets, after
which a cache-consuming query rebuilds only what it needs. Versioned Hugging
Face revisions should normally need no explicit refresh.

`lerobot_info`, `lerobot_episodes`, and `lerobot_tasks` register `refresh` in
the current API. Passing `true` invalidates both routing caches at bind time
without rebuilding them; the metadata itself is read by the native scan.
This support was added in `ca5260c`. At `81db162`, these three functions did
not register the parameter: DuckDB rejected it during binding as an invalid
named parameter, and the option-parsing branch was unreachable.

`lerobot_cache_info(root)` always returns one `data` row and one `video` row
with the stable columns `root`, `component`, `cached`, `entries`, and `bytes`.
`entries` is zero or one for the corresponding `ObjectCache` entry, and `bytes`
is its current memory estimate. This is a passive snapshot: it neither reads
storage nor validates, refreshes, or creates a cache entry.

The data route builder projects only `episode_index`, `length`,
`data/chunk_index`, and `data/file_index` from episode metadata. The lazy video
route builder projects only `episode_index`, episode `length`, and each video
feature's chunk, file, and timestamp-range columns. Once the extension has
selected the relevant paths, native `parquet_scan` owns schema/footer reads,
projection and filter pushdown,
row-group min/max pruning, parallel reads, and external-file caching. DuckDB's
optional `parquet_metadata_cache` setting can therefore be used without any
LeRobot-specific footer cache.

Decoded images are deliberately not cached by this extension: they are large,
often consumed once by training, and cache policy belongs with the Vane actor or
model pipeline. Video reads go through DuckDB's `FileSystem`, so local files,
`hf://`, credentials, and any caching provided by the host filesystem use the
same path as native scans. Within one decode query the FFmpeg container and
conversion context are reused for every target on the same shard. Consecutive
requests that select the same decoded frame also reuse its RGB conversion until
the decoder advances or seeks; output vectors still receive independent values.

Model inference, such as hand-pose or reward scoring, remains a Vane GPU actor
UDF concern; this extension owns the data-plane hot path.

See [the reader design](docs/design.md) for the LeRobot and Daft-derived
format, query-planning, and video-alignment invariants.

## Build

Initialize the DuckDB submodule, then build a loadable extension and a DuckDB
shell with the extension preloaded:

```bash
git submodule update --init --recursive
make
```

The metadata and Parquet functions can be built without FFmpeg by explicitly
setting `LEROBOT_ENABLE_FFMPEG=OFF`. Native visual writing and video decoding
are enabled by default, and configuration fails unless `pkg-config` can find
`libavformat`, `libavcodec`, `libavutil`, and `libswscale`; on Debian/Ubuntu the
corresponding development packages can be installed with:

```bash
sudo apt-get install pkg-config libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
```

Use `-DLEROBOT_ENABLE_FFMPEG=OFF` for a metadata-only build. Run the deterministic
H.264 decode test explicitly with:

```bash
LEROBOT_FFMPEG_TESTS=1 make test
```

The native CI matrix pins the DuckDB submodule at v1.5.5/C++11 and runs three
configurations on Ubuntu 24.04: a metadata-only release build, an FFmpeg-enabled
release build with every visual test enabled, and an FFmpeg-enabled
ASAN+UBSAN build. The metadata-only job also installs the pinned official
LeRobot release and runs the bidirectional format test documented in
[`test/conformance/README.md`](test/conformance/README.md). CMake options can be
reproduced locally without replacing the project's standard configure command,
for example:

```bash
LEROBOT_FFMPEG_TESTS=1 make test \
  BUILD_DIR=build/sanitize \
  BUILD_TYPE=RelWithDebInfo \
  EXTRA_CMAKE_ARGS="-DLEROBOT_ENABLE_FFMPEG=ON -DFORCE_ASSERT=ON -DENABLE_SANITIZER=ON -DENABLE_UBSAN=ON"
```

FFmpeg support is fail-closed: configuring with its default
`LEROBOT_ENABLE_FFMPEG=ON` requires all four development libraries. Use
`-DLEROBOT_ENABLE_FFMPEG=OFF` only when intentionally building the
metadata-only variant.

A release is blocked unless the complete native matrix and the pinned
bidirectional conformance job pass, `LICENSE` is present, and every direct
dependency or fixture is accounted for in `THIRD_PARTY_NOTICES.md`. The pull
request template records the same checks before merge.

`frame_indices` is the pre-decode sampling control for the low-level frame API;
training reads should normally use `lerobot_video_windows`. `tolerance`
defaults to `1e-4` seconds like native LeRobot, `cluster_gap` defaults to 10
seconds, and clusters are additionally capped at an estimated 20,000-frame
timestamp span using dataset FPS. Each seek receives that cluster's estimated
work plus a 20,000-frame emergency margin, so the guard detects pathological
decode progress without limiting video duration. `batch_size` defaults to 16
output rows. `max_output_bytes` independently caps image bytes emitted by one
call at 64 MiB (a single larger frame is still emitted).

`target_buffer_size` defaults to 256 alignment targets per shard buffer.
`max_cached_decoders` defaults to 8 decoder sessions, while
`decode_threads` independently defaults to 8 workers. Source timestamp reads
are partitioned by routed Parquet file and run on up to 4 background producer
tasks by default; set `producer_threads` independently to change that limit.
`max_pending_targets` defaults to twice `target_buffer_size * decode_threads`;
producers deschedule at that strict queue bound and consumers reschedule them
after taking a buffer. A partial shard buffer is published before a producer
blocks so even a one-target bound can always make progress.
Actual concurrent decoder work cannot exceed either the worker or
`max_cached_decoders` session limit, but changing one does not silently rewrite
the other.
`codec_threads` defaults to one FFmpeg thread per decoder to avoid nested
oversubscription. `depth_output_unit` defaults to `mm` and can be set to `m`;
both forms use little-endian float32 values in the returned BLOB. Projection
pushdown avoids opening video entirely unless the
query needs `image`, `decoded_timestamp`, or unknown native dimensions, so
metadata-only queries also work in an FFmpeg-disabled build. The relation input
uses DuckDB typed vectors and native child-plan projection/filter pushdown; the
source APIs also evaluate pushed output filters themselves. RGB and dequantized
depth data are written directly into DuckDB `BLOB` vectors.

With JSON profiling enabled, `lerobot_video_frames` and
`lerobot_video_windows` publish source query/task/chunk counts, producer and
consumer waits, queue high-watermark, targets, decoder acquires/cache
hits/opens/evictions, decoder seeks, AVIO seeks, video bytes read, frames
decoded, RGB and depth conversions, and same-frame fan-out hits. See the
[A/B benchmark harness](benchmark/README.md) for matching DuckDB, Daft, and
native LeRobot runs and the current adaptive-threshold/hardware-decode decision
gates.

Vane integration is tested against the matching official DuckDB release; Vane
fork-specific integration stays in the Vane repository rather than leaking
into this portable extension.

## License

The extension source is licensed under the
[Apache License 2.0](LICENSE). Direct dependency licenses, optional FFmpeg codec
implications, and test-fixture provenance are recorded in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## C++ compatibility

Extension-owned code uses DuckDB's native `string`, `vector`, `unique_ptr`,
`make_uniq`, and related helpers. It intentionally stays within the C++11
language subset rather than introducing another STL or runtime abstraction.

The build does not override the language standard selected by DuckDB. The
pinned DuckDB 1.5.5 tree selects `-std=c++11`, so this branch compiles the
extension as C++11 without a second toolchain policy. The writer directly uses
the Variegata `CopyFunction` and Parquet rotation APIs; DuckDB `main` and
Vane-fork API adapters belong on their matching version branches.
