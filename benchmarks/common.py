"""Shared configuration, provenance and request contracts; standard library only."""

import hashlib
import json
import os
import platform
import re
import subprocess
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCHEMA_VERSION = 1
ENGINES = {
    "video_read": {"duckdb", "daft", "lerobot"},
    "tensor_batch": {"duckdb", "torchcodec"},
}


def quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def read_json(path):
    return json.loads(Path(path).read_text())


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n"
    )
    temporary.replace(path)


def digest(value):
    return hashlib.sha256(
        json.dumps(
            value, sort_keys=True, separators=(",", ":"), allow_nan=False
        ).encode()
    ).hexdigest()


def file_hash(path):
    with Path(path).open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def extension_info(path, commit=None, manifest_path=None):
    result = {"sha256": file_hash(path), "source_commit": commit}
    if manifest_path:
        manifest = read_json(manifest_path)
        if manifest["extension_sha256"] != result["sha256"]:
            raise ValueError(
                "distribution manifest does not describe this extension binary"
            )
        source = manifest["source_revisions"]["source"]
        if commit and commit != source:
            raise ValueError("extension commit differs from its distribution manifest")
        result["source_commit"] = source
        result["distribution"] = {
            key: manifest[key]
            for key in (
                "source_revisions",
                "compiler",
                "cmake_options",
                "packages",
                "ffmpeg",
            )
        }
        result["distribution_manifest_sha256"] = file_hash(manifest_path)
    if result["source_commit"] and not re.fullmatch(
        r"[0-9a-f]{40}", result["source_commit"]
    ):
        raise ValueError("extension source must be a full 40-character commit SHA")
    return result


def payload_files(root):
    """Only dataset payload, excluding download caches and generated reader caches."""
    root = Path(root)
    return {
        str(p.relative_to(root)): file_hash(p)
        for prefix in ("meta", "data", "videos")
        for p in sorted((root / prefix).rglob("*"))
        if p.is_file()
    }


def load_config(path):
    with Path(path).open("rb") as handle:
        config = tomllib.load(handle)
    if config["schema_version"] != SCHEMA_VERSION:
        raise ValueError("unsupported config schema")
    if config["workload"] not in ENGINES:
        raise ValueError("unknown workload")
    engines = config["engines"]
    if (
        not engines
        or len(set(engines)) != len(engines)
        or not set(engines) <= ENGINES[config["workload"]]
    ):
        raise ValueError("invalid or duplicate engines")
    for key in ("warmups", "repeats", "threads", "codec_threads", "batch_size"):
        if type(config[key]) is not int or config[key] < 1:
            raise ValueError(f"{key} must be a positive integer")
    if config["dataset"]["kind"] == "huggingface":
        if not re.fullmatch(r"[0-9a-f]{40}", config["dataset"]["revision"]):
            raise ValueError("dataset revision must be an immutable 40-character SHA")
    elif config["dataset"]["kind"] != "synthetic":
        raise ValueError("unknown dataset kind")
    cameras = config["dataset"]["cameras"]
    if not cameras or len(set(cameras)) != len(cameras) or not all(cameras):
        raise ValueError("cameras must be nonempty and unique")
    names = set()
    for case in config["cases"]:
        if not re.fullmatch(r"[a-z0-9-]+", case["name"]) or case["name"] in names:
            raise ValueError("case names must be unique, safe file names")
        names.add(case["name"])
        if case["rows"] < 1 or case["order"] not in {"sequential", "random"}:
            raise ValueError("invalid case rows or order")
        if case["variant"] not in {"single", "multi"}:
            raise ValueError("unknown dataset variant")
    if not names:
        raise ValueError("at least one case is required")
    return config


def request_keys(requests, cameras):
    return [
        [r["episode_index"], r["frame_index"], camera]
        for r in requests
        for camera in cameras
    ]


def validate_keys(actual, expected):
    if actual != expected:
        raise ValueError("decoded frame keys/order differ from the request manifest")


def verify_packages(packages, lock):
    def normalize(name):
        return re.sub(r"[-_.]+", "-", name).lower()

    actual = {normalize(name): version for name, version in packages.items()}
    for line in Path(lock).read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        name, version = line.split("==")
        if actual.get(normalize(name)) != version:
            raise ValueError(
                f"environment differs from {Path(lock).name}: expected {name}=={version}"
            )


def source_info():
    def git(*args):
        return subprocess.check_output(
            ["git", "-C", str(ROOT), *args], text=True
        ).strip()

    files = {
        str(p.relative_to(ROOT)): file_hash(p)
        for prefix in ("benchmarks", "python/src")
        for p in sorted((ROOT / prefix).rglob("*"))
        if p.is_file()
        and p.suffix in {".py", ".toml", ".txt", ".in"}
        and not any(part.endswith(".egg-info") for part in p.parts)
    }
    return {
        "git_commit": git("rev-parse", "HEAD"),
        "git_dirty": bool(git("status", "--porcelain")),
        "files": files,
        "files_sha256": digest(files),
    }


def machine_info():
    info = {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "cpu_count": os.cpu_count(),
        "python": platform.python_version(),
        "affinity": sorted(os.sched_getaffinity(0))
        if hasattr(os, "sched_getaffinity")
        else None,
    }
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        text = cpuinfo.read_text()
        info["cpu_model"] = next(
            (
                line.split(":", 1)[1].strip()
                for line in text.splitlines()
                if line.startswith("model name")
            ),
            "unknown",
        )
    if hasattr(os, "sysconf"):
        info["memory_bytes"] = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
    return info
