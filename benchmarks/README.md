# Benchmarks

Reproducible local CPU workloads with fixed dataset and dependency versions.
Results describe these workloads on the recorded machine; they do not measure
training, DataLoader throughput, network I/O or GPU decoding.

## Workloads

| Config | Engines | Delivered output |
| :--- | :--- | :--- |
| `video-read.toml` | DuckDB, Daft, upstream LeRobot / TorchCodec | Ordered contiguous RGB uint8 HWC NumPy arrays |
| `tensor-batch.toml` | DuckDB / FFmpeg, DuckDB / TorchCodec | Contiguous CPU uint8 NCHW Torch batches, batch size 32 |
| `smoke.toml` | Both tensor paths | Tiny generated fixture, correctness only |
| `smoke-video.toml` | All three video readers | Same tiny fixture, correctness only |

The full configs run sequential reads of 16, 100 and 632 frame rows, a seeded
100-row random read, and a 96-row random read across three video files. Requests
are sampled without replacement, preserve their requested order, and include
every configured camera. Both random cases span episode boundaries.

## Setup

The checked-in locks target **Linux x86_64, Python 3.12, CPU**. Prerequisites:
`uv 0.11.29`, a system FFmpeg runtime compatible with TorchCodec 0.10.0, and a
Release build of `lerobot.duckdb_extension` for **DuckDB 1.5.5**. See the
[build instructions](../README.md#build-from-source). Record the actual binary's
source commit; a checkout SHA alone does not identify a prebuilt extension.

```bash
python3 benchmarks/setup.py

build/benchmarks/envs/duckdb/bin/python benchmarks/prepare.py \
  --config benchmarks/configs/video-read.toml \
  --output build/benchmarks/datasets/egodex
```

Setup creates one environment per engine from `requirements/*.txt`, with CPU
Torch wheels and the local optional adapter installed normally. No environments
are shared through `.pth` files or `PYTHONPATH`. The tensor comparison runs both
paths in the same Torch environment, in separate processes.

Preparation downloads `pepijn223/egodex-test` at
`9ab66a91daf0d0e73f022adadb59f5c9ad7a6b16`, verifies its pinned payload hashes,
and renames only `video.is_depth_map` to `is_depth_map` in `meta/info.json`.
Video and frame Parquet bytes remain unchanged. An existing original snapshot
can be supplied with `--source /path/to/snapshot`; the same hash check applies.

The multi-file variant writes one data shard per episode and copies each
episode's entire original MP4 into its own physical file, preserving timestamp
offsets. It tests file routing with the same content, not a larger or more varied
dataset. Source and prepared payload hashes, frame inventory and routing are
recorded in `manifest.json`. Preparation refuses to overwrite existing data.

## Run

Choose eight available CPUs on an otherwise idle machine. Each run directory
must be new; existing output is never overwritten.

```bash
taskset -c 0-7 python3 benchmarks/run.py \
  --config benchmarks/configs/video-read.toml \
  --data-root build/benchmarks/datasets/egodex \
  --extension build/release/extension/lerobot/lerobot.duckdb_extension \
  --extension-commit "$(git rev-parse HEAD)" \
  --output build/benchmarks/video-read

python3 benchmarks/report.py build/benchmarks/video-read/results.json \
  --output build/benchmarks/video-read/report.md
```

For the tensor comparison, change the config to `tensor-batch.toml` and output
to `build/benchmarks/tensor-batch`. For a distribution artifact, use
`--extension-manifest /path/to/distribution-manifest.json` instead of
`--extension-commit`: the runner verifies its binary hash and records the
compiler, build settings, FFmpeg libraries and exact source revision.

`report.py --update-readme README.md` also regenerates the marked compact video
table. It rejects incomplete runs, missing engines, altered validation files,
different outputs, and mismatched environments. Full reports include medians
and IQR; raw samples and the first-call measurements remain in `results.json`.

## Measurement contract

- One fresh process per engine and case. Workers run sequentially, with a seeded
  shuffle of engine order in each round. Imports and dataset/reader construction
  are recorded as setup, outside measurement.
- One first call is recorded separately, followed by one warmup and five measured
  repeats. OS caches are retained; dataset integrity checks already read files.
  The first call is **not a cold-disk measurement**. Decoder caches persist within
  each case; engines retain their normal public-API cache behavior.
- Timing starts with an already planned list of frame requests. It includes
  engine selection/routing, decoding, transfer to Python, reordering, conversion
  and delivery of the stated contiguous output. Batch consumption and lightweight
  shape/order checks are included. No resize, float normalization or augmentation.
- DuckDB fetches actual RGB BLOBs into Python. Daft creates and collects a fresh
  lazy plan each time. LeRobot 0.6.1 uses its unmodified public `__getitem__` with
  `return_uint8=True` and TorchCodec; the suite does not patch upstream methods.
- Configs request eight engine/Torch threads and set OMP, MKL, OpenBLAS and Rayon
  limits. DuckDB FFmpeg and the optional reader use one thread per decoder.
  Daft and upstream LeRobot retain their public decoder defaults. CPU affinity,
  thread settings, Python/package versions and adapter details are recorded.
- After timing, each engine replays the same operation to compare every ordered
  frame key, shape and SHA-256 pixel digest. Hashing, request planning, IPC and
  JSON/report writing are outside the measured interval. A mismatch fails the run.

## Results and CI

Generated results, per-frame digests, profiles and logs belong under the ignored
`build/benchmarks/<run-id>/` directory. Commit source, configs, locks and small
test fixtures only. Share full result directories as CI artifacts or release
attachments; do not add dated result dumps to the source tree. Historical results
remain accessible through Git history and are not comparable to this output contract.

Native CI runs unit checks and the tiny tensor smoke workload (sequential,
random and multi-file), uploads its evidence, and applies no performance threshold.
Run `smoke-video.toml` locally to check all three readers. Full performance runs
belong on an idle, fixed machine rather than shared PR runners.

## Other experiments

The existing write and timestamp experiments retain their individual CLIs:

| Script under `workloads/` | Purpose |
| :--- | :--- |
| `numeric_write.py` | Numeric COPY, frame/statistics validation |
| `image_write.py` | Image COPY, independent Pillow PNG checks |
| `video_write.py` | Video COPY, whole-process resource accounting |
| `target_timestamps.py` | Actual video-target timestamp routing |
| `timestamp_lookup.py` | SQL timestamp lookup prototype |

Use `python3 benchmarks/workloads/<script>.py --help` for their options and write
`--output` under `build/benchmarks/`. The image script requires NumPy and Pillow;
run it with `build/benchmarks/envs/daft/bin/python`. `fixtures/multishard.py` is the older synthetic
scheduler stress fixture; the cross-engine suite uses `prepare.py` instead.

To intentionally update dependencies, edit the relevant `.in` and regenerate its
lock with `uv pip compile --python-version 3.12 --python-platform x86_64-unknown-linux-gnu
--index https://download.pytorch.org/whl/cpu --index-strategy unsafe-best-match
--no-header --no-annotate benchmarks/requirements/<engine>.in
-o benchmarks/requirements/<engine>.txt`, then recreate environments and rerun
correctness checks before publishing new measurements.
