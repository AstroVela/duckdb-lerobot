# Additional correctness checks

`Quality CI` runs on pull requests, pushes to `v1.5-variegata`, manual runs,
and nightly at 19:23 UTC. All three jobs fail on detected errors. They supplement
Native CI's SQL, FFmpeg, ASAN/UBSAN/LSan and LeRobot conformance tests.

- **TSan** builds the pinned DuckDB core, native readers and extension tests
  with Clang 18 ThreadSanitizer. It runs the controlled codec executor,
  producer cancellation/lifetime, nested reader and timestamp/cache suites.
  FFmpeg and jemalloc are disabled, so this checks extension concurrency with
  instrumented host code and intercepted allocation. Codec-library internals
  and every possible SQL schedule are outside this job's scope. There are no
  race suppressions or successful retries after a sanitizer error.
- **libFuzzer** instruments production extension code for coverage and builds
  DuckDB and the harness with ASAN/UBSAN. It exercises portable paths, COPY
  options and FEATURES JSON/schema validation, depth parameter bounds, video
  dimensions/budgets (including paired resize dimensions) and temporal rounding
  against an independent ties-to-even oracle. It never executes a COPY or lets
  input select a file or network address. Inputs are limited to 4 KiB. Only
  input-rejection exception classes are accepted; internal exceptions,
  sanitizer failures and invariant failures fail the job.
- **clang-tidy 18** checks every `src/**/*.cpp` with FFmpeg enabled and the
  explicit defect checks in `.clang-tidy`. Missing compilation units fail the
  job. The dedicated driver preserves this configuration; the upstream tidy
  target replaces it with DuckDB's broader style configuration.

Fuzzing first replays all generated seeds and checked-in regression inputs.
It then mutates them for 120 seconds on PRs/pushes/manual runs or 30 minutes
nightly. Per-input timeout is 30 seconds and the RSS limit is 4 GiB. GitHub
artifacts retain crash inputs and the evolved corpus for 14 days, including
failed runs. Replay a downloaded failure with the **same revision and build**:

```sh
build/quality/extension/lerobot/test/lerobot_validation_fuzz path/to/crash-input
```

After fixing a failure, minimize the reproducer and save it under
[`../fuzz/regressions`](../fuzz/regressions/README.md) for mandatory replay.
Artifacts are not executed by a privileged workflow or restored from another
PR. Nightly runs start from repository seeds/regressions; an evolved corpus
can also be downloaded and passed to the fuzzer locally.

## Local commands

Install Clang/clang-tidy/LLVM 18, Ninja and FFmpeg development headers. Use
separate TSan and ASAN build directories:

```sh
git submodule update --init --recursive
git -C duckdb apply ../test/patches/duckdb-1.5.5-cached-eof.patch
CC=clang-18 CXX=clang++-18 cmake -S duckdb -B build/tsan -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDUCKDB_EXTENSION_CONFIGS="$PWD/extension_config.cmake" \
  -DOVERRIDE_GIT_DESCRIBE=v1.5.5-0-gd8cdaa33fd \
  -DBUILD_UNITTESTS=ON -DBUILD_SHELL=OFF -DENABLE_JEMALLOC=OFF \
  -DFORCE_ASSERT=ON -DLEROBOT_ENABLE_FFMPEG=OFF \
  -DENABLE_THREAD_SANITIZER=ON -DENABLE_SANITIZER=OFF -DENABLE_UBSAN=OFF \
  -DCMAKE_C_FLAGS=-fsanitize=thread
cmake --build build/tsan --parallel 2 --target \
  lerobot_codec_executor_test lerobot_video_producer_test lerobot_nested_query_test
for suite in codec_executor video_producer nested_query; do
  TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
    "build/tsan/extension/lerobot/test/lerobot_${suite}_test" || exit 1
done
```

The GitHub Ubuntu runner sets `vm.mmap_rnd_bits=28` for Clang 18's TSan shadow
address space. A local runtime mapping failure is an environment failure,
not a passed race check; use a host with a supported address layout.

For fuzzing, use the same configure command with `build/fuzz`,
`ENABLE_THREAD_SANITIZER=OFF`, `ENABLE_SANITIZER=ON`, `ENABLE_UBSAN=ON`,
`CMAKE_C_FLAGS=-fsanitize=address,undefined` and `LEROBOT_BUILD_FUZZERS=ON`:

```sh
cmake --build build/fuzz --parallel 2 --target lerobot_validation_fuzz
python3 test/fuzz/make_corpus.py build/fuzz/corpus
mkdir -p build/fuzz/artifacts
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
build/fuzz/extension/lerobot/test/lerobot_validation_fuzz \
  -runs=0 -timeout=30 -rss_limit_mb=4096 build/fuzz/corpus
build/fuzz/extension/lerobot/test/lerobot_validation_fuzz \
  -max_total_time=120 -max_len=4096 -timeout=30 -rss_limit_mb=4096 \
  -dict=test/fuzz/validation.dict -artifact_prefix=build/fuzz/artifacts/ build/fuzz/corpus
git -C duckdb apply --reverse ../test/patches/duckdb-1.5.5-cached-eof.patch
```

For Tidy, configure a release build with `DISABLE_UNITY=ON`,
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`, FFmpeg enabled and both ASAN/UBSAN disabled,
then run `python3 test/quality/run_tidy.py --build-dir build/tidy`. The complete
configuration is in `.github/workflows/quality-ci.yml`; no build is needed.

Performance thresholds remain deferred until a stable dedicated runner and
variance measurements exist. Wasm remains outside the supported target matrix.
