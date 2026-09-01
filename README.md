# DuckDB LeRobot extension

`duckdb-lerobot` is an Apache-2.0 DuckDB/Vane extension for querying and
preparing [LeRobot](https://github.com/huggingface/lerobot) robot-learning
datasets from SQL. It is developed by AstroVela.

The project keeps the official `duckdb/duckdb` dependency as a submodule,
following the repository structure used by AstroVela's `duckdb-iceberg` fork.
Vane consumes release-compatible extension builds through an explicit DuckDB
version compatibility matrix.

## Current status

The extension now exposes native dataset-root scans for LeRobot v3 metadata
and frame Parquet files. Like the Iceberg extension, it copies DuckDB's
registered `read_json_auto` and `parquet_scan` function sets and injects only a
format-specific `MultiFileReader`. DuckDB therefore retains its native
projection, filter, parallel I/O, and row-group pruning paths.

Episode-scoped scans additionally cache the small LeRobot control plane:
`meta/info.json` plus the `episode_index` to data-shard mapping from
`meta/episodes`. The cache resolves custom `data_path` templates and hands an
explicit, deduplicated file list to DuckDB before Parquet binding.

Video routing is available as a metadata-only second stage. It resolves each
requested episode and video feature to its full MP4 path and timestamp range,
without opening or decoding the MP4. Its episode-camera routes live in a
separate lazy cache, so a frame-only query does not pay their memory or metadata
projection cost. Neither cache duplicates Parquet footers, row-group
statistics, or data blocks; those remain native DuckDB responsibilities.

Native video decode is available as the third stage. `lerobot_video_frames`
reads only the selected episodes' frame timestamps, adds each episode's video
offset, groups work by MP4 shard, and decodes independent shards in parallel.
Targets within a shard are timestamp-sorted and split at gaps larger than 10
seconds. Each shard is opened once; each cluster seeks backward to the
preceding keyframe and decodes forward to the closest frame within half a frame
period. Results are small Arrow-compatible batches of interleaved RGB24 bytes,
not Python image objects.

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

-- Inspect the data route cache. Set refresh := true after an in-place
-- metadata update that does not publish a new dataset revision.
SELECT *
FROM lerobot_metadata_cache('hf://datasets/lerobot/droid_1.0.1');

-- Materialize and inspect the separate lazy video route cache.
SELECT *
FROM lerobot_video_metadata_cache('hf://datasets/lerobot/droid_1.0.1');
```

| root | info_path | episodes_path | data_path | videos_path |
| --- | --- | --- | --- | --- |
| `hf://datasets/lerobot/droid_1.0.1` | `…/meta/info.json` | `…/meta/episodes` | `…/data` | `…/videos` |

## Roadmap

1. Native state/proprioception expressions and episode trimming.
2. Remote-revision benchmarks for decode cold/warm paths and byte ranges.
3. Optional hardware-accelerated FFmpeg backends after the CPU contract is
   stable.

## Metadata and Parquet caching

The data and video route caches are separate database-instance `ObjectCache`
entries keyed by normalized dataset root. Entries are immutable and
memory-accounted, so DuckDB can evict them. A size/mtime/version-tag fingerprint
of `meta/info.json` invalidates stale entries automatically; `refresh := true`
forces a rebuild for non-versioned or manually edited datasets. Versioned
Hugging Face revisions should normally need no explicit refresh.

The data route builder projects only `episode_index`, `data/chunk_index`, and
`data/file_index` from episode metadata. The lazy video route builder projects
only `episode_index` and each video feature's chunk, file, and timestamp-range
columns. Once the extension has selected the relevant paths, native
`parquet_scan` owns schema/footer reads, projection and filter pushdown,
row-group min/max pruning, parallel reads, and external-file caching. DuckDB's
optional `parquet_metadata_cache` setting can therefore be used without any
LeRobot-specific footer cache.

Decoded images are deliberately not cached by this extension: they are large,
often consumed once by training, and cache policy belongs with the Vane actor or
model pipeline. Video reads go through DuckDB's `FileSystem`, so local files,
`hf://`, credentials, and any caching provided by the host filesystem use the
same path as native scans. Within one decode query the FFmpeg container and
conversion context are reused for every target on the same shard.

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

The metadata and Parquet functions build without FFmpeg. Native video decode is
enabled automatically when `pkg-config` can find `libavformat`, `libavcodec`,
`libavutil`, and `libswscale`; on Debian/Ubuntu the corresponding development
packages can be installed with:

```bash
sudo apt-get install pkg-config libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
```

Use `-DLEROBOT_ENABLE_FFMPEG=OFF` for a metadata-only build. Run the deterministic
H.264 decode test explicitly with:

```bash
LEROBOT_FFMPEG_TESTS=1 make test
```

`frame_indices` is the pre-decode sampling control and should be used for sparse
training reads. `tolerance` defaults to half a frame period, `cluster_gap`
defaults to 10 seconds, and `batch_size` defaults to 16 output rows so raw RGB
frames do not inflate a standard DuckDB vector to excessive memory.

Vane integration is tested against the matching official DuckDB release; Vane
fork-specific integration stays in the Vane repository rather than leaking
into this portable extension.

## C++ compatibility

Extension-owned code uses DuckDB's native `string`, `vector`, `unique_ptr`,
`make_uniq`, and related helpers. It intentionally stays within the C++11
language subset so the same source remains compatible with Vane's DuckDB fork.

The build does not force `-std=c++11`; it inherits the standard selected by the
host DuckDB tree. Current official DuckDB builds as C++17 and the current Vane
tree builds as C++20. Extension-owned translation units are additionally
syntax-checked with `-std=c++11` against the Vane headers, keeping this layer
portable even though either host can require a newer standard internally.
