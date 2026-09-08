# LeRobot decode A/B benchmark

`lerobot_ab.py` runs DuckDB, current Daft, and native LeRobot in separate
Python environments against the same episode/frame/camera contract. Every
schema-version-1 result records machine details, the requested, resolved, and
effective dataset sources plus their local/remote mode, exact selected frame
and camera inventory, immutable dataset revision, temporal window, reference
backend, exact configuration,
warmup/repeat decode timings, a separate validation time, per-image shape and
SHA-256, and (for DuckDB) a JSON profile containing decoder opens, cache
hits/evictions, decoder and AVIO seeks, bytes read, decoded frame count, and RGB
conversion/fan-out counts.

`compare` rejects results before printing timing ratios when their revision,
requested or available cameras, actual frame selection, temporal window,
reference backend, resize/tolerance contract, cache state, or hardware identity
differs. Source mode and normalized effective source must match, so local and
remote I/O measurements cannot be mixed. Warmup and repeat counts must also
match so cache preparation and median sample sizes stay comparable.
Artifacts with another schema version are intentionally not accepted.

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
the Hub when given a local path, and it does not scan or hash the snapshot
before timing. The caller is responsible for preparing the same immutable
snapshot for every local run. Dataset paths with leading or trailing whitespace
are rejected because DuckDB and Daft normalize them before opening files. Run
one engine per environment:

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

For DuckDB and Daft, a remote Hugging Face repo ID or unpinned `hf://datasets/`
root is canonicalized to `hf://datasets/<repo>@<revision>` using the required
commit SHA. An already pinned URI must name the same commit. Every other URI
scheme is rejected; download those sources with
`hf download --revision ... --local-dir ...` first. Native LeRobot receives the
same SHA through its `revision` argument. Local paths are resolved to their
absolute canonical path. Native LeRobot records `dataset.root`, including its
resolved Hub cache path when `--lerobot-root` is omitted, because its default v3
reader decodes from that local snapshot. Native LeRobot therefore cannot be
compared with a DuckDB/Daft run that decodes directly from a remote Hugging Face
URI.

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
snapshot is preferred when measuring decoder CPU; the automatically
revision-pinned `hf://` URI is useful when measuring remote open/seek/read
amplification. The setup phase is reported separately.

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

## Native multi-camera write benchmark

`lerobot_copy_write.py` is a local, generated-data benchmark for issue #8; it
does not use Hugging Face or the network. It runs 2- and 4-camera COPY workloads
with a fixed DuckDB/codec thread budget, first with one camera worker and then
with bounded camera parallelism. Every measured output is checked for route
timestamps, one final shard per camera, frame shape/count, and decoded pixel
hash equivalence before its timing is accepted.

```bash
python benchmark/lerobot_copy_write.py \
  --duckdb-cli build/release/duckdb \
  --extension build/release/extension/lerobot/lerobot.duckdb_extension \
  --work-dir build/write-benchmark-data \
  --output build/write-benchmark.json \
  --threads 4 \
  --episodes 8 \
  --frames-per-episode 30 \
  --repeats 3
```

Result schema v2 records wall time, per-child peak RSS, kernel filesystem block
counters, output size, and the serial/parallel median ratio. On systems with
`wait4`, accounting covers the entire CLI process, including shard assembly,
metadata publication and shutdown; validation runs separately. Linux and macOS
RSS values are normalized to bytes. These block counters replace the old sampled
`rchar`/`wchar` fields, whose final writes could be missed. They measure filesystem
I/O and depend on the page cache; they do not measure logical bytes transferred
by read/write calls. On Linux, each block unit represents 512 bytes.

The 4-camera case doubles the episode count and reports block-counter ratios.
Unavailable metrics and ratios with zero denominators are `null`, never assumed
zero. Treat these ratios as diagnostics: cached reads can produce zero blocks,
and the ratios alone cannot establish an algorithm's I/O complexity. Systems
without `wait4` retain wall time with unknown resource metrics. Use `--keep-output`
to retain all validated datasets.

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

## Timestamp lookup experiment

