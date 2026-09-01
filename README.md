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
the canonical LeRobot dataset layout without touching storage. It establishes a
stable URI and SQL contract for the native scans that follow.

```sql
LOAD lerobot;

SELECT * FROM lerobot_layout('hf://datasets/lerobot/droid_1.0.1');
```

| root | info_path | episodes_path | data_path | videos_path |
| --- | --- | --- | --- | --- |
| `hf://datasets/lerobot/droid_1.0.1` | `…/meta/info.json` | `…/meta/episodes` | `…/data` | `…/videos` |

## Roadmap

1. Native `lerobot_episodes` scan over `meta/episodes/*.parquet`, including
   projection and filter pushdown.
2. Episode-to-video-shard timestamp mapping.
3. Shard-aware, batched video decode into Arrow-compatible image batches.
4. Native state/proprioception expressions and episode trimming.

Model inference, such as hand-pose or reward scoring, remains a Vane GPU actor
UDF concern; this extension owns the data-plane hot path.

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
DuckDB and Vane extension for LeRobot datasets
