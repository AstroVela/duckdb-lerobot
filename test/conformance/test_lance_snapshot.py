#!/usr/bin/env python3
"""Check LeRobot route rebinding and retained Blob snapshots in one connection."""

import argparse
import json
import tempfile
from pathlib import Path

import duckdb
import lance
import pyarrow as pa
from lance.blob import BlobArray
from lerobot_lancedb.convert import convert


def quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--extension", type=Path, required=True)
    parser.add_argument("--lance-extension", type=Path, required=True)
    parser.add_argument("--rgb-codec", default="libaom-av1")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="lerobot-lance-snapshot-") as temporary:
        base = Path(temporary)
        parquet, dataset = base / "parquet", base / "lance"
        with duckdb.connect(config={"allow_unsigned_extensions": "true"}) as connection:
            connection.execute(f"LOAD {quote(args.extension)}; LOAD {quote(args.lance_extension)};")
            features = {"camera": {"dtype": "video", "shape": [64, 64, 3]}}
            connection.execute(
                "COPY (SELECT 0::BIGINT AS episode_index, 'move' AS task, "
                "from_hex(repeat(printf('%02x', i * 31 + 20), 64*64*3)) AS camera "
                f"FROM range(3) t(i)) TO {quote(parquet)} "
                f"(FORMAT lerobot, FPS 10, FEATURES {quote(json.dumps(features))}, "
                f"RGB_CODEC {quote(args.rgb_codec)}, VIDEO_WORKERS 1, ENCODER_THREADS 1);"
            )
            convert(dataset, root=parquet)
            route_query = f"SELECT DISTINCT video_path FROM lerobot_video_routes({quote(dataset)}, [0])"
            frames_query = (
                f"SELECT frame_index, image FROM lerobot_video_frames({quote(dataset)}, [0]) ORDER BY frame_index"
            )
            old_uri = connection.execute(route_query).fetchone()[0]
            expected_frames = connection.execute(frames_query).fetchall()
            old_payload = connection.execute("SELECT content FROM read_blob(?)", [old_uri]).fetchone()[0]
            source = lance.dataset(dataset / "videos.lance")
            rows = source.to_table(
                columns=["video_key", "chunk_index", "file_index", "file_size"], with_row_id=True
            ).to_pylist()
            payloads = []
            for row in rows:
                with source.take_blobs("video_bytes", ids=[row.pop("_rowid")])[0] as blob:
                    payloads.append(blob.read())
            assert payloads == [old_payload]
            source.delete("video_key = 'camera'")
            try:
                connection.execute(route_query).fetchall()
            except duckdb.Error as error:
                assert "has no route" in str(error), error
            else:
                raise AssertionError("warm route cache reused a deleted videos.lance row")
            assert connection.execute("SELECT content FROM read_blob(?)", [old_uri]).fetchone()[0] == old_payload
            blobs = BlobArray.from_pylist(payloads)
            table = pa.Table.from_pylist(rows).append_column(pa.field("video_bytes", blobs.type), blobs)
            lance.write_dataset(table, dataset / "videos.lance", mode="append", data_storage_version="2.2")
            new_uri = connection.execute(route_query).fetchone()[0]
            assert new_uri != old_uri, (new_uri, old_uri)
            assert connection.execute(frames_query).fetchall() == expected_frames
            assert connection.execute("SELECT content FROM read_blob(?)", [old_uri]).fetchone()[0] == old_payload
            assert connection.execute("SELECT content FROM read_blob(?)", [new_uri]).fetchone()[0] == old_payload
            print("PASS: warm routes rebind after delete/append; retained old Blob URI preserves original bytes")


if __name__ == "__main__":
    main()
