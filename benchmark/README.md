# LeRobot decode A/B benchmark

`lerobot_ab.py` runs DuckDB, current Daft, and native LeRobot in separate
Python environments against the same episode/frame/camera contract. Every
schema-version-4 result records machine details, exact selected frame and camera
inventory, immutable dataset revision, temporal window, reference backend,
exact configuration, warmup/repeat decode timings, a separate validation time,
per-image shape and SHA-256, and (for DuckDB) a JSON profile containing decoder
opens, cache hits/evictions, decoder and AVIO seeks, bytes read, decoded frame
count, and RGB conversion/fan-out counts.

`compare` rejects results before printing timing ratios when their revision,
requested or available cameras, actual frame selection, temporal window,
reference backend, resize/tolerance contract, cache state, or hardware identity
differs. Schema-version-3 artifacts are intentionally not accepted.

The common cross-engine comparison intentionally uses `delta_timestamps =
[0.0]`: Daft's public dataset reader does not expose LeRobot temporal-window
expansion. SQL tests cover non-zero deltas, padding, duplicates, and order
restoration separately.

## Local snapshot (recommended)

The complete `pepijn223/egodex-test` snapshot is small: revision
`9ab66a91daf0d0e73f022adadb59f5c9ad7a6b16` contains 632 frames in three
episodes and one 1920x1080 AV1 video shard. Its regular files total about 7.4
MiB. Download it once, then force offline mode for every measured process:

```bash
export BENCH_DATA="$PWD/build/benchmark-data/egodex-test"
export BENCH_REVISION=9ab66a91daf0d0e73f022adadb59f5c9ad7a6b16
hf download pepijn223/egodex-test \
  --repo-type dataset \
  --revision "$BENCH_REVISION" \
  --local-dir "$BENCH_DATA"
export HF_HUB_OFFLINE=1
```

`hf download` resumes partial downloads. The benchmark itself does not contact
the Hub when given these local paths. Run one engine per environment:

```bash
python benchmark/lerobot_ab.py run \
  --engine duckdb \
  --dataset "$BENCH_DATA" \
  --revision "$BENCH_REVISION" \
  --camera observation.image \
  --rows 100 \
  --duckdb-cli build/benchmark/duckdb \
  --output build/benchmark-results/duckdb.json

python benchmark/lerobot_ab.py run \
  --engine daft \
  --dataset "$BENCH_DATA" \
  --revision "$BENCH_REVISION" \
  --camera observation.image \
  --rows 100 \
  --output build/benchmark-results/daft.json

python benchmark/lerobot_ab.py run \
  --engine lerobot \
  --dataset pepijn223/egodex-test \
  --lerobot-root "$BENCH_DATA" \
  --revision "$BENCH_REVISION" \
  --camera observation.image \
  --rows 100 \
  --video-backend pyav \
  --output build/benchmark-results/lerobot.json

python benchmark/lerobot_ab.py compare \
  build/benchmark-results/duckdb.json \
  build/benchmark-results/daft.json \
  build/benchmark-results/lerobot.json
```

The official DuckDB shell is useful when the Python environment contains a
different DuckDB ABI. Build a benchmark-only shell with both `httpfs` and this
extension statically linked:

```bash
cmake -S duckdb -B build/benchmark -DCMAKE_BUILD_TYPE=Release \
  '-DDUCKDB_EXTENSION_CONFIGS=/absolute/path/to/duckdb/.github/config/extensions/httpfs.cmake;/absolute/path/to/extension_config.cmake'
cmake --build build/benchmark --target shell -j8
```

Use `--all-episodes --rows 632` for the complete `egodex-test` dataset. Without
`--all-episodes`, `--episode 0` is selected by default. Label fresh-process and
warmed-process measurements with `--cache-state cold-process --warmups 0` and
`--cache-state warm-process --warmups 1`, respectively. The label is metadata;
the caller remains responsible for OS and remote-object cache control.

Use the same immutable dataset revision and machine for all runs. A local
snapshot is preferred when measuring decoder CPU; an `hf://` URI is useful when
measuring remote open/seek/read amplification. The setup phase is reported
separately.

The benchmark has three explicit phases so target selection and correctness
work cannot distort decoder timing:

1. During setup, select only `(episode_index, frame_index)` metadata, sort it,
   and take the requested prefix. DuckDB embeds those rows as a `VALUES` target
   relation. Daft filters by the selected global `index`; its optimizer pushes
   that predicate into the Parquet scan below the video UDF. Native LeRobot
   selects the same positional prefix from its episode subset.
