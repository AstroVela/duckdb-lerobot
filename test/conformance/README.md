# LeRobot v3 bidirectional conformance

`test_bidirectional.py` pins the reference implementation to Hugging Face
LeRobot `v0.6.1` and exercises both directions of the format boundary:

1. LeRobot creates a two-episode numeric dataset, then the DuckDB extension
   reads and validates its info, tasks, episodes, and frames.
2. `COPY ... (FORMAT lerobot)` creates an equivalent dataset, then the official
   `LeRobotDataset` reader loads and validates its metadata and frames.
3. Pandas restores the task index without fabricated creator/version metadata.
   Independently written timestamp columns have identical float32 bits at FPS
   3, 30, 16777217 and 16777219, including operands beyond exact float32 integers.

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
build/conformance-venv/bin/python test/conformance/test_numeric_stats.py \
  --duckdb build/release/duckdb \
  --extension build/release/extension/lerobot/lerobot.duckdb_extension
```

`test_numeric_stats.py` calls LeRobot 0.6.1's `compute_episode_stats` with
NumPy 2.2.6. It checks all ten statistics in episode Parquet and dataset JSON
using exact array comparisons. The 27 single-episode cases cover scalar and
14/64/65/256-dimensional float32/float64 features, 1/7/8/129/2050 frames, and
all eleven supported numeric/bool dtypes. Dyadic values keep sums exactly
representable before division; agreement is specific to these fixtures.
Existing SQL regressions separately retain difficult float32 rounding and
integer extrema beyond DOUBLE's exact range. Native CI runs this oracle
alongside the bidirectional test in the FFmpeg release job.

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

`test_mixed_video.py` adds a lightweight independent FFmpeg comparison using
only the Python standard library, `ffmpeg`, and `ffprobe`. Three COPY statements
in one process each encode two RGB cameras of different sizes alongside depth,
with three episodes per COPY. Camera dimensions change between COPY statements
to exercise repeated codec lifecycles. Four additional depth clips cover widths
16, 32, 64, and 66, including the narrow-image GOP workaround and its boundary.
All seven COPY statements share the same process and codec worker pool.

The test checks dimensions, codec/pixel format, frame count, every timestamp,
episode routing, and all decoded pixels. RGB patterns identify camera, spatial
position, and frame; lossy source comparisons allow a maximum error of 16 and
mean error of 3 uint8 levels, while the two FFmpeg readers must agree within 2.
Depth quantization codes and decoded integer millimetres must match exactly.
Narrow depth clips must contain only keyframes and report their actual GOP in
`info.json`. Patterns cover image edges; constant-colour frames cannot detect
the x265 reference-padding corruption exposed by this test.

```bash
python3 test/conformance/test_mixed_video.py \
  --duckdb build/release/duckdb \
  --extension build/release/extension/lerobot/lerobot.duckdb_extension \
  --codec libaom-av1 \
  --workdir build/mixed-video-conformance
