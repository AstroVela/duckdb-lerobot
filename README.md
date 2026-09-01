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
explicit, deduplicated file list to DuckDB before Parquet binding. It does not
duplicate Parquet footer metadata, row-group statistics, or block caching;
those remain native DuckDB responsibilities.

`lerobot_layout(root)` and `lerobot_v3_shard_paths(...)` expose the canonical
layout without touching storage. A bare Hugging Face repository ID such as
`lerobot/droid_1.0.1` is normalized to `hf://datasets/lerobot/droid_1.0.1`.

```sql
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

-- Inspect the derived route cache. Set refresh := true after an in-place
-- metadata update that does not publish a new dataset revision.
SELECT *
FROM lerobot_metadata_cache('hf://datasets/lerobot/droid_1.0.1');
```

| root | info_path | episodes_path | data_path | videos_path |
| --- | --- | --- | --- | --- |
| `hf://datasets/lerobot/droid_1.0.1` | `…/meta/info.json` | `…/meta/episodes` | `…/data` | `…/videos` |

## Roadmap

1. Episode-to-video-shard timestamp mapping.
2. Shard-aware, batched video decode into Arrow-compatible image batches.
3. Native state/proprioception expressions and episode trimming.

## Metadata and Parquet caching

The route cache is a database-instance `ObjectCache` entry keyed by normalized
dataset root. Entries are immutable and memory-accounted, so DuckDB can evict
them. A size/mtime/version-tag fingerprint of `meta/info.json` invalidates a
stale entry automatically; `refresh := true` forces a rebuild for non-versioned
or manually edited datasets. Versioned Hugging Face revisions should normally
need no explicit refresh.

The route builder projects only `episode_index`, `data/chunk_index`, and
`data/file_index` from episode metadata. Once it has selected the relevant data
shards, the native `parquet_scan` owns schema/footer reads, projection and
filter pushdown, row-group min/max pruning, parallel reads, and external-file
caching. DuckDB's optional `parquet_metadata_cache` setting can therefore be
used without any LeRobot-specific footer cache.

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

Vane integration is tested against the matching official DuckDB release; Vane
fork-specific integration stays in the Vane repository rather than leaking
into this portable extension.

## C++ compatibility

Extension-owned code uses DuckDB's native `string`, `vector`, `unique_ptr`,
`make_uniq`, and related helpers. It intentionally stays within the C++11
language subset so the same source remains compatible with Vane's DuckDB fork.

The build does not force `-std=c++11`; it inherits the standard selected by the
host DuckDB tree. Current official DuckDB and AstroVela's current
`duckdb-iceberg` both require C++17, while older/Vane DuckDB builds can compile
this extension as C++11.
