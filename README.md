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

The extension now exposes native dataset-root scans for LeRobot v3 episode,
task, and frame Parquet files. Like the Iceberg extension, it copies DuckDB's
registered `read_json_auto` and `parquet_scan` function sets and injects only a
format-specific `MultiFileReader`. DuckDB therefore retains its native
projection, filter, parallel I/O, and row-group pruning paths.

Episode-scoped scans additionally cache the small LeRobot control plane:
`meta/info.json` plus each episode's `length` and data-shard mapping from
`meta/episodes`. The cache resolves custom `data_path` templates and hands an
explicit, deduplicated file list to DuckDB before Parquet binding.

Video routing is available as a metadata-only second stage. It resolves each
requested episode and video feature to its full MP4 path and timestamp range,
without opening or decoding the MP4. Its episode-camera routes live in a
separate lazy cache, so a frame-only query does not pay their memory or metadata
projection cost. Neither cache duplicates Parquet footers, row-group
statistics, or data blocks; those remain native DuckDB responsibilities.

Native video decode is available as the third stage. `lerobot_video_frames`
streams only the selected episodes' frame timestamps, adds each episode's video
offset, and routes targets into bounded per-MP4-shard buffers. Targets are
timestamp-sorted inside each buffer and split at gaps larger than 10 seconds.
Decoder sessions stay open across buffers, continue forward when timestamps are
nearby, and otherwise seek backward to the preceding keyframe. A bounded LRU
pool closes least-recently-used idle decoders when more shards are encountered
than can remain open. Results are small Arrow-compatible batches of interleaved
RGB24 bytes, not Python image objects.

Native dataset creation is available through DuckDB's COPY surface. `FORMAT
lerobot` delegates data, episode metadata, and task tables to DuckDB's native
Parquet writer, while the extension owns LeRobot's episode boundaries, shard
rotation, statistics, visual encoding, and final metadata commit. The writer
uses a sibling staging directory and publishes the dataset root only after all
Parquet, image/video, `stats.json`, and `info.json` work succeeds. Like the
current LeRobot recorder, it is create-only and requires contiguous episodes
ordered from zero.

`lerobot_video_windows` is the training-oriented entry point. It accepts an
ordered list of `(request_id, episode_index, frame_index)` structs plus LeRobot
`delta_timestamps`. Delta values are validated against the dataset FPS, target
frames are clamped to episode `length`, and padding is reported explicitly. The
function reads each clamped frame's authoritative Parquet timestamp before
mapping it into the MP4 shard. Request and delta ordinals survive the internal
shard/time reorder, so callers can restore the requested tensor order without
discarding duplicates.

`lerobot_video_targets` is the scalable relation entry point. Its second
argument is a DuckDB relation with exactly `request_id`, `episode_index`,
`frame_index`, `video_key`, and `delta_index`. Each row selects one camera and
one element of the named `delta_timestamps` list, so upstream joins, duplicate
requests, and filters remain relational instead of being boxed into a bind-time
list or expanded into a camera/window Cartesian product. The semantic keys
survive shard/time scheduling, and `target_ordinal` keeps even exact duplicates
distinct. Use an order-bearing `request_id` together with `delta_index` and
`target_ordinal` to restore the caller's tensor order. Padding and the resolved
target frame are returned explicitly.

`lerobot_temporal_targets` applies the same LeRobot FPS validation, integer
frame-offset conversion, episode-boundary clamp, and padding semantics without
opening video. Its input relation has exactly `request_id`, `episode_index`,
`frame_index`, and `delta_index`. Join the returned `target_frame_index` to
`lerobot_frames` to fetch any state, action, or other non-video feature.
`lerobot_tasks` scans the current v3 `meta/tasks.parquet` contract directly, so
joining a frame's `task_index` yields its `task` string while retaining native
Parquet projection and filter pushdown. Legacy task layouts are not inferred.

`lerobot_layout(root)` and `lerobot_v3_shard_paths(...)` expose the canonical
layout without touching storage. A bare Hugging Face repository ID such as
`lerobot/droid_1.0.1` is normalized to `hf://datasets/lerobot/droid_1.0.1`.
Remote `hf://` reads require the host's `httpfs` extension; released DuckDB
builds can normally auto-install and auto-load it, or it can be loaded
explicitly before `lerobot`.