2. Timed repeats decode and materialize the selected image columns without
   hashing. DuckDB consumes every produced BLOB with `octet_length`, Daft
   collects a materialized DataFrame, and LeRobot returns uint8 Torch tensors.
   The native LeRobot adapter strictly projects the timestamp map passed to its
   video reader, so it decodes only the requested `--camera` keys even when the
   dataset contains additional cameras.
   Daft creates a fresh lazy DataFrame for every repeat because `collect()`
   materializes the object in place; reusing it would benchmark a cached no-op.
   Each result's `timing_boundary` records the engine-specific boundary.
3. One additional untimed-for-comparison pass computes per-image SHA-256 over
   the exact RGB bytes. DuckDB hashes its BLOBs in SQL; Daft and LeRobot
   normalize collected images to contiguous uint8 HWC bytes in Python. The
   result's `validation_boundary` states which path was used.
   `validation_seconds` reports the cost, while `compare` treats any key, shape,
   or pixel-hash difference as a correctness failure.

`decode_durations_seconds`, `decode_median_seconds`, and
`decode_min_seconds` are the performance fields. Do not add
`validation_seconds` to the decode median: it deliberately replays the workload
for correctness.

The timed DuckDB query exercises `lerobot_video_targets`. Its profiler pass
replays the same selection through `lerobot_video_frames`, because DuckDB's
source-table profiler hook exposes per-worker decoder metrics there. The result
records this distinction in `profile_scope`; profiler timing is not the timed
relation result. Use `--producer-threads 1` and `--producer-threads 4` on a
multi-data-file fixture to compare serial and parallel source production; the
profile records source query/task/chunk counts, waits, and queue high-watermark.

## Local multi-shard stress fixture

The small public snapshot has only one physical video shard, so it cannot test
decoder scheduling or LRU pressure. Create 20 logical shards and 10,000 targets
without duplicating the MP4 blocks on filesystems that support hard links:

```bash
python benchmark/make_multishard_fixture.py \
  --duckdb-cli build/benchmark/duckdb \
  --source-video "$BENCH_DATA/videos/observation.image/chunk-000/file-000.mp4" \
  --output build/benchmark-data/egodex-20x500 \
  --shards 20 \
  --frames-per-shard 500 \
  --fps 30 \
  --width 1920 \
  --height 1080 \
  --codec av1
```

The generated Parquet metadata and directory entries use roughly 100 KiB of
additional space when hard linking succeeds. Use the source APIs for dense,
known frame scans that should partition work across shards. The
`lerobot_video_targets` table-in/out form preserves arbitrary upstream request
relations, but its effective parallelism is also bounded by the upstream
DuckDB pipeline; a single ordered input partition may therefore feed only one
decoder worker even when `decode_threads` is larger.

## Daft reference scale

Daft's published benchmark used one Apple M4 Max with 36 GB RAM, not a
multi-machine cluster. Its remote `pepijn223/egodex-test` sweep covered 1–10
rows, plus 100-frame and full 632-frame runs. The reported "up to 15x" compares
Daft's old per-row video reader with its new batched video reader; it is not a
Daft-versus-native-LeRobot result. The batched implementation reduced 10 rows
from 34.4 s to 3.9 s, 100 frames from 311.8 s to 25.6 s, and 632 frames from
1750.7 s to 115.8 s. It also checked the first 16 frames/all cameras on six
public datasets. See Daft's [implementation article](https://www.eventual.ai/blog/how-we-made-our-lerobot-video-reader-up-to-15x-faster),
[benchmark description](https://github.com/Eventual-Inc/Daft/tree/main/benchmarking/lerobot),
and [real-dataset results](https://github.com/Eventual-Inc/Daft/blob/main/benchmarking/lerobot/real_datasets.md).

## Decisions after measurement

Keep the fixed 10-second cluster threshold and CPU FFmpeg path as defaults until
the benchmark data justifies added complexity:

- Sweep `--cluster-gap 0`, `1`, `5`, `10`, `20`, and `60` on local and remote
  storage. Add an adaptive threshold only if the best threshold changes by
  workload and beats 10 seconds by at least 10% without increasing bytes read
  or decoded frames pathologically.
- Compare native LeRobot `--video-backend pyav` and `torchcodec` on supported
  hardware. Add a DuckDB hardware backend only after it preserves the strict
  timestamp/pixel contract and improves end-to-end median time by at least
  1.5x on representative codecs and resolutions.
- Treat pixel mismatches as correctness failures before interpreting timings.
  The `compare` command exits non-zero when shapes or SHA-256 values differ.
