#!/usr/bin/env python3
"""Collect the inputs of a default, statically linked LGPL release.

Installed vcpkg SPDX records identify source archives, port files and binary
libraries. Missing inputs or checksum mismatches fail packaging. This is a
release engineering check, not a general-purpose license scanner.
"""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import tempfile

RUNTIME_PORTS = {"ffmpeg", "aom", "dav1d", "zlib"}
FFMPEG_LIBRARIES = {"libavcodec", "libavformat", "libavutil", "libswscale"}
# OS/compiler support libraries, excluded from corresponding source under the
# LGPL system-component exception. Codec libraries must never be added here.
SYSTEM_LIBRARIES = {
    "dl",
    "m",
    "pthread",
    "atomic",
    "advapi32",
    "bcrypt",
    "crypt32",
    "gdi32",
    "mfuuid",
    "mfplat",
    "dxgi",
    "d3d11",
    "d3d12",
    "ole32",
    "oleaut32",
    "psapi",
    "secur32",
    "shlwapi",
    "strmiids",
    "user32",
    "uuid",
    "vfw32",
    "ws2_32",
    "usp10",
    "cfgmgr32",
    "rpcrt4",
    "appkit",
    "avfoundation",
    "coreimage",
    "audiotoolbox",
    "videotoolbox",
    "corefoundation",
    "coremedia",
    "corevideo",
    "coreaudio",
    "coregraphics",
    "coreservices",
    "applicationservices",
    "metal",
    "foundation",
    "security",
    "opencl",
    "opengl",
}
REBUILD_OPTIONS = (
    "CMAKE_BUILD_TYPE",
    "CMAKE_CXX_STANDARD",
    "CMAKE_OSX_DEPLOYMENT_TARGET",
    "OSX_BUILD_ARCH",
    "EXTENSION_STATIC_BUILD",
    "DUCKDB_EXPLICIT_PLATFORM",
    "ENABLE_LTO",
    "ENABLE_JEMALLOC",
    "BUILD_SHELL",
    "BUILD_UNITTESTS",
    "DISABLE_BUILTIN_EXTENSIONS",
    "STATIC_LIBCPP",
)


def run(*args):
    return subprocess.check_output([str(arg) for arg in args]).decode().strip()


def digest(path, algorithm="sha256"):
    hasher = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def cache_values(path):
    values = {}
    for line in path.read_text().splitlines():
        match = re.match(r"([^/#][^:]*):[^=]+=(.*)", line)
        if match:
            values[match[1]] = match[2]
    return values


def checked_relative(name):
    path = PurePosixPath(name)
    if not path.parts or path.is_absolute() or ".." in path.parts or "\\" in name or ":" in name:
        raise ValueError(f"Unsafe archive path: {name}")
    return path.as_posix()


def validate_report(report):
    libraries = report.get("libraries", [])
    if len(libraries) != 4 or {lib.get("name") for lib in libraries} != FFMPEG_LIBRARIES:
        raise ValueError("Expected reports from all four linked FFmpeg libraries")
    for lib in libraries:
        if lib.get("license") not in ("LGPL version 2.1 or later", "LGPL version 3 or later"):
            raise ValueError(f"Default releases require LGPL FFmpeg: {lib}")
        config = lib.get("configuration", "")
        if not config or any(
            flag in config for flag in ("--enable-gpl", "--enable-nonfree", "--enable-libx264", "--enable-libx265")
        ):
            raise ValueError(f"Forbidden or missing FFmpeg configuration: {lib['name']}")


def snapshot(repository, prefix, files, revisions):
    """Include tracked working source recursively, excluding generated CI files.

    Untracked files must never be collected: CI creates environment files that
    can contain credentials in the repository root.
    """
    revisions[prefix] = run("git", "-C", repository, "rev-parse", "HEAD")
    tracked = subprocess.check_output(["git", "-C", str(repository), "ls-files", "--stage", "-z"]).decode().split("\0")
    for entry in filter(None, tracked):
        metadata, name = entry.split("\t", 1)
        mode, _, stage = metadata.split()
        if stage != "0":
            raise ValueError(f"Unmerged source: {repository / name}")
        destination = f"{prefix}/{checked_relative(name)}"
        if mode == "160000":
            snapshot(repository / name, destination, files, revisions)
        else:
            files[destination] = repository / name


def verify_checksum(data, checksums, label):
    if not checksums:
        raise ValueError(f"Missing checksum: {label}")
    for checksum in checksums:
        algorithm = checksum["algorithm"].lower().replace("-", "")
        if hashlib.new(algorithm, data).hexdigest() != checksum["checksumValue"].lower():
            raise ValueError(f"Checksum mismatch: {label}")


