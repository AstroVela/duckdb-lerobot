# Review decisions and implementation status

The 2026-09-04 proposal described `81db162`, not the current public API. This
record separates verified behavior from design choices and future work. It
does not assert that the unavailable companion review's four P0 findings have
all been reproduced or resolved.

## Implemented

| Area | Result |
| --- | --- |
| Public scan surface | `lerobot_scan` owns optional episode selection; hardcoded layout helpers, the decoder alias, and implicit repo-id rewriting were removed in `ca5260c`. |
| Metadata plans | `info/episodes/tasks` use native bind replacement and explicit parameters. `refresh` invalidates both route caches. At `81db162` it was unregistered on these functions and rejected during binding, not an accepted no-op. Info parsing at bind time still exists. |
| Diagnostics | `lerobot_cache_info` inspects data/video cache entries without loading dataset files. Its columns remain an API with defined meanings. |
| Caller identity | `27f3ff2` added optional stable `target_id` pass-through to both target relations. `target_ordinal` is unique within a query and not a stable input position. |
| Statistics | `lerobot_stats` returns `feature VARCHAR, stats JSON`, preserving exact extrema and heterogeneous shapes. Empty datasets have a typed zero-row result. |
| Images | `lerobot_decode_image` returns HWC bytes with dimensions, channels and dtype. PNG/RGBA and uncompressed integer/float depth TIFF are externally tested; physical units and path resolution stay explicit. |
| Encoding | Validated RGB AV1 codec/quality/GOP controls are separate from depth quantizer controls. Default depth clipping is retained; strict range validation is opt-in. Stream-derived codec/pixel format are persisted. Closed HEVC GOPs make episode boundaries seekable by the official reader. |
| Error isolation | A thread setting change that invalidates an already-bound COPY codec budget raises a recoverable input error. Encoder output mismatches use I/O errors. OOM, interruption and missing capabilities retain their specific exceptions. |
| Release evidence | LICENSE, dependency provenance, explicit FFmpeg configuration failure, and numeric conformance existed already. The FFmpeg CI job now also runs pinned external visual conformance. Native image/quantizer tests run under the visual sanitizer suite. |

## Deferred with acceptance criteria

| Project | Required evidence before implementation or release |
| --- | --- |
| Complete windows macro replacement | Indexed request and delta expansion; exact defaults, schemas, duplicate, empty-input, invalid-input and projection behavior. The C++ path is retained. Its limit is **100,000 request/delta pairs before camera expansion**. A 12-row expansion test alone is insufficient. |
| Shared resource governance | Separate producer I/O, decoder sessions, decode workers, per-codec threads and write workers. Admission must be shared by concurrent queries in one DatabaseInstance, cancelable and deadlock-free. Retain per-frame hard bounds independently of any aggregate budget; define which buffers/codec allocations the budget covers. SET is still public API. |
| Timestamp cache | Compare cold construction, repeated reuse, sparse/dense requests, remote I/O and eviction pressure. Preserve missing/duplicate-frame validation, cancellation and concurrent-load behavior. Define shard identity and invalidation; info.json fingerprints alone do not detect independent shard edits. |
| V2.0/V2.1 read adapters | Start with explicit target datasets and separate fixtures for each version's tasks, statistics and path contracts. Normalize to the common route model; do not send old layouts into the v3 parser. Write support stays v3. |
| Append and remote COPY | Define writer exclusion, immutable files versus tail reuse, commit/recovery protocol, reader snapshots and cache invalidation. A final meta-directory rename is not a complete transaction protocol. Timestamp caching is not a prerequisite. |
| Broader codec/platform support | Test each codec/pixel-format/depth combination and official readback. Selecting a runtime encoder does not alter the license of the linked FFmpeg build. Community distribution templates and Linux ARM/macOS/Windows jobs require their own build validation. |
| Structural cleanup | Split along real ownership boundaries without changing behavior; centralize only the DuckDB internal APIs actually in use. Do not couple the split to a new scheduler or resource framework. |