```sql
INSTALL httpfs;
LOAD httpfs;
LOAD lerobot;

SELECT * FROM lerobot_layout('hf://datasets/lerobot/droid_1.0.1');

SELECT * FROM lerobot_v3_shard_paths(
  'hf://datasets/lerobot/droid_1.0.1',
  'observation.images.wrist',
  2, 17
);

-- Read all v3 episode-metadata shards through DuckDB's native Parquet scan.
SELECT * FROM lerobot_episodes('hf://datasets/lerobot/droid_1.0.1');

-- Read the current v3 task-index to task-text mapping.
SELECT task_index, task
FROM lerobot_tasks('hf://datasets/lerobot/droid_1.0.1');

-- Read the authoritative version and path-template metadata.
SELECT * FROM lerobot_info('hf://datasets/lerobot/droid_1.0.1');

-- Read all frame rows while retaining native Parquet pushdown.
SELECT episode_index, frame_index, timestamp
FROM lerobot_frames('hf://datasets/lerobot/droid_1.0.1');

-- Expand only the episode rows selected by an earlier metadata query.
SELECT *
FROM lerobot_episode_frames(
  'hf://datasets/lerobot/droid_1.0.1',
  [4, 7, 12]
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

-- Decode selected frame rows. The image column is raw HWC RGB24; dimensions
-- and the timestamp actually selected by FFmpeg are returned alongside it.
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
    (1001::BIGINT, 12::BIGINT, 45::BIGINT,
     'observation.images.wrist'::VARCHAR, 0::BIGINT),
    (1001::BIGINT, 12::BIGINT, 45::BIGINT,
     'observation.images.wrist'::VARCHAR, 1::BIGINT),
    (1002::BIGINT, 12::BIGINT, 90::BIGINT,
     'observation.images.front'::VARCHAR, 2::BIGINT)
  ) t(request_id, episode_index, frame_index, video_key, delta_index)
)
SELECT request_id, target_ordinal, video_key, delta_index, is_padding,
       target_frame_index, decoded_timestamp, image
FROM lerobot_video_targets(
  'hf://datasets/lerobot/droid_1.0.1',
  (SELECT * FROM targets),
  delta_timestamps := [-0.2, -0.1, 0.0]
)
ORDER BY target_ordinal;

-- Resolve temporal state/action targets without decoding video. Every input
-- row chooses one delta; exact duplicates remain distinct via target_ordinal.
WITH requests AS (
  SELECT * FROM (VALUES
    (1001::BIGINT, 12::BIGINT, 45::BIGINT, 0::BIGINT),
    (1001::BIGINT, 12::BIGINT, 45::BIGINT, 1::BIGINT),
    (1001::BIGINT, 12::BIGINT, 45::BIGINT, 2::BIGINT)
  ) t(request_id, episode_index, frame_index, delta_index)
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
JOIN lerobot_frames('hf://datasets/lerobot/droid_1.0.1') frames
  ON frames.episode_index = targets.episode_index
 AND frames.frame_index = targets.target_frame_index
JOIN lerobot_tasks('hf://datasets/lerobot/droid_1.0.1') tasks USING (task_index)
ORDER BY targets.request_id, targets.delta_index, targets.target_ordinal;

-- Inspect the data route cache. Set refresh := true after an in-place
-- metadata update that does not publish a new dataset revision.
SELECT *
FROM lerobot_metadata_cache('hf://datasets/lerobot/droid_1.0.1');

-- Materialize and inspect the separate lazy video route cache.
SELECT *
FROM lerobot_video_metadata_cache('hf://datasets/lerobot/droid_1.0.1');
```