def port_files(spdx, name, source_root, vcpkg_root):
    """Recover exactly the recipe files used, including cached registry versions."""
    port = next(p for p in spdx["packages"] if p["SPDXID"] == "SPDXRef-port")
    tree = port["downloadLocation"].rsplit("@", 1)[-1]
    candidates = (
        source_root / "vcpkg_ports" / name,
        source_root / "extension-ci-tools/vcpkg_ports" / name,
        vcpkg_root / "ports" / name,
        vcpkg_root / "buildtrees/versioning_/versions" / name / tree,
    )
    recovered = {}
    for item in spdx["files"]:
        if not item["SPDXID"].startswith("SPDXRef-port-file-"):
            continue
        relative = checked_relative(item["fileName"])
        content = None
        for candidate in candidates:
            path = candidate / relative
            if path.is_file():
                data = path.read_bytes()
                try:
                    verify_checksum(data, item["checksums"], path)
                    content = data
                    break
                except ValueError:
                    continue
        if content is None and re.fullmatch(r"[0-9a-f]{40}", tree):
            result = subprocess.run(
                ["git", "-C", str(vcpkg_root), "show", f"{tree}:{relative}"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            if result.returncode == 0:
                verify_checksum(result.stdout, item["checksums"], f"{name}/{relative}")
                content = result.stdout
        if content is None:
            raise ValueError(f"Cannot recover the installed port recipe: {name}/{relative}")
        recovered[relative] = content
    if "portfile.cmake" not in recovered or "vcpkg.json" not in recovered:
        raise ValueError(f"Incomplete port recipe: {name}")
    return recovered


def source_archives(spdx, name, downloads, hashes, recipe=None):
    resources = [p for p in spdx["packages"] if p["SPDXID"].startswith("SPDXRef-resource-")]
    if name in RUNTIME_PORTS and not resources:
        raise ValueError(f"Missing source resources: {name}")
    result = []
    for resource in resources:
        checksums = resource.get("checksums", [])
        if checksums:
            checksum = next((c for c in checksums if c["algorithm"] == "SHA512"), None)
            if checksum is None:
                raise ValueError(f"Unsupported source checksum: {name}")
            expected = checksum["checksumValue"].lower()
            if name == "vcpkg-tool-meson" and expected == "${download_sha512}":
                # vcpkg's SPDX scanner sees this template before substitution.
                # Recover the constant from the checksum-verified port recipe.
                config = (recipe or {}).get("vcpkg-port-config.cmake", b"").decode()
                match = re.search(r"set\(download_sha512 ([0-9a-f]{128})\)", config)
                if match is None:
                    raise ValueError("Cannot resolve Meson's source checksum")
                expected = match[1]
            archive = hashes.get(expected)
        else:
            # vcpkg_from_git records an immutable git ref, but no archive hash.
            location = resource["downloadLocation"]
            revision = location.rsplit("@", 1)[-1]
            if not location.startswith("git+") or not re.fullmatch(r"[0-9a-f]{40}", revision):
                raise ValueError(f"Unpinned source resource: {location}")
            archive = downloads / f"{name}-{revision}.tar.gz"
        if archive is None or not archive.is_file():
            raise ValueError(f"Missing source archive for {name}: {resource['downloadLocation']}")
        result.append(archive)
    return result


def collect_dependencies(source_root, vcpkg_root, installed, triplet, downloads, files):
    # Downloads contains source archives at its top level. Tool downloads live
    # in subdirectories and need not be included in the corresponding source.
    hashes = {digest(p, "sha512"): p for p in downloads.iterdir() if p.is_file()}
    packages = []
    verified_libraries = {}
    records = sorted(installed.glob("*/share/*/vcpkg.spdx.json"))
    if not records:
        raise ValueError("No installed vcpkg SPDX records")
    for record in records:
        name = record.parent.name
        package_triplet = record.parents[2].name
        spdx = json.loads(record.read_text())
        prefix = installed / package_triplet
        recipe = port_files(spdx, name, source_root, vcpkg_root)
        for relative, data in recipe.items():
            files[f"ports/{package_triplet}/{name}/{relative}"] = data
        files[f"licenses/{package_triplet}/{name}/vcpkg.spdx.json"] = record
        copyright_file = record.parent / "copyright"
        if not copyright_file.is_file() or copyright_file.stat().st_size == 0:
            raise ValueError(f"Missing installed copyright: {record.parent}")
        files[f"licenses/{package_triplet}/{name}/copyright"] = copyright_file
        abi_file = record.parent / "vcpkg_abi_info.txt"
        if abi_file.is_file():
            files[f"licenses/{package_triplet}/{name}/vcpkg_abi_info.txt"] = abi_file
            abi = dict(line.split(" ", 1) for line in abi_file.read_text().splitlines() if " " in line)
            triplet_hash = abi.get("triplet_abi", "").split("-", 1)[0]
            candidates = [
                vcpkg_root / directory / f"{package_triplet}.cmake" for directory in ("triplets", "triplets/community")
            ]
            triplet_file = next((p for p in candidates if p.is_file() and digest(p) == triplet_hash), None)
            if triplet_file is None:
                raise ValueError(f"Release requires a bundled baseline triplet: {package_triplet}")
            files[f"triplets/{package_triplet}.cmake"] = triplet_file
        archives = source_archives(spdx, name, downloads, hashes, recipe)
        for archive in archives:
            files[f"downloads/{archive.name}"] = archive
        for item in spdx["files"]:
            if not item["SPDXID"].startswith("SPDXRef-binary-file-"):
                continue
            relative = checked_relative(item["fileName"])
            path = prefix / relative
            if package_triplet != triplet:
                continue
            if name in RUNTIME_PORTS and re.search(r"\.(so(?:\.[0-9.]+)?|dylib|dll)$", relative):
                raise ValueError(f"Shared dependency in a static distribution: {path}")
            if relative.startswith("lib/") and path.suffix in (".a", ".lib"):
                verify_checksum(path.read_bytes(), item["checksums"], path)
                verified_libraries[path.resolve()] = name
        package = next(p for p in spdx["packages"] if p["SPDXID"] == "SPDXRef-port")
        packages.append(
            {
                "name": name,
                "triplet": package_triplet,
                "version": package["versionInfo"],
                "source_archives": [f"downloads/{a.name}" for a in archives],
            }
        )
    return packages, verified_libraries


def validate_links(link_file, verified_libraries):
    found = set()
    runtime_ports = set()
    for line in link_file.read_text().splitlines():
        if not line:
            continue
        path = Path(line)
        owner = verified_libraries.get(path.resolve())
        system_name = re.split(r"\.(?:a|lib|so|dylib|framework)(?:\.|$)", path.name.lower())[0]
        if system_name.startswith("lib"):
            system_name = system_name[3:]
        if owner:
            runtime_ports.add(owner)
        elif line not in ("-pthread", "-pthreads") and system_name not in SYSTEM_LIBRARIES:
            raise ValueError(f"Unverified link input: {path}")
        stem = path.stem.lower()
        normalized = stem if stem.startswith("lib") else "lib" + stem
        if normalized in FFMPEG_LIBRARIES:
            if path.resolve() not in verified_libraries:
                raise ValueError(f"FFmpeg link input is outside the verified static packages: {path}")
            found.add(normalized)
    if found != FFMPEG_LIBRARIES:
        raise ValueError(f"Could not verify every FFmpeg link input: {sorted(found)}")
    if runtime_ports != RUNTIME_PORTS:
        raise ValueError(f"Unreviewed linked runtime dependency set: {sorted(runtime_ports)}")


def write_archive(destination, files):
    with tarfile.open(destination, "w:gz") as archive:
        for name, source in sorted(files.items()):
            checked_relative(name)
            if isinstance(source, bytes):
                info = tarfile.TarInfo(name)
                info.size = len(source)
                archive.addfile(info, io.BytesIO(source))
            else:
                archive.add(source, arcname=name, recursive=False)


def host_triplet(cache, packages):
    if cache.get("VCPKG_HOST_TRIPLET"):
        return cache["VCPKG_HOST_TRIPLET"]
    # vcpkg chooses a host default internally when the toolchain has no explicit
    # host triplet. The installed host-only CMake helper records that choice.
    candidates = {p["triplet"] for p in packages if p["name"] == "vcpkg-cmake-get-vars"}
    if len(candidates) != 1:
        raise ValueError("Cannot determine host triplet; set VCPKG_HOST_TRIPLET explicitly")
    return candidates.pop()


def package(args):
    source_root, build, vcpkg, installed = (
        Path(p).resolve() for p in (args.source_root, args.build_dir, args.vcpkg_root, args.installed_dir)
    )
    extension = Path(args.extension).resolve()
    report = json.loads((build / "lerobot-ffmpeg-license.json").read_text())
    validate_report(report)
    cache = cache_values(build / "CMakeCache.txt")
    baseline = json.loads((source_root / "vcpkg.json").read_text())["builtin-baseline"]
    if run("git", "-C", vcpkg, "rev-parse", "HEAD") != baseline:
        raise ValueError("Release vcpkg checkout must match the manifest builtin-baseline")
    if cache.get("LEROBOT_ALLOW_GPL", "OFF") != "OFF" or cache.get("VCPKG_MANIFEST_FEATURES", ""):
        raise ValueError("Only the default manifest feature set may be packaged")
    if cache.get("VCPKG_CHAINLOAD_TOOLCHAIN_FILE"):
        raise ValueError("Custom chainloaded toolchains need their own source packaging")
    if not extension.is_file():
        raise ValueError(f"Missing extension: {extension}")
    downloads = Path(os.environ.get("VCPKG_DOWNLOADS", str(vcpkg / "downloads")))
    files = {}
    revisions = {}
    snapshot(source_root, "source", files, revisions)
    for required in (
        "source/duckdb/CMakeLists.txt",
        "source/extension-ci-tools/makefiles/duckdb_extension.Makefile",
        "source/LICENSE",
        "source/THIRD_PARTY_NOTICES.md",
        "source/scripts/rebuild_release.py",
        "source/REDISTRIBUTION.md",
        "source/cmake/lerobot_distribution.cmake",
        "source/src/ffmpeg_license_probe.cpp",
    ):
        if required not in files:
            raise ValueError(f"Source snapshot is incomplete: {required}")
    packages, libraries = collect_dependencies(source_root, vcpkg, installed, args.triplet, downloads, files)
    validate_links(build / "lerobot-ffmpeg-link-libraries.txt", libraries)
    files["rebuild.py"] = source_root / "scripts/rebuild_release.py"
    files["README.md"] = source_root / "REDISTRIBUTION.md"
    files["ffmpeg-license.json"] = build / "lerobot-ffmpeg-license.json"
    files["ffmpeg-link-libraries.txt"] = build / "lerobot-ffmpeg-link-libraries.txt"
    manifest = {
        "schema_version": 1,
        "extension_sha256": digest(extension),
        "source_revisions": revisions,
        "vcpkg_revision": baseline,
        "triplet": args.triplet,
        "host_triplet": host_triplet(cache, packages),
        "duckdb_describe": run("git", "-C", source_root / "duckdb", "describe", "--tags", "--long"),
        "cmake_options": {key: cache[key] for key in REBUILD_OPTIONS if cache.get(key)},
        "compiler": {key: cache.get(key, "") for key in ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER")},
        "ffmpeg": report,
        "packages": packages,
    }
    output = extension.parent / "release"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lerobot-package-", dir=build) as tmp:
        bundle = Path(tmp) / "vcpkg.bundle"
        # Include git objects so versioned recipes can be rebuilt without relying
        # on a future upstream checkout or a moving branch.
        run("git", "-C", vcpkg, "bundle", "create", bundle, "HEAD")
        files["vcpkg.bundle"] = bundle
        manifest["files"] = {
            name: (hashlib.sha256(value).hexdigest() if isinstance(value, bytes) else digest(value))
            for name, value in files.items()
            if isinstance(value, bytes) or not value.is_symlink()
        }
        manifest["symlinks"] = {
            name: os.readlink(value)
            for name, value in files.items()
            if not isinstance(value, bytes) and value.is_symlink()
        }
        manifest_bytes = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
        files["distribution-manifest.json"] = manifest_bytes
        archive_path = Path(tmp) / "lerobot-source.tar.gz"
        write_archive(archive_path, files)
        os.replace(archive_path, output / archive_path.name)
    (output / "distribution-manifest.json").write_bytes(manifest_bytes)
    (output / "LICENSE").write_bytes((source_root / "LICENSE").read_bytes())
    (output / "THIRD_PARTY_NOTICES.md").write_bytes((source_root / "THIRD_PARTY_NOTICES.md").read_bytes())
    (output / "REDISTRIBUTION.md").write_bytes((source_root / "REDISTRIBUTION.md").read_bytes())
    release_files = (
        "LICENSE",
        "THIRD_PARTY_NOTICES.md",
        "REDISTRIBUTION.md",
        "distribution-manifest.json",
        "lerobot-source.tar.gz",
    )
    (output / "SHA256SUMS").write_text("".join(f"{digest(output / name)}  {name}\n" for name in release_files))
    print(f"Packaged LGPL release materials: {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("source-root", "build-dir", "vcpkg-root", "installed-dir", "triplet", "extension"):
        parser.add_argument("--" + flag, required=True)
    try:
        package(parser.parse_args())
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Release packaging failed: {error}\n")


if __name__ == "__main__":
    main()
