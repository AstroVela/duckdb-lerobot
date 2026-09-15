# DuckDB LeRobot Extension

Query and create [LeRobot](https://github.com/huggingface/lerobot) v3 datasets
with SQL, including frame data, images, and video observation windows.

This branch targets **DuckDB 1.5.5**.

## Install

### Build from source

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

### Load

Start the built DuckDB shell with unsigned extensions enabled:

```bash
./build/release/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/lerobot/lerobot.duckdb_extension';
```

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

Measured on 2026-09-15: Linux, Xeon E5-2686 v4, CPU affinity 0–7, local
`pepijn223/egodex-test` snapshot (632 frames, one 1080p AV1 camera).
Only a legacy metadata field name was normalized; video and Parquet files were unchanged.
Times are seconds, median of five runs after one warmup; OS caches were not
flushed. All selected frame keys, shapes, and RGB pixel hashes matched.

**Video reads into each engine's native output:**

| Frames | duckdb-lerobot / DuckDB 1.5.5 | Daft 0.7.25 | LeRobot 0.6.1 / TorchCodec | LeRobot main / TorchCodec |
| ---: | ---: | ---: | ---: | ---: |
| 16 | 0.122 | 0.582 | 0.189 | 0.177 |
| 100 | 0.826 | 2.710 | 1.360 | 1.349 |
| 632 | 4.819 | 14.841 | 7.638 | 7.214 |

DuckDB consumes RGB BLOBs in SQL; Daft collects image columns; LeRobot returns
uint8 Torch tensors. DuckDB's Python transfer and tensor conversion are excluded.
LeRobot main is pinned to `89236ea`; both LeRobot variants use TorchCodec 0.10.0.

**SQL selection through contiguous uint8 NCHW Torch batches** (batch size 32):

| Frames | SQL FFmpeg → Torch | SQL → TorchCodec |
| ---: | ---: | ---: |
| 16 | 0.346 | 0.192 |
| 100 | 2.130 | 1.208 |
| 632 | 11.499 | 7.668 |

This second comparison includes Python transfer/conversion and batch assembly,
using PyTorch 2.10.0 CPU. Both comparisons exclude imports and pixel-hash checks;
they measure this local video workload, not model training or DataLoader throughput.
See the [recorded results](benchmark/results/video-read-20260915.json),
[cross-engine reproduction steps](benchmark/README.md#local-snapshot-recommended),
and [TorchCodec benchmark command](python/README.md#verify).

## License

Extension source: [Apache License 2.0](LICENSE). Dependency licenses and
FFmpeg codec requirements: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
