# Distributing and rebuilding duckdb-lerobot

The project's own source remains Apache-2.0. The default binary statically
links LGPL FFmpeg, libaom, dav1d, and zlib. Their licenses apply alongside the
project's license. Selecting a codec in SQL does not change the license of
the libraries already linked into a binary.

## Published downloads

[v0.1.0 for DuckDB v1.5.5](https://github.com/AstroVela/duckdb-lerobot/releases/tag/v0.1.0)
provides the source, dependency licenses, and rebuild materials for commit
`901f26a6639c3f3ba763a4df758d83e197694806`, the initial community release.
Choose the platform archive matching `PRAGMA platform;` in DuckDB. For example,
on Linux x86-64:

```sh
curl -fLO https://github.com/AstroVela/duckdb-lerobot/releases/download/v0.1.0/lerobot-v0.1.0-duckdb-v1.5.5-linux_amd64.tar.gz
curl -fLO https://github.com/AstroVela/duckdb-lerobot/releases/download/v0.1.0/SHA256SUMS
sha256sum --ignore-missing -c SHA256SUMS
tar -xzf lerobot-v0.1.0-duckdb-v1.5.5-linux_amd64.tar.gz
cd lerobot-v0.1.0-duckdb-v1.5.5-linux_amd64
sha256sum -c SHA256SUMS
```

Each platform archive contains the signed community binary under `community/`,
the original project CI binary, and its unchanged `release/` directory described
below. The archive's `README.md` records both build runs, pinned inputs, and
binary checksums. The source bundle's manifest describes the project CI build;
the separately built community binary has its own checksum. Rebuilding does not
require either binary or a signing key.

Use these platform archives for rebuilding. GitHub's automatic **Source code**
downloads omit submodules and dependency sources. For normal installation, use
`INSTALL lerobot FROM community; LOAD lerobot;`.

## Default release

Build from a checkout containing all source files and initialized submodules.
Check out vcpkg at the `builtin-baseline` in `vcpkg.json`, bootstrap it, and use
its toolchain:

```sh
VCPKG_BINARY_SOURCES=clear make release GEN=ninja LEROBOT_PACKAGE_RELEASE=1 \
  VCPKG_TARGET_TRIPLET=x64-linux-release \
  VCPKG_TOOLCHAIN_PATH=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
```

Choose the static triplet matching the target platform. CI uses the platform
matrix from `extension-ci-tools`. Its `v1.5-variegata` branch may advance; each
source bundle records and includes the actual checked-out source, including
DuckDB and CI tools. Release packaging collects tracked files only. Commit new
source files before making a release; generated CI environment files and
untracked files are excluded.

The build queries the license and configuration APIs of all four linked
FFmpeg libraries. The release packager additionally checks static library
paths and hashes against installed vcpkg SPDX records, requires the reviewed
runtime dependency set, and collects the matching source archives and port
recipes. Missing source or licenses, GPL/nonfree FFmpeg, and unexpected runtime
dependencies fail the build. Binary caching is disabled in distribution CI so
the matching source archives are available.

Each native CI artifact contains `lerobot.duckdb_extension` and `release/`:

- `LICENSE` and `THIRD_PARTY_NOTICES.md` identify the project and dependencies.
- `lerobot-source.tar.gz` contains the extension and DuckDB source, CI tools,
  vcpkg history, dependency source archives, exact port recipes and patches,
  per-package license texts/SPDX records, and `rebuild.py`.
- `distribution-manifest.json` records revisions, FFmpeg license/configuration,
  dependency versions, rebuild options, source hashes, and the extension hash.
- `SHA256SUMS` covers the accompanying release files.

Publish the binary **and its matching release directory** together, retaining
them at the same download location. GitHub Actions artifacts expire; copy the
materials to durable release storage when publishing. An upstream source URL
alone is not the corresponding-source package for a static binary. If signing
or compressing the binary changes it after this build, preserve the association
with this source bundle and record the final download's checksum separately.

## Rebuild and replace FFmpeg

Use the same OS/architecture as the artifact and a compatible C/C++ compiler.
Install Git, Python 3.8+, CMake, Ninja, pkg-config, and the platform development
tools; x86/x64 builds also need NASM. Windows requires a Visual Studio developer
command prompt and short paths without spaces. Build tools may be downloaded
during vcpkg bootstrap/configuration; dependency source is included in the
bundle. This is a source rebuild, not a promise of byte-identical output.

```sh
cd release
sha256sum -c SHA256SUMS
mkdir unpacked
tar -xzf lerobot-source.tar.gz -C unpacked
python3 unpacked/rebuild.py --verify-only
python3 unpacked/rebuild.py --work-dir /path/to/fresh-work --jobs 4
```

To modify FFmpeg, supply a patch relative to the FFmpeg source root:

```sh
python3 unpacked/rebuild.py --work-dir /path/to/modified-work \
  --ffmpeg-patch /path/to/my-ffmpeg.patch --jobs 4
```

The script verifies the original materials, copies them into a new directory,
applies the patch through the bundled FFmpeg port, disables dependency binary
caches, rebuilds, relinks the extension, and runs the SQL suite against the
rebuilt extension file, including portable RGB/PNG tests. Alternatively, use `--configure-only`, edit the copied
extension/DuckDB sources or `ports/` recipes, then rerun CMake and build in the
printed build directory. Changes to a dependency recipe require reconfiguring
CMake so vcpkg rebuilds that dependency.

The rebuilt extension is in `build/extension/lerobot/lerobot.duckdb_extension`.
Use a matching DuckDB version and enable unsigned extensions to load a local
rebuild, for example `duckdb -unsigned` followed by `LOAD '/path/to/lerobot.duckdb_extension';`.
No publisher signing key is required for this local use. Recipients may modify
and relink the LGPL components and reverse-engineer the combined work to debug
those modifications as permitted by the applicable LGPL terms. Do not impose
additional terms that remove those rights.

## Custom builds

Default releases support RGB AV1/PNG and reading supported media. They omit
x265 and cannot write lossless HEVC Main12 depth videos. The explicit
`gpl-codecs` manifest feature enables x265 for custom builds. System-library
development builds using GPL FFmpeg require `-DLEROBOT_ALLOW_GPL=ON`.
Neither option is accepted by `LEROBOT_PACKAGE_RELEASE=ON`.

These custom artifacts require their own license review and distribution
materials; do not label them as the default LGPL release. FFmpeg builds with
`--enable-nonfree` are rejected even with the GPL development option.

The checks cover this project's default dependency configuration. They do not
resolve codec patent licensing or certify every downstream application's
distribution terms. See [FFmpeg's license guidance](https://ffmpeg.org/legal.html)
and the full license texts in `licenses/` inside the bundle, including LGPL
section 6 for the static relinking obligations.
