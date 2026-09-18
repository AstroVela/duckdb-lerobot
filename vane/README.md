# LeRobot on Vane

This branch adds a separate Vane build of the extension. The upstream DuckDB
submodule, `CMakeLists.txt`, `Makefile`, `extension_config.cmake`, native sources,
and existing workflows are unchanged. Native DuckDB builds use their original
entry points and do not compile any Vane adapter.

`extension_config_vane.cmake` enables `LEROBOT_VANE_DISTRIBUTED`. The adapter
CMake file reuses the native source list and dependencies, substituting a few
translation units with wrappers in `src/vane`. Each wrapper includes the native
implementation once and adds Vane callbacks under the compile guard. This also
keeps private bind data accessible without changing native headers. The video
producer bridge captures Vane's interrupt epoch while the task is running.

## Build

The integration follows the separate manifest / CI-tools approach used by
[AstroVela/duckdb-iceberg](https://github.com/AstroVela/duckdb-iceberg/tree/v1.5-variegata_vane).
`vane-extension.toml` pins the official Vane source and vcpkg to exact commits;
the additional `vane-extension-ci-tools` submodule pins the build tooling.
Vane builds use that checkout's `external/duckdb`, independently of `duckdb/`.

```sh
git submodule update --init --recursive
make -f Makefile.vane vane_validate
make -f Makefile.vane vane_prepare

# Point at vcpkg checked out at the manifest's exact revision.
export VCPKG_TOOLCHAIN_PATH=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
make -f Makefile.vane vane_native VANE_BUILD_JOBS=4
make -f Makefile.vane vane_wheel VANE_BUILD_JOBS=4
```

The Vane native build produces `build/vane-native/extension/lerobot/lerobot.duckdb_extension`.
It links Parquet and JSON, the extensions required by LeRobot, and uses this
repository's FFmpeg dependency manifest. The native test host does not build
`httpfs`. The wheel retains Vane's default extensions, including `httpfs`, whose
CURL dependency is supplied by Vane's own dependency manifest.
The wheel build produces `build/vane-wheel/dist/*.whl` with LeRobot linked into
the Vane engine, including worker engines. These artifacts use Vane's build
identity and must not be loaded into upstream DuckDB. The upstream extension is
still built with the original `make release` command.

The CI workflow `.github/workflows/VaneExtension.yml` runs for pushes and PRs
targeting `v1.5-variegata_vane`. The original native workflows still target
`v1.5-variegata`. Format checks, upstream DuckDB builds with FFmpeg enabled and
disabled, Vane native regressions, and the packaged Vane build run independently.
The installed Vane wheel is then tested with both `local-fast` and a two-worker
Ray cluster.

The Vane regression job uses system FFmpeg with the GPL opt-in for HEVC depth
tests and sets `LEROBOT_FFMPEG_TESTS=1`, enabling all six media SQL suites. It
also builds and runs the codec-executor, video-producer, nested-query and video-I/O
C++ suites against Vane, including the guarded producer and nested-query wrappers.
This test build is separate from the native extension and wheel artifacts built
with the pinned vcpkg dependency profile.

The workflow uses read-only GitHub permissions and pins reusable workflows and actions.
Workers load the extension from the built wheel; runtime tests disable extension
autoinstall and autoload and do not download extensions.
Each runtime job opens one test connection for its selected runner. Expected
rows come from the deterministic fixture, including pixel hashes computed from
its known black RGB frames. The Ray job checks query dispatch and worker node
IDs without opening a local-fast reference connection.

```sh
python -m venv build/runtime
build/runtime/bin/python -m pip install build/vane-wheel/dist/*.whl
VANE_RUNNER=local-fast build/runtime/bin/python -I test/vane/test_runtime.py
VANE_RUNNER=ray build/runtime/bin/python -I test/vane/test_runtime.py
```

## Execution

| API | Vane execution |
| --- | --- |
| `lerobot_scan`, `lerobot_info`, `lerobot_episodes`, `lerobot_tasks`, `lerobot_stats` | Native Parquet / JSON plans produced by the original bind replacement |
| `lerobot_video_routes` | Immutable coordinator snapshot, singleton source |
| `lerobot_cache_info` | Coordinator cache snapshot captured at bind time |
| `lerobot_video_frames`, `lerobot_video_windows` | Explicit episode splits; workers receive selected queries, routes and decode options |
| `lerobot_temporal_targets`, `lerobot_video_targets` | Serializable bind snapshots with native table-in/out execution; a global window provides unique `target_ordinal` values |
| `COPY ... (FORMAT lerobot)` | Worker Parquet staging followed by coordinator validation and dataset publication |

Metadata snapshots carry values and fixed paths, never process-local cache,
decoder, connection or filesystem objects. Video workers clear the original
query list before assignment. Empty assignments stay empty; duplicate splits
and splits from another bound scan are rejected. Windows retain the original
request ordinals across episode splits. Non-contiguous episode IDs are supported
by readers. As with other unordered SQL results, use `ORDER BY` for presentation
and caller-provided `target_id` to correlate target rows across executions.

Distributed COPY requires a **shared local filesystem** visible at the same
absolute paths on the coordinator and every worker. It writes a new dataset;
remote destinations, partitioned COPY, per-thread output and file rotation are
not supported by this adapter. Input must satisfy the original writer's episode
ordering and schema requirements. Use an explicit `ORDER BY` in the COPY query.
With `WRITE_EMPTY_FILE false`, empty input returns zero without creating a
dataset or changing an existing dataset. Non-empty input still requires a new
destination. Remote URIs are rejected before resolving a local destination.

Each worker attempt writes unique staging artifacts. The coordinator consumes
only the results selected by Vane, validates row counts and a global input
ordinal, then calls the original writer to produce data, video, tasks and stats.
The original staging directory is published only after finalization. Failure
aborts the write and cleans its owned staging files; writes to existing datasets
are rejected. Cleanup cannot recover files after the entire coordinator process is
forcibly terminated.

This first implementation distributes input processing and video reads. COPY's
global ordering, final video encoding, statistics and publication run on the
coordinator. Large writes therefore have a coordinator throughput limit and
require temporary space for the intermediate Parquet files; video encoding is
not parallelized across machines. A multi-machine deployment must provide the
shared filesystem; CI's two Ray workers exercise this contract on one host.
