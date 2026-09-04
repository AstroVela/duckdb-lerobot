# LeRobot v3 bidirectional conformance

`test_bidirectional.py` pins the reference implementation to Hugging Face
LeRobot `v0.6.1` and exercises both directions of the format boundary:

1. LeRobot creates a two-episode numeric dataset, then the DuckDB extension
   reads and validates its info, tasks, episodes, and frames.
2. `COPY ... (FORMAT lerobot)` creates an equivalent dataset, then the official
   `LeRobotDataset` reader loads and validates its metadata and frames.

The reference fixture is generated into a fresh temporary directory on every
run. This keeps the fixture authentic to the pinned implementation without
committing opaque generated Parquet files. The CI job uses Python 3.12 and the
exact package versions shown below and in `requirements.txt`; none is a runtime
dependency of the extension.

Run it after building the extension:

```bash
python3 -m venv build/conformance-venv
build/conformance-venv/bin/python -m pip install --upgrade pip
build/conformance-venv/bin/python -m pip install \
  torch==2.7.1 torchvision==0.22.1 torchcodec==0.5 \
  --index-url https://download.pytorch.org/whl/cpu
build/conformance-venv/bin/python -m pip install \
  -r test/conformance/requirements.txt
build/conformance-venv/bin/python test/conformance/test_bidirectional.py \
  --duckdb build/release/duckdb \
  --extension build/release/extension/lerobot/src/lerobot.duckdb_extension
```
