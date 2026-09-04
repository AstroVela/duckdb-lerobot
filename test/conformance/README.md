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
  --extension build/release/extension/lerobot/lerobot.duckdb_extension
```

An FFmpeg-enabled build additionally runs `test_visual.py` with the same CLI
arguments and pinned environment. It generates independent Pillow PNG/RGBA,
RGB TIFF with per-channel SampleFormat, compressed/uncompressed RGBA TIFF, and
uint16/float32 TIFF fixtures, creates RGB and depth videos with official
LeRobot, and compares both readers on official and extension-created datasets.
Both directions cover two concatenated episodes, exact frame selection, padded
windows, metres/millimetres, default and custom logarithmic/linear quantizers,
and extension RGB encoding through SVT-AV1 and libaom.

Write-side depth values are also checked against the reference quantizer,
followed by the same Torch float32 dequantization path used by LeRobotDataset.
This catches encoding mistakes that two readers of the same file would miss.
The separate NumPy dequantization branch can differ at rounding boundaries;
it is not used as a substitute for the dataset reader's contract.

Comparison is on decoded values, never compressed-file hashes. RGB conversion
allows at most 2 uint8 levels for differences between the FFmpeg builds; depth
millimetres must match exactly and float metres use an absolute tolerance of
1e-6. Timestamp tolerance is not relaxed. The reference depth encoder uses
closed GOPs: LeRobot 0.6.1's default open-GOP short clips can fail its own PyAV
random seeks at a concatenated episode boundary. The extension also emits
closed GOPs, and this test exercises those boundary frames explicitly.

CI runs this external comparison in the FFmpeg release job. ASAN/UBSAN jobs
run the native visual SQL suite, including malformed-image and quantizer tests;
they do not instrument the external Python codec stack. These jobs establish
compatibility for the pinned versions and tested formats, not universal
bit-for-bit equivalence across codecs, versions, or platforms.