`timestamp_lookup.py` measures the tradeoff between repeated native Parquet
timestamp lookups and preloading an explicit SQL table. It uses the supplied
DuckDB CLI and checks result equivalence before reporting timings. See
[scope and interpretation](../docs/review-followups.md#timestamp-experiment).
It does not add or benchmark an extension-owned timestamp cache.

`video_target_timestamps.py` measures the real `lerobot_video_targets` operator
on generated local shards, including multiple input chunks, sparse/dense
requests and repeated targets. Its metadata projection consumes every timestamp
and works with deliberately absent MP4 files. A native join checks count and
timestamp-sum equivalence. For example, run the same command with the before
and after extensions and separate output paths:

```bash
python3 benchmark/video_target_timestamps.py \
  --duckdb build/release/duckdb \
  --extension build/release/extension/lerobot/lerobot.duckdb_extension \
  --rows 1000000 --shards 1 4 --targets 4 4096 32768 --repeats 5 \
  --output build/video-target-timestamps.json
```

Use `--batch-size`, `--threads` and `--memory-limit` to vary batching and memory
pressure. `--video clip.mp4` also measures decoding and records an untimed pixel
hash aggregate for comparisons with the same pinned DuckDB build. The supplied
clip must be 32 fps and contain at least `rows / shards` frames for every shard
count tested. This is separate from external codec conformance.

Each scenario uses a fresh database; repeats share route/footer caches but each
target query constructs and releases its own timestamp indexes. OS caches are
not flushed, and the join runs before the target queries. Buffer-memory values
are cumulative connection high-water marks, including native query memory, not
isolated index residency or RSS. DuckDB 1.5.5 does not export dynamic metrics for
table-in/out operators; the report leaves these metrics unavailable. Controlled
C++ tests check query counts, data reads and the index memory cap directly.
Neither local benchmark measures HTTP/Hugging Face latency or bandwidth.

## Numeric COPY

`lerobot_copy_numeric.py` measures COPY of synthetic state/action arrays with
float32, float64 and int64 elements. The defaults cover 14, 64, 65 and 256
dimensions, including the statistics batch boundary. Every run checks every
written frame with a native Parquet scan, then hashes all episode and dataset
statistics; compare these hashes before interpreting timings across binaries.

```bash
python3 benchmark/lerobot_copy_numeric.py \
  --duckdb build/release/duckdb \
  --extension build/release/extension/lerobot/lerobot.duckdb_extension \
  --frames 10000 --episodes 2 --repeats 3 --threads 1 \
  --widths 14 64 65 256 --dtypes float32 float64 int64 \
  --memory-limit 256MB --output build/numeric-copy.json
```

Each repeat starts a fresh process. Input-table generation precedes profiling;
`copy_seconds` measures the COPY statement, while `process_seconds` also
includes setup and validation. The connection buffer high-water mark is
cumulative. Linux runs with `/usr/bin/time` additionally record the process
RSS high-water mark, including validation and shutdown; other environments
leave RSS unavailable. Neither metric is a hard RSS bound imposed by
`memory_limit`. Temporary spill files use a separate directory per run.
OS caches are not flushed. This is a numeric workload with no codec work.

The local Linux x86_64 comparison in
[`results/numeric-copy-20260906.json`](results/numeric-copy-20260906.json)
records both extension hashes and per-repeat timings/memory. It also includes
a 96 MB memory configuration that spills to disk. The same 256-dimensional
fixture fails at 64 MB in both implementations; this benchmark does not
establish support below DuckDB's allocation requirements for the workload.

## Image COPY

`lerobot_copy_image.py` measures RGB `image` COPY, using the independent NumPy
fixtures and Pillow validator from `test/conformance/test_image_copy.py`.
It needs NumPy, PyArrow and Pillow; the conformance environment provides them.
Defaults cover 16×16, 160×120 and 640×480 images, a noisy VGA case, and two
cameras with different resolutions. There are two episodes per dataset.

```bash
build/conformance-venv/bin/python benchmark/lerobot_copy_image.py \
  --duckdb build/release/duckdb \
  --extension build/release/extension/lerobot/lerobot.duckdb_extension \
  --baseline-extension /path/to/before/lerobot.duckdb_extension \
  --repeats 4 --output build/image-copy.json
```

Omit `--baseline-extension` to measure one binary. When supplied, the script
alternates before/after run order on successive repeats and requires equal PNG
and statistics hashes across both binaries. Each image is also decoded with a
fresh Pillow instance and compared pixel by pixel to the input. Compressed-byte
equality is a check for the same FFmpeg build, not a portable bitstream promise.

DuckDB profiling measures COPY after input materialization; Python fixture
generation and validation are outside that timer. Each run uses a fresh process
and temporary directory. Linux GNU time measures DuckDB's peak RSS including
input load and shutdown, excluding the Python process. Native buffer/spill
metrics are cumulative connection peaks; `memory_limit` does not bound RSS or
FFmpeg allocations. OS caches are not flushed.

[`results/image-copy-20260906.json`](results/image-copy-20260906.json) records
the local comparison, binary/source hashes and all repeats, including a second
measurement with alternating run order for the noisy VGA workload. The small
image case benefits from encoder reuse; compressing high-entropy large images
remains expensive. These are whole-COPY measurements, not isolated PNG encoder
timings or results for other platforms.
