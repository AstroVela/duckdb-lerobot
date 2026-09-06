# Third-party software and test provenance

This file records direct dependencies and test inputs used by
`duckdb-lerobot`. Transitive dependencies of a particular DuckDB, FFmpeg, or
Python distribution remain subject to that distribution's own notices.

## Distributed source dependency

- **DuckDB v1.5.5** is pinned as the `duckdb/` Git submodule at commit
  `d8cdaa33fda8df955cc76ef58a280f68f4cd43fa`. DuckDB is licensed under the
  MIT License; its license is included at `duckdb/LICENSE` after submodule
  initialization.
  Native CI applies the locally authored zero-byte-read fix documented in
  [test/patches/README.md](test/patches/README.md) to its test host. The patch
  includes a small context excerpt from this MIT-licensed source; the
  distribution pipeline uses the unmodified pinned DuckDB revision.

## Optional native multimedia dependencies

The extension discovers the four FFmpeg libraries with `pkg-config`. Codec
implementations are supplied by that FFmpeg build rather than linked or
discovered separately by this repository; none of their source is vendored.

- **FFmpeg** (`libavformat`, `libavcodec`, `libavutil`, and `libswscale`) is
  primarily LGPL-2.1-or-later, but an FFmpeg build becomes GPL when GPL
  components are enabled. Binary distributors must inspect the exact FFmpeg
  configuration they link and satisfy that build's license.
- **SVT-AV1** (`libsvtav1`) is the current RGB encoder used by the writer when
  FFmpeg exposes it. Older releases use BSD-2-Clause and current releases use
  BSD-3-Clause-Clear; both also include the Alliance for Open Media Patent
  License 1.0. Binary distributors must inspect the selected version.
- **x265** is GPL-2.0-or-later (or available under separate commercial terms)
  and is the current depth encoder. A build that enables or links x265 is not
  an LGPL-only FFmpeg distribution.
- FFmpeg builds may use **libaom** (BSD-2-Clause plus the Alliance for Open
  Media Patent License 1.0), **dav1d** (BSD-2-Clause plus the Alliance for Open
  Media Patent License 1.0), and **zlib** (Zlib) depending on the distributor's
  configuration.

See <https://ffmpeg.org/legal.html> and the package-specific license files for
the configuration being distributed. This inventory is informational and is
not legal advice.

## Conformance-only dependencies

- **Hugging Face LeRobot v0.6.1** is installed only by the conformance job and
  is licensed under Apache-2.0. The corresponding upstream tag resolves to
  `7e241bd630a3719a56157a497ce5d08f244784f1`. It creates the reference-side
  fixture at test time and reads the dataset produced by this extension. It is
  not linked into the extension artifact.
- **PyTorch 2.7.1** and **torchvision 0.22.1** CPU wheels are pinned explicitly
  by the conformance CI setup so installing LeRobot never selects accelerator
  wheels. Both projects are licensed under BSD-3-Clause and are test-only.
- **TorchCodec 0.5** is pinned to the release compatible with PyTorch 2.7 and
  installed from PyTorch's CPU wheel index. It is licensed under BSD-3-Clause
  and is test-only.
- **NumPy 2.2.6** is imported directly by the conformance fixture generator and
  is licensed under BSD-3-Clause. It is test-only.
- **PyArrow** (Apache-2.0) and **pandas** (BSD-3-Clause) are used directly by
  the conformance readers to inspect Parquet values, statistics and task-index
  metadata. They are installed through LeRobot's dataset dependencies;
  local validation used PyArrow 25.0.1 and pandas 2.3.3. They are test-only
  and are not linked into the extension.
- **PyAV 15.1.0** and **Pillow 12.3.0** are pinned for the visual conformance
  environment. PyAV is BSD-3-Clause and Pillow uses the
  [MIT-CMU license](https://pillow.readthedocs.io/en/stable/about.html#license). Their
  wheels may bundle native libraries with separate licenses; they are test-only
  and are not part of the extension artifact.

## Checked-in media fixture

- `test/data/lerobot/long-20701.mp4` is a generated 16x16 H.264 test stream.
  Its exact FFmpeg command and SHA-256 digest are documented in
  `test/data/lerobot/README.md`; it is not copied from a third-party dataset.

## Runtime-generated test inputs

- The numeric, image, mixed-video and shard conformance scripts generate
  deterministic arrays and media at runtime. Pillow, FFmpeg and LeRobot
  supply independent readers or reference routines; no third-party dataset
  content is checked in by these tests.
- `test/conformance/test_session_settings.py` serves generated metadata and
  Parquet over loopback and reuses the generated H.264 fixture above. Its S3
  keys are public dummy values, not credentials for an external service.
- SQL and C++ regression fixtures are generated within their temporary test
  directories. Benchmark result JSON records measurements of synthetic data,
  not third-party media.
