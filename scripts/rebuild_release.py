#!/usr/bin/env python3
"""Verify a release source bundle and rebuild with replaceable FFmpeg sources."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def bundle_path(root, name):
    path = PurePosixPath(name)
    if not path.parts or path.is_absolute() or ".." in path.parts or "\\" in name or ":" in name:
        raise ValueError(f"Unsafe bundle path: {name}")
    result = root / path
    try:
        result.resolve().relative_to(root.resolve())
    except ValueError as error:
        raise ValueError(f"Bundle path escapes its directory: {name}") from error
    return result


def verify_bundle(root):
    manifest = json.loads((root / "distribution-manifest.json").read_text())
    if manifest.get("schema_version") != 1 or not manifest.get("files"):
        raise ValueError("Unsupported or empty distribution manifest")
    for name, expected in manifest["files"].items():
        path = bundle_path(root, name)
        if path.is_symlink():
            raise ValueError(f"Regular file replaced with symlink: {name}")
        hasher = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                hasher.update(block)
        if hasher.hexdigest() != expected:
            raise ValueError(f"Bundle checksum mismatch: {name}")
    for name, target in manifest.get("symlinks", {}).items():
        path = bundle_path(root, name)
        if not path.is_symlink() or os.readlink(path) != target:
            raise ValueError(f"Bundle symlink mismatch: {name}")
    return manifest


def prepare_ports(bundle, destination, manifest, ffmpeg_patch=None):
    for package in manifest["packages"]:
        source = bundle / "ports" / package["triplet"] / package["name"]
        target = destination / package["name"]
        if target.exists():
            # Host and target dependencies must use the same recipe.
            for path in source.rglob("*"):
                if path.is_file() and path.read_bytes() != (target / path.relative_to(source)).read_bytes():
                    raise ValueError(f"Different host/target recipes: {package['name']}")
        else:
            shutil.copytree(source, target)
    if ffmpeg_patch:
        port = destination / "ffmpeg/portfile.cmake"
        content = port.read_text()
        # The bundled FFmpeg recipe obtains sources with one top-level call.
        # Add the user's patch to that call so vcpkg applies it on clean sources
        # and includes it in its cache key.
        pattern = r"(\Avcpkg_from_github\([\s\S]*?\n    PATCHES\n)"
        content, count = re.subn(pattern, r"\1        lerobot-user.patch\n", content, count=1)
        if count != 1:
            raise ValueError("FFmpeg recipe changed; add your patch to its PATCHES list manually")
        shutil.copyfile(ffmpeg_patch, destination / "ffmpeg/lerobot-user.patch")
        port.write_text(content)


def rebuild(args):
    bundle = Path(args.bundle).resolve()
    manifest = verify_bundle(bundle)
    print("Source bundle checksums verified", flush=True)
    if args.verify_only:
        return
    work = Path(args.work_dir).resolve()
    if work.exists():
        raise ValueError("Use a new --work-dir; existing build directories are never overwritten")
    work.mkdir(parents=True)
    source, vcpkg, build = work / "source", work / "vcpkg", work / "build"
    shutil.copytree(bundle / "source", source, symlinks=True)
    run("git", "clone", bundle / "vcpkg.bundle", vcpkg)
    run("git", "-C", vcpkg, "checkout", "--detach", manifest["vcpkg_revision"])
    shutil.copytree(bundle / "downloads", vcpkg / "downloads", dirs_exist_ok=True)
    shutil.copytree(bundle / "triplets", work / "triplets")
    prepare_ports(bundle, work / "ports", manifest, args.ffmpeg_patch)
    environment = os.environ.copy()
    environment.update(
        {
            "VCPKG_BINARY_SOURCES": "clear",
            "VCPKG_ROOT": str(vcpkg),
            "VCPKG_DOWNLOADS": str(vcpkg / "downloads"),
            "VCPKG_MAX_CONCURRENCY": str(args.jobs),
        }
    )
    # Discard machine-specific overlays; all recorded recipes are supplied below.
    for variable in ("VCPKG_OVERLAY_PORTS", "VCPKG_OVERLAY_TRIPLETS", "DONT_LINK"):
        environment.pop(variable, None)
    if os.name == "nt":
        run("cmd", "/c", vcpkg / "bootstrap-vcpkg.bat", "-disableMetrics", env=environment)
    else:
        run("sh", vcpkg / "bootstrap-vcpkg.sh", "-disableMetrics", env=environment)
    options = dict(manifest["cmake_options"])
    options.update(
        {
            "VCPKG_BUILD": "1",
            "CMAKE_TOOLCHAIN_FILE": vcpkg / "scripts/buildsystems/vcpkg.cmake",
            "VCPKG_MANIFEST_DIR": source,
            "VCPKG_TARGET_TRIPLET": manifest["triplet"],
            "VCPKG_HOST_TRIPLET": manifest["host_triplet"],
            "VCPKG_OVERLAY_PORTS": work / "ports",
            "VCPKG_OVERLAY_TRIPLETS": work / "triplets",
            "DUCKDB_EXTENSION_CONFIGS": source / "extension_config.cmake",
            "OVERRIDE_GIT_DESCRIBE": manifest["duckdb_describe"],
            "UNITTEST_ROOT_DIRECTORY": source,
            "ENABLE_UNITTEST_CPP_TESTS": "OFF",
            "BUILD_SHELL": "ON",
            "BUILD_UNITTESTS": "ON",
            "ENABLE_SANITIZER": "OFF",
            "ENABLE_UBSAN": "OFF",
            "LEROBOT_PACKAGE_RELEASE": "OFF",
            "LEROBOT_ALLOW_GPL": "OFF",
            "LEROBOT_ENABLE_FFMPEG": "ON",
        }
    )
    run(
        "cmake",
        "-G",
        "Ninja",
        "-S",
        source / "duckdb",
        "-B",
        build,
        *(f"-D{key}={value}" for key, value in options.items()),
        env=environment,
    )
    if args.configure_only:
        print(f"Configured {build}; edit sources/recipes and run cmake --build {build}")
        return
    run("cmake", "--build", build, "--parallel", args.jobs, env=environment)
    suffix = ".exe" if os.name == "nt" else ""
    environment["LEROBOT_NON_GPL_FFMPEG_TESTS"] = "1"
    environment["DUCKDB_TEST_STATICALLY_LOADED_EXTENSIONS"] = '["core_functions", "parquet", "json"]'
    extension = build / "extension/lerobot/lerobot.duckdb_extension"
    # SQLLogicTest starts with automatic static-extension loading disabled.
    # Load the rebuilt file before any `require lerobot` can load a static copy.
    environment["DUCKDB_TEST_ON_INIT"] = "LOAD '" + extension.as_posix().replace("'", "''") + "';"
    run(build / f"test/unittest{suffix}", "test/*", cwd=source, env=environment)
    print(f"Rebuilt extension: {extension}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", default=Path(__file__).resolve().parent)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--work-dir", default="lerobot-rebuild")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--ffmpeg-patch", type=Path)
    try:
        args = parser.parse_args()
        if args.jobs < 1:
            raise ValueError("--jobs must be positive")
        rebuild(args)
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Rebuild failed: {error}\n")


if __name__ == "__main__":
    main()
