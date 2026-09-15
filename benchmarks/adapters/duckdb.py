"""SQL target selection, FFmpeg decoding and BLOB transfer into Python."""

from benchmarks.common import quote
from benchmarks.adapters.base import Frame


def connect(extension, threads):
    import duckdb

    connection = duckdb.connect(
        config={"allow_unsigned_extensions": True, "threads": threads}
    )
    connection.execute(f"LOAD {quote(extension)}")
    return connection


def requests_sql(requests, cameras):
    rows = [
        f"({i}, {request['episode_index']}, {request['frame_index']}, {quote(camera)}, 0)"
        for i, (request, camera) in enumerate((r, c) for r in requests for c in cameras)
    ]
    return (
        "SELECT * FROM (VALUES "
        + ",".join(rows)
        + ") AS requests(request_id, episode_index, frame_index, video_key, delta_index) ORDER BY request_id"
    )


class Adapter:
    def __init__(self, job):
        import numpy as np

        self.np = np
        self.job = job
        self.connection = connect(job["extension"], job["config"]["threads"])
        config = job["config"]
        self.query = f"""
            SELECT episode_index, target_frame_index, video_key, image, height, width
            FROM lerobot_video_targets({quote(job["dataset_root"])}, (
                SELECT requests.*, request_id AS target_id FROM (
                    {requests_sql(job["requests"], config["dataset"]["cameras"])}
                ) requests
            ), codec_threads := {config["codec_threads"]}, decode_threads := {config["threads"]})
            ORDER BY target_id
        """
        self.metadata = {
            "api": "lerobot_video_targets -> fetchmany -> RGB uint8 HWC",
            "query": self.query,
            "codec_threads": config["codec_threads"],
            "duckdb_version": self.connection.execute("PRAGMA version").fetchone(),
        }

    def batches(self):
        cursor = self.connection.execute(self.query)
        while rows := cursor.fetchmany(self.job["config"]["batch_size"]):
            yield [
                Frame(
                    [int(episode), int(frame), camera],
                    self.np.frombuffer(blob, dtype=self.np.uint8).reshape(
                        height, width, 3
                    ),
                    "HWC",
                )
                for episode, frame, camera, blob, height, width in rows
            ]

    def close(self):
        self.connection.close()
