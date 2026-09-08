#!/usr/bin/env python3
"""Reject media references that bypass DuckDB's filesystem and access policy.

Uses only stdlib, the committed H.264 fixture and ffmpeg. All network traffic
stays on an ephemeral loopback server, and all media is generated test data.
"""

import argparse
import csv
import functools
import http.server
import io
import json
import shutil
import subprocess
import tempfile
import threading
from pathlib import Path
from xml.sax.saxutils import escape


def quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def check_access(duckdb, extension, ffmpeg, workspace):
    root = workspace / "dataset"
    (root / "meta/episodes").mkdir(parents=True)
    outside = workspace / "outside-dataset"
    outside.mkdir()
    source = Path(__file__).resolve().parents[1] / "data/lerobot/long-20701.mp4"
    subprocess.run(
        [
            ffmpeg,
            "-v",
            "error",
            "-i",
            str(source),
            "-map",
            "0:v:0",
            "-frames:v",
            "4",
            "-c",
            "copy",
            "-f",
            "dash",
            str(outside / "manifest.mpd"),
        ],
        check=True,
        capture_output=True,
        timeout=30,
    )
    manifest = (outside / "manifest.mpd").read_text()
    (root / "meta/info.json").write_text(
        json.dumps(
            {
                "codebase_version": "v3.0",
                "data_path": "frames.parquet",
                "video_path": "video.mp4",
                "fps": 2,
                "total_episodes": 1,
                "total_frames": 1,
                "total_tasks": 0,
                "features": {"camera": {"dtype": "video", "shape": [16, 16, 3]}},
            }
        )
    )

    def query(sql):
        result = subprocess.run(
            [str(duckdb), "-unsigned", "-csv", "-noheader", "-batch"],
            input=f"LOAD {quote(extension)}; {sql}",
            text=True,
            capture_output=True,
            timeout=30,
        )
        # Expected SQL errors also exit nonzero. Never let a crash or a
        # sanitizer failure during cleanup masquerade as the expected denial.
        assert result.returncode in (0, 1), (result.returncode, result.stderr)
        for diagnostic in ("AddressSanitizer", "LeakSanitizer", "UndefinedBehaviorSanitizer", "runtime error:"):
            assert diagnostic not in result.stderr, result.stderr
        return result

    setup_sql = f"""
COPY (SELECT 0::BIGINT AS episode_index, 1::BIGINT AS length,
  0::BIGINT AS "data/chunk_index", 0::BIGINT AS "data/file_index",
  0::BIGINT AS "videos/camera/chunk_index", 0::BIGINT AS "videos/camera/file_index",
  0.0::DOUBLE AS "videos/camera/from_timestamp", 2.0::DOUBLE AS "videos/camera/to_timestamp")
TO {quote(root / 'meta/episodes/routes.parquet')} (FORMAT parquet);
COPY (SELECT 0::BIGINT AS episode_index, 0::BIGINT AS frame_index, 0.0::DOUBLE AS timestamp)
TO {quote(root / 'frames.parquet')} (FORMAT parquet);
"""
    setup = query(setup_sql)
    assert setup.returncode == 0, setup.stderr
    guard = f"SET allowed_directories=[{quote(str(root) + '/')}]; SET enable_external_access=false; "
    native = query(guard + f"SELECT content FROM read_blob({quote(outside / 'init-stream0.m4s')});")
    assert native.returncode == 1 and "Permission Error" in native.stderr, native.stderr
    relations = (
        f"lerobot_video_frames({quote(root)}, [0])",
        f"lerobot_video_targets({quote(root)}, (SELECT 0 AS request_id, 0 AS episode_index, "
        "0 AS frame_index, 'camera' AS video_key, 0 AS delta_index))",
        f"lerobot_video_windows({quote(root)}, [{{'request_id':0,'episode_index':0,'frame_index':0}}])",
    )

    def check_valid():
        shutil.copyfile(source, root / "video.mp4")
        for relation in relations:
            result = query(guard + f"SELECT octet_length(image) FROM {relation};")
            assert result.returncode == 0, (relation, result.stderr)
            assert list(csv.reader(io.StringIO(result.stdout))) == [["768"]], result.stdout

    def check_manifest(base_url):
        # Disguise DASH as the dataset's normal .mp4 shard. File extension
        # validation alone must not allow the demuxer to follow this BaseURL.
        assert "<Period " in manifest
        (root / "video.mp4").write_text(
            manifest.replace("<Period ", f"<BaseURL>{escape(base_url)}</BaseURL>\n<Period ", 1)
        )
        for policy in ("", guard):
            for relation in relations:
                result = query(policy + f"SELECT octet_length(image) FROM {relation};")
                assert result.returncode == 1, ("external media was decoded", relation, result.stdout)
                assert "LeRobot video" in result.stderr, (relation, result.stderr)

    check_valid()
    check_manifest(outside.as_uri() + "/")
    requests = []

    class Handler(http.server.SimpleHTTPRequestHandler):
        def do_GET(self):
            requests.append(("GET", self.path))
            super().do_GET()

        def do_HEAD(self):
            requests.append(("HEAD", self.path))
            super().do_HEAD()

        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Handler, directory=str(outside)))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        check_manifest(f"http://127.0.0.1:{server.server_port}/")
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    assert requests == [], requests
    check_valid()
    print("Video access: 6 valid MP4 reads, 12 rejected playlists, zero HTTP requests")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", type=Path, required=True)
    parser.add_argument("--extension", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()
    Path("build").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="video-access-", dir=Path("build").resolve()) as temporary:
        check_access(args.duckdb.resolve(), args.extension.resolve(), args.ffmpeg, Path(temporary))


if __name__ == "__main__":
    main()
