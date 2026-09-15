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

Local CPU video reads into contiguous RGB uint8 NumPy arrays, including Python
transfer and conversion. DuckDB 1.5.5, Daft 0.7.25, LeRobot 0.6.1 / TorchCodec 0.10.0.
Median seconds over five measured runs; lower is better.

<!-- benchmark-table:start -->

Results are being regenerated with the common output contract.

<!-- benchmark-table:end -->

Dataset: `pepijn223/egodex-test`, 632 frames, one 1080p AV1 camera.
Linux / Xeon E5-2686 v4, eight pinned CPUs; one warmup after a separate first
call, OS cache retained. Frame keys, shapes and pixels are checked across engines.
[Reproduce the benchmarks](benchmarks/README.md) for the pinned dataset, methodology,
random/multi-file cases and the separate Torch tensor batch comparison.

## License

Extension source: [Apache License 2.0](LICENSE). Dependency licenses and
FFmpeg codec requirements: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