No target count of public functions is a release gate. Small, documented
interfaces can be shipped independently. Correctness, failure cleanup,
projection semantics and pinned visual read/write conformance take precedence
over speculative performance work. Execution knobs retain their current
named-parameter contract; no undocumented `lerobot_test_*` settings are added.

## Timestamp experiment

Run `benchmark/timestamp_lookup.py` with the pinned DuckDB CLI. It compares
native Parquet lookups with an explicit temporary SQL timestamp table and
reports materialization cost, first/repeated lookup latency, peak buffer
memory and an estimated reuse break-even point. Results must agree before
timings are reported.

This prototype is not a compact ObjectCache array, and its first-query metric
does not mean an OS-cold or remote read. The lower bound of eight bytes per
timestamp excludes keys, missing-value state, construction memory and cache
bookkeeping. SQL table measurements must not be presented as a future array
cache's memory use. Reproduce with:

```sh
python3 benchmark/timestamp_lookup.py --duckdb build/release/duckdb \
  --rows 1000000 --targets 4 256 8192 --repeats 5 \
  --output build/timestamp-benchmark.json
```

Materialization runs after the Parquet lookups in the same connection, so its
construction input is warm. DuckDB's `system_peak_buffer_memory` is a cumulative
connection high-water mark, not isolated cache residency or process RSS.

An illustrative local Linux x86_64 run on 2026-09-05, with DuckDB v1.5.5,
1,000,000 rows, one thread and five measured repeats, produced:

| Targets | Parquet warm median | SQL table warm median | Table construction | Estimated reuses to amortize construction |
| --- | --- | --- | --- | --- |
| 4 | 13.58 ms | 2.64 ms | 124.19 ms | 12 |
| 256 | 64.85 ms | 10.66 ms | 112.13 ms | 3 |
| 8,192 | 74.91 ms | 23.48 ms | 104.05 ms | 3 |

The connection buffer high-water mark after materialization was 79.9–82.9 MiB,
including the SQL table and query working memory. That is separate from the
7.63 MiB timestamp-only payload lower bound. This run supports investigating
repeated lookups; it does not establish that a production cache pays off for
one-off, remote or memory-constrained queries.

The original six nested Connection construction sites consist of three
metadata sites, two video sites and one COPY FEATURES parser. Replacing the
video sites would leave four, before considering how a cache loads its data.
Removing SQL text construction and reducing storage work are separate goals.

## Local validation

The follow-up review fixes share the reader's depth-parameter validation with
COPY and validate CRF/GOP before conversion to integers. TIFF sample formats
are checked per channel. For FFmpeg compatibility, the packet's first-page
directory omits only validated unsigned SampleFormat and unassociated RGBA
ExtraSamples entries, keeping the original payload and all field offsets intact.
Strict decode errors remain enabled so a corrupt compressed strip cannot become
a blank image. Regression tests cover these cases and big-endian directories.

The following checks passed on Linux x86_64 with pinned DuckDB v1.5.5:

| Check | Result |
| --- | --- |
| Release, FFmpeg enabled, complete SQL suite | 1,171 assertions across 13 test cases |
| Release, FFmpeg disabled, complete SQL suite | 847 assertions across 8 test cases; 5 visual test files skipped |
| Debug, ASAN + UBSAN, targeted SQL suite | 999 assertions across 12 test cases |
| Pinned LeRobot 0.6.1 numeric and visual conformance | Both directions passed; visual checks include an independent quantizer comparison |
| Changed C++ formatting and Python lint/format checks | Passed |

The targeted sanitizer run excluded `lerobot_copy.test`. Its original
100,000-row low-memory COPY case was stopped after an extended Debug run;
the entire file passed in Release. This is not a claim that the full local
sanitizer suite completed. The CI configuration retains the complete SQL suite
under optimized ASAN/UBSAN and adds external visual conformance to the FFmpeg
release job. Hosted CI and additional operating systems were not run locally.
