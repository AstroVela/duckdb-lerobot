"""DuckDB metadata routing followed by the project's optional TorchCodecReader."""

from benchmarks.adapters.base import Frame
from benchmarks.adapters.duckdb import connect, requests_sql
from benchmarks.common import file_hash


class Adapter:
    def __init__(self, job):
        import torch
        from torchcodec.decoders import VideoDecoder  # noqa: F401
        import duckdb_lerobot.reader
        from duckdb_lerobot import TorchCodecReader

        torch.set_num_threads(job["config"]["threads"])
        self.connection = connect(job["extension"], job["config"]["threads"])
        self.reader = TorchCodecReader(
            self.connection,
            job["dataset_root"],
            batch_size=job["config"]["batch_size"],
            max_cached_decoders=4,
            num_ffmpeg_threads=job["config"]["codec_threads"],
        )
        self.query = requests_sql(job["requests"], job["config"]["dataset"]["cameras"])
        self.metadata = {
            "api": "TorchCodecReader.batches",
            "max_cached_decoders": 4,
            "codec_threads": job["config"]["codec_threads"],
            "reader_sha256": file_hash(duckdb_lerobot.reader.__file__),
            "query": self.query,
        }

    def batches(self):
        for batch in self.reader.batches(self.query):
            yield [
                Frame(
                    [t.episode_index, t.target_frame_index, t.video_key], image, "CHW"
                )
                for t, image in zip(batch.targets, batch.images, strict=True)
            ]

    def close(self):
        self.reader.close()
        self.connection.close()
