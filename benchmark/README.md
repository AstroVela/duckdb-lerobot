# LeRobot decode A/B benchmark

`lerobot_ab.py` runs DuckDB, current Daft, and native LeRobot in separate
Python environments against the same episode/frame/camera contract. Every
result records machine details, exact configuration, warmup/repeat timings,
per-image shape and SHA-256, and (for DuckDB) a JSON profile containing decoder
opens, cache hits/evictions, decoder and AVIO seeks, bytes read, decoded frame
count, and RGB conversion/fan-out counts.

The common cross-engine comparison intentionally uses `delta_timestamps =
[0.0]`: Daft's public dataset reader does not expose LeRobot temporal-window
expansion. SQL tests cover non-zero deltas, padding, duplicates, and order
restoration separately.

Run one engine per environment:

```bash
python benchmark/lerobot_ab.py run \
  --engine duckdb \
  --dataset hf://datasets/pepijn223/egodex-test \
  --camera observation.images.front \
  --rows 100 \
  --extension build/release/extension/lerobot/lerobot.duckdb_extension \
  --output benchmark/results/duckdb.json

python benchmark/lerobot_ab.py run \
  --engine daft \
  --dataset pepijn223/egodex-test \
  --camera observation.images.front \
  --rows 100 \
  --output benchmark/results/daft.json

python benchmark/lerobot_ab.py run \
  --engine lerobot \
  --dataset pepijn223/egodex-test \
  --camera observation.images.front \
  --rows 100 \
  --video-backend pyav \
  --output benchmark/results/lerobot.json

python benchmark/lerobot_ab.py compare \
  benchmark/results/duckdb.json \
  benchmark/results/daft.json \
  benchmark/results/lerobot.json
```

Use the same immutable dataset revision and machine for all runs. A local
snapshot is preferred when measuring decoder CPU; an `hf://` URI is useful when
measuring remote open/seek/read amplification. The setup phase is reported
separately; timed repeats cover materialization and decode after engine setup.

## Daft reference scale

Daft's published benchmark used one Apple M4 Max with 36 GB RAM, not a
multi-machine cluster. Its remote `pepijn223/egodex-test` sweep covered 1–10
rows, plus 100-frame and full 632-frame runs. The batched implementation reduced
10 rows from 34.4 s to 3.9 s, 100 frames from 311.8 s to 25.6 s, and 632 frames
from 1750.7 s to 115.8 s. It also checked the first 16 frames/all cameras on six
public datasets. See Daft's [benchmark description](https://github.com/Eventual-Inc/Daft/tree/main/benchmarking/lerobot)
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
