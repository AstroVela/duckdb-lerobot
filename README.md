# DuckDB LeRobot extension

`duckdb-lerobot` is an Apache-2.0 DuckDB/Vane extension for querying and
preparing [LeRobot](https://github.com/huggingface/lerobot) robot-learning
datasets from SQL. It is developed by AstroVela.

The project keeps the official `duckdb/duckdb` dependency as a submodule,
following the repository structure used by AstroVela's `duckdb-iceberg` fork.
Vane consumes release-compatible extension builds through an explicit DuckDB
version compatibility matrix.

## Current status

The first vertical slice is deliberately small: `lerobot_layout(root)` exposes
the canonical LeRobot dataset layout and `lerobot_v3_shard_paths(...)` expands
v3's metadata path templates without touching storage. Together they establish
a stable URI and SQL contract for the native scans that follow.

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
```

| root | info_path | episodes_path | data_path | videos_path |
| --- | --- | --- | --- | --- |
| `hf://datasets/lerobot/droid_1.0.1` | `…/meta/info.json` | `…/meta/episodes` | `…/data` | `…/videos` |

## Roadmap

1. Replace the initial `lerobot_episodes` Parquet macro with a dedicated scan,
   including dataset-layout discovery plus projection and filter pushdown.
2. Episode-to-video-shard timestamp mapping.
3. Shard-aware, batched video decode into Arrow-compatible image batches.
4. Native state/proprioception expressions and episode trimming.

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