| root | info_path | episodes_path | tasks_path | data_path | videos_path |
| --- | --- | --- | --- | --- | --- |
| `hf://datasets/lerobot/droid_1.0.1` | `…/meta/info.json` | `…/meta/episodes` | `…/meta/tasks.parquet` | `…/data` | `…/videos` |

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
  ENCODER_THREADS 4
);
```

Visual input is an unboxed HWC frame in a DuckDB `BLOB`: RGB features require
exactly `height * width * 3` uint8 bytes. Depth features set
`info.is_depth_map` to true and require either little-endian uint16 millimetres
or float32 metres. Image features are embedded as PNG/TIFF structs in Parquet.
Video features use LeRobot's current defaults: AV1/yuv420p for RGB and
lossless HEVC/gray12le after 12-bit logarithmic quantization for depth.
Multiple episodes are stream-copy concatenated into a shard, and the episode
metadata records the resulting `[from_timestamp, to_timestamp)` routes.

The destination must be a new local path. There is intentionally no append,
overwrite, legacy-layout inference, or codec fallback in this strict writer.

## Roadmap

1. Vane-side tensor layout and image-transform materialization.
2. Optional depth-video decoding and dequantization when a target dataset requires it.
3. Optional adaptive seek clustering if cross-storage benchmarks beat the
   fixed 10-second policy materially.
4. Optional hardware-accelerated FFmpeg backends after the CPU contract is
   stable.

## Metadata and Parquet caching

The data and video route caches are separate database-instance `ObjectCache`
entries keyed by normalized dataset root. Entries are immutable and
memory-accounted, so DuckDB can evict them. A size/mtime/version-tag fingerprint
of `meta/info.json` invalidates stale entries automatically; `refresh := true`
forces a rebuild for non-versioned or manually edited datasets. Versioned
Hugging Face revisions should normally need no explicit refresh.

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

The metadata and Parquet functions build without FFmpeg. Native visual writing
and video decoding are enabled automatically when `pkg-config` can find
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

`frame_indices` is the pre-decode sampling control for the low-level frame API;
training reads should normally use `lerobot_video_windows`. `tolerance`
defaults to `1e-4` seconds like native LeRobot, `cluster_gap` defaults to 10
seconds, and `batch_size` defaults to 16 output rows. `max_output_bytes`
independently caps image bytes emitted by one call at 64 MiB (a single larger
frame is still emitted).

`target_buffer_size` defaults to 256 alignment targets per shard buffer.
`max_cached_decoders` defaults to 8 decoder sessions, while
`decode_threads` independently defaults to 8 workers. `max_pending_targets`
defaults to twice `target_buffer_size * decode_threads`; the scheduler flushes
the least-recently-used partial shard buffer before crossing that bound.
`max_open_shards` remains a deprecated compatibility alias for
`max_cached_decoders`. Actual concurrent decoder work cannot exceed either the
worker or decoder-session limit, but changing one no longer silently rewrites
the other.
`codec_threads` defaults to one FFmpeg thread per decoder to avoid nested
oversubscription. Projection pushdown avoids opening video entirely unless the
query needs `image`, `decoded_timestamp`, or unknown native dimensions, so
metadata-only queries also work in an FFmpeg-disabled build. The relation input
uses DuckDB typed vectors and native child-plan projection/filter pushdown; the
source APIs also evaluate pushed output filters themselves. RGB data is written
directly into DuckDB `BLOB` vectors.

With JSON profiling enabled, `lerobot_video_frames` and
`lerobot_video_windows` publish targets, decoder acquires/cache hits/opens/
evictions, decoder seeks, AVIO seeks, video bytes read, frames decoded, RGB
conversions, and same-frame fan-out hits. See the
[A/B benchmark harness](benchmark/README.md) for matching DuckDB, Daft, and
native LeRobot runs and the current adaptive-threshold/hardware-decode decision
gates.

Vane integration is tested against the matching official DuckDB release; Vane
fork-specific integration stays in the Vane repository rather than leaking
into this portable extension.

## C++ compatibility

Extension-owned code uses DuckDB's native `string`, `vector`, `unique_ptr`,
`make_uniq`, and related helpers. It intentionally stays within the C++11
language subset rather than introducing another STL or runtime abstraction.

The build does not override the language standard selected by DuckDB. The
pinned DuckDB 1.5.5 tree selects `-std=c++11`, so this branch compiles the
extension as C++11 without a second toolchain policy. The writer directly uses
the Variegata `CopyFunction` and Parquet rotation APIs; DuckDB `main` and
Vane-fork API adapters belong on their matching version branches.