```

Use `--codec libsvtav1` to test SVT instead. The work directory must be new; it
retains source SQL, media, version information, encoder logs, and validation
metrics for inspection. Both FFmpeg native CI jobs run this comparison: libaom
in Release and SVT with the DuckDB/extension ASAN+UBSAN build. The external
FFmpeg processes are not instrumented, and this encoding check disables LSan;
the separate decoder lifetime job retains leak detection.

Both jobs also run `--fps 30 --episode-lengths 1 2 3 6` in a separate work
directory. This checks short/long fragment boundaries and timestamps at a
frame rate that cannot be represented exactly in integer microseconds. Depth
metadata must declare `bf=0`; otherwise mixing short clips with x265's default
reorder delay can make decode timestamps run backwards.

With FFmpeg enabled, `cmake --build build/release --target test_lerobot_video_io`
runs filesystem fault injection against the production COPY implementation.
It covers encoder output, MP4 faststart, fragment reads, metadata/Parquet
finalization, and the final rename. Injected failures must leave the target
unpublished, release handles, and allow a retry in the same database. Staging
is removed when the filesystem permits it. Combined close/removal faults must
still attempt all cleanup, preserve the primary error, and report the exact
residual path when logging is enabled. The tests also make logging throw via
`warnings_as_errors`, verifying that diagnostics cannot stop cleanup.

`test_lerobot_copy_cleanup` runs numeric-only cases for partial construction,
staging lookup/removal errors, cancellation before publication, directory
ownership and retry. It is available
with or without FFmpeg. Native CI runs the full video suite under ASAN/UBSAN
and separately enables LSan for `[video_io]` and `[copy_cleanup]`, which do not
initialize external codecs.
The video I/O LSan step also runs `[video_copy_cancel]` using explicit libaom:
real COPY is interrupted at final output creation, fragment reads, faststart
reopening and manifest writing. Every case checks `InterruptException`, zero
open handles, absent output/staging, and successful retry in the same database.
The numeric cleanup LSan step includes `[numeric_stats]` and `[copy_bind]`; numeric
coverage includes a real 4097-row, 129-dimensional COPY and exact int64
extrema, with no external codec initialization.

`test_session_settings.py` uses the real HTTPFS extension against two loopback
S3 fixtures. Native JSON/Parquet control queries, metadata routing, the video
producer and target timestamp lookup must honor `SET SESSION` credentials and
endpoints. Warm data and video caches must reject access after credentials are
reset or directory/file-scoped secrets are dropped, including when only the
episode Parquet object is private and the manifest, listing and payloads are
public. Switching endpoints through SESSION/GLOBAL settings or root/directory
secret replacement must select new routes even with identical `info.json`
fingerprints; switching back must restore the original routing. A file-specific
endpoint that conflicts with its directory listing must report the same ETag
error as native HTTPFS, including after warming the route caches.
Server-only policy changes, with credentials and bytes unchanged, must also
reject warm routes: the checks separately revoke object access, GET access
while allowing HEAD, and directory listing. Data/video checks run with external
file caching both enabled and disabled; `lerobot_cache_info` remains passive
after revocation. GET-only revocation of `info.json` also covers ordinary and
zero-episode datasets, with native JSON reads as controls. Metadata replacement,
a changed file list, and moving rows between empty/nonempty Parquet files must
update both routes even when
`info.json` is unchanged. Native reads provide controls for these cases.
Another check commits an empty manifest and removes all episode files after
warming both caches, then restores the full dataset. Both transitions must
rebuild routes with external file caching enabled and disabled.
Only public dummy keys are used; the fixtures do not record authorization
headers. Install HTTPFS for the pinned DuckDB, then run:

```bash
python3 test/conformance/test_session_settings.py \
  --duckdb build/release/duckdb \
  --extension build/release/extension/lerobot/lerobot.duckdb_extension
```

Use `--httpfs-extension` for an already available extension file, and
`--skip-video` for a build without FFmpeg. Native CI runs the full loopback check
in the FFmpeg release job. The C++ nested-reader suite additionally covers
connection isolation, NULL/literal setting values, reset semantics and native
Parquet producer reads at one/four threads without HTTPFS or FFmpeg; this also
runs under ASAN/UBSAN/LSan. A controlled S3 filesystem switches a second
connection's global endpoint during metadata I/O to verify both route caches,
cold/warm loads, explicit refresh, bounded retries and recovery. It also
interrupts remote manifest and episode validation reads, checks handle cleanup,
and confirms the same connection can reuse the cache after clearing the interruption.

`test_video_shards.py` supplies identical encoded fragments to DuckDB and the
official LeRobot 0.6.1 writer, substituting only the official encoder's output.
Official metadata, rollover decisions, file stats and concatenation still run.
It demonstrates the documented difference between summed fragment sizes and
the size of an already merged prefix, then verifies both readers on every
boundary frame. Nine datasets / 42 frames cover the difference, +/- one byte
around the first-pair threshold, a single episode larger than the target size,
and chunk rollover. The FFmpeg release CI job runs this pinned comparison.

`test_lerobot_image_writer` exercises the production PNG writer and image COPY
under a separate LSan step with `detect_leaks=1`. It covers retained outputs
across frame/packet reuse, independent features, invalid frames, exception
unwinding, filesystem failure and real connection interruption after the first
episode has encoded. Repeated cancellation and retry use the same database.
The FFmpeg-disabled job checks the missing-support error.

The FFmpeg release job also runs an independent Pillow oracle:

```bash
build/conformance-venv/bin/python test/conformance/test_image_copy.py \
  --duckdb build/release/duckdb \
  --extension build/release/extension/lerobot/lerobot.duckdb_extension
```

It compares every pixel of 16,744 PNGs with deterministic NumPy inputs, using
a new Pillow decoder for each image. Cases include one-pixel images, unaligned
rows, two cameras with different shapes, noise, more than two input chunks,
episode boundaries, and three successive COPY statements with changing shapes
in one connection, at `threads=1` and `threads=4`. Each image must be a complete
single-frame RGB PNG. The companion image benchmark shares this reference and
records compressed-byte and statistics hashes for before/after comparisons;
these hashes are observations for the tested build, not a cross-version PNG
bitstream contract.

CI runs the pinned LeRobot comparison in the FFmpeg release job. ASAN/UBSAN jobs
also run the native visual SQL suite, including malformed-image and quantizer
tests; they do not instrument the external Python codec stack. These jobs establish
compatibility for the pinned versions and tested formats, not universal
bit-for-bit equivalence across codecs, versions, or platforms.
