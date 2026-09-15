"""Unmodified upstream LeRobotDataset, using its public uint8 TorchCodec path."""

from benchmarks.adapters.base import Frame, chunks


class Adapter:
    def __init__(self, job):
        import torch
        from lerobot.datasets.lerobot_dataset import LeRobotDataset
        from torchcodec.decoders import VideoDecoder  # noqa: F401

        torch.set_num_threads(job["config"]["threads"])
        self.job = job
        self.dataset = LeRobotDataset(
            repo_id=job["config"]["dataset"]["repo_id"],
            root=job["dataset_root"],
            video_backend="torchcodec",
            return_uint8=True,
            tolerance_s=1e-4,
        )
        self.metadata = {
            "api": "LeRobotDataset.__getitem__(index), return_uint8=True",
            "backend": "torchcodec",
            "codec_threads": "upstream VideoDecoder default",
            "seek_mode": "upstream approximate",
            "upstream_modified": False,
        }

    def batches(self):
        cameras = self.job["config"]["dataset"]["cameras"]
        pending = []
        for request in self.job["requests"]:
            row = self.dataset[request["index"]]
            for camera in cameras:
                pending.append(
                    Frame(
                        [int(row["episode_index"]), int(row["frame_index"]), camera],
                        row[camera],
                        "CHW",
                    )
                )
                if len(pending) == self.job["config"]["batch_size"]:
                    yield pending
                    pending = []
        yield from chunks(pending, self.job["config"]["batch_size"])

    def close(self):
        pass  # Each case owns a subprocess; upstream caches die with that process.
