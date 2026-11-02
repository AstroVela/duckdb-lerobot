# DuckDB LeRobot Extension

Query and create [LeRobot](https://github.com/huggingface/lerobot) v3 datasets
with SQL, including frame data, images, and video observation windows.

This branch targets **DuckDB 1.5.5**.

## Install

```sql
INSTALL lerobot FROM community;
LOAD lerobot;
```

Available for Linux, macOS, and Windows on DuckDB 1.5.5.

<details>
<summary>Build from source (Linux x86_64)</summary>

On Linux x86_64, install Git, a C++ compiler, CMake, Ninja, Python 3, and NASM.
From this repository, initialize the dependencies and build:

```bash
git submodule update --init --recursive
git clone https://github.com/microsoft/vcpkg.git build/vcpkg
git -C build/vcpkg checkout 84bab45d415d22042bd0b9081aea57f362da3f35
./build/vcpkg/bootstrap-vcpkg.sh -disableMetrics

make release GEN=ninja \
  VCPKG_TARGET_TRIPLET=x64-linux-release \
  VCPKG_TOOLCHAIN_PATH="$PWD/build/vcpkg/scripts/buildsystems/vcpkg.cmake"
```

The vcpkg revision matches `builtin-baseline` in [`vcpkg.json`](vcpkg.json).
The default build includes LGPL FFmpeg, libaom, dav1d, and zlib.

Start the built DuckDB shell with unsigned extensions enabled:

```bash
./build/release/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/lerobot/lerobot.duckdb_extension';
```

</details>

## Usage

### Query a dataset

Pass the dataset root containing `meta/info.json`:

```sql
SELECT * FROM lerobot_info('my_dataset');
SELECT * FROM lerobot_episodes('my_dataset');
SELECT * FROM lerobot_tasks('my_dataset');

SELECT episode_index, frame_index, timestamp, action
FROM lerobot_scan('my_dataset', episode_indices := [0])
ORDER BY frame_index
LIMIT 10;
```

Hugging Face datasets use `hf://` URIs and DuckDB's `httpfs` extension:

```sql
INSTALL httpfs;
LOAD httpfs;

SELECT *
FROM lerobot_scan('hf://datasets/your-org/your-dataset')
LIMIT 10;
```

### Query a Lance dataset

LeRobot datasets with `"storage_format": "lance"` in `meta/info.json` use
`frames.lance` for frame data. The same metadata functions and `lerobot_scan`
work with local paths and `hf://` roots. Frame columns keep their original
LeRobot names (including dots) and Parquet-compatible types.

```sql
INSTALL lance;
LOAD lance;
SELECT "observation.state", action
FROM lerobot_scan('hf://datasets/lance-format/pusht-lance', episode_indices := [0]);
```

Image features are returned as the same `STRUCT(bytes BLOB, path VARCHAR)`
columns as in the Parquet layout. In nonempty datasets, `language_persistent`
and `language_events` retain their nested layout, including structured tool-call
arguments inferred by the LeRobot annotation writer. String-valued `tool_calls`
are exposed as JSON lists, restoring the JSON type stripped by the converter.

`lerobot_scan` loads Lance on demand and installs it if automatic installation is
enabled. Plain v3 datasets and the metadata-only functions, including
`lerobot_temporal_targets`, do not require Lance. If installation is disabled,
install Lance explicitly before scanning a nonempty Lance dataset. Clients
must enable replacement scans. `union_by_name`, `binary_as_string`, `filename`,
and `file_row_number` are Parquet-only options and are rejected for Lance.

Private datasets require both the DuckDB/httpfs credentials used for `meta/`
and a separate `TYPE LANCE` secret for the frame table, for example:

```sql
CREATE SECRET lance_hf (
  TYPE LANCE,
  SCOPE 'hf://datasets/your-org/',
  HF_TOKEN 'your-token'
);
```

The lance extension caches each opened table for the lifetime of a connection,
so `refresh := true` reloads `meta/` but not `frames.lance`. Open a new
connection after another process rewrites a Lance dataset.

Lance video routes, frames, windows and targets require a compatible
[AstroVela/lance-duckdb](https://github.com/AstroVela/lance-duckdb) build with
`lance_blob_uri` and the Lance Blob filesystem (tested at commit `f791817`).
DuckDB 1.5.5's official `INSTALL lance` artifact supports frame scans but does
not include these video interfaces. Build the extension against the same
DuckDB version and load that artifact before calling the video functions:

```sql
LOAD '/path/to/lance.duckdb_extension';
SELECT episode_index, frame_index, image
FROM lerobot_video_frames('local_lance_dataset', [0]);
```

For a source build on Linux with a C++ compiler, CMake 3.28 or newer, Ninja,
Rust, `protoc` and FFmpeg development libraries installed, build both extensions
together:

```bash
git submodule update --init --recursive
git clone https://github.com/AstroVela/lance-duckdb.git build/lance-source
git -C build/lance-source checkout f791817e7b5dce775118e93cd00c34d20f973b0c
cmake -S duckdb -B build/lance-integration -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DDUCKDB_EXTENSION_CONFIGS="$PWD/test/lance-extension.cmake" \
  -DOVERRIDE_GIT_DESCRIBE=v1.5.5-0-gd8cdaa33fd \
  -DLEROBOT_ENABLE_FFMPEG=ON \
  -DENABLE_SANITIZER=OFF -DENABLE_UBSAN=OFF
cmake --build build/lance-integration --parallel 2 --target \
  shell lerobot_loadable_extension lance_loadable_extension
build/lance-integration/duckdb -unsigned
```

Both extensions use this repository's pinned DuckDB source. Load the artifacts
from `build/lance-integration/extension/{lerobot,lance}/` when using another
DuckDB 1.5.5 shell. The locally built artifacts are unsigned; `-unsigned` applies
to that shell process. FFmpeg builds containing GPL codecs additionally require
`-DLEROBOT_ALLOW_GPL=ON`; these are development builds, not the LGPL release
configuration. See [Lance integration CI](.github/workflows/lance-ci.yml) for the
complete test environment and the pinned test-host patch.

Video routes return snapshot-bound `lance-blob://` paths. The existing native
FFmpeg reader opens these through DuckDB's filesystem, including inline,
packed, dedicated and external Blob v2 values. Historical versions and their
files must remain available while the paths are in use. Each open applies the
current permissions and target-scoped `TYPE LANCE` credentials. Metadata under
`meta/` still uses DuckDB/httpfs credentials. Lance target timestamp lookups
use filtered native queries without the Parquet shard index. Video binding
reloads the full video route table to observe snapshot changes, so repeated
small queries still incur dataset-wide route metadata work.

LeRobot's internal readers use separate connections. In an explicit transaction,
they pin a child transaction and compare its complete secret catalog view with
the caller's before reading. Equal views permit reads; uncommitted secret
creation, replacement or deletion, or a conflicting commit from another
connection, causes a clear error. Commit or roll back those changes before
reading. This compatibility check supports DuckDB's built-in temporary
`KeyValueSecret` storage only. Persistent and third-party secret stores are
outside the explicit-transaction contract; use autocommit for those stores.
It does not make external Lance tables transactional or provide an atomic
snapshot across `frames.lance` and `videos.lance`. Use datasets that remain
unchanged during a query.

The optional Python [`TorchCodecReader`](python/README.md) supports these blob
paths for local dataset roots. It loads complete encoded videos through the
caller's DuckDB connection, with a configurable per-video limit (64 MiB by
default), then decodes tensors with TorchCodec. Blob storage may be remote when
the connection authorizes access. Vane distributed video execution is not part
of this native DuckDB integration. `COPY ... (FORMAT lerobot)` continues to write
the Parquet-based v3 layout.

### Create a dataset

`COPY` creates a new local dataset. Supply episodes in order, starting at zero:

```sql
COPY (
  SELECT 0::BIGINT AS episode_index,
         'pick up a block' AS task,
         i::FLOAT AS action
  FROM range(10) AS frames(i)
  ORDER BY i
) TO 'new_dataset' (
  FORMAT lerobot,
  FPS 10,
  FEATURES '{"action": {"dtype": "float32", "shape": [1]}}'
);

SELECT * FROM lerobot_scan('new_dataset');
```

The writer generates timestamps, frame indices, metadata, and statistics.
The destination must not already exist; append and overwrite are unsupported.

### Decode video frames

For a dataset with an `observation.images.front` video feature:

```sql
SELECT episode_index, frame_index, width, height, channels, image
FROM lerobot_video_frames(
  'video_dataset', [0],
  video_keys := ['observation.images.front'],
  frame_indices := [0, 1, 2],
  width := 320,
  height := 240
);
```

RGB images are returned as raw HWC `BLOB` values.

### Read an observation window

Request frames around a sample with `delta_timestamps` in seconds:

```sql
SELECT request_id, delta_ordinal, is_padding, target_frame_index, image
FROM lerobot_video_windows(
  'video_dataset',
  [struct_pack(request_id := 1, episode_index := 0, frame_index := 30)],
  video_keys := ['observation.images.front'],
  delta_timestamps := [-0.1, 0.0, 0.1]
)
ORDER BY request_ordinal, delta_ordinal;
```

Offsets must align with the dataset FPS. Targets outside the episode are
clamped to its boundary and marked with `is_padding`.

## Benchmark

Local CPU video reads into contiguous RGB uint8 NumPy arrays, including Python
transfer and conversion. The DuckDB columns use the SQL FFmpeg decoder and the
optional [TorchCodec adapter](python/README.md), respectively. DuckDB 1.5.5,
Daft 0.7.25, LeRobot 0.6.1, TorchCodec 0.10.0.
Median seconds over five measured runs; lower is better.

<!-- benchmark-table:start -->

| Case | duckdb-lerobot | duckdb-lerobot / TorchCodec | Daft | LeRobot / TorchCodec |
| :--- | ---: | ---: | ---: | ---: |
| sequential-16 | 0.346 | 0.201 | 0.732 | 0.471 |
| sequential-100 | 1.565 | 1.058 | 3.807 | 2.456 |
| sequential-632 | 11.243 | 6.555 | 20.961 | 14.443 |
| random-100 | 4.586 | 1.556 | 4.171 | 2.787 |
| multi-file-96 | 4.655 | 1.516 | 3.808 | 2.761 |

<!-- benchmark-table:end -->

Dataset: `pepijn223/egodex-test`, 632 frames, one 1080p AV1 camera.
Linux / Xeon E5-2686 v4, eight pinned CPUs; one warmup after a separate first
call, OS cache retained. Frame keys, shapes and pixels are checked across engines.
[Reproduce the benchmarks](benchmarks/README.md) for the pinned dataset, methodology,
random/multi-file cases and full results with timing variation.

## License

Extension source: [Apache License 2.0](LICENSE). Dependency licenses and
FFmpeg codec requirements: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Binary source and rebuild materials: [Releases](https://github.com/AstroVela/duckdb-lerobot/releases).
