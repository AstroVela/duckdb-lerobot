"""Daft's public LeRobot reader; recreate the lazy plan for every measurement."""

from benchmarks.adapters.base import Frame, chunks


class Adapter:
    def __init__(self, job):
        import daft
        from daft.datasets import lerobot

        self.daft = daft
        self.lerobot = lerobot
        self.job = job
        self.metadata = {
            "api": "daft.datasets.lerobot.read -> filter -> collect -> to_pydict",
            "codec_threads": "Daft/PyAV public API default",
        }

    def batches(self):
        cameras = self.job["config"]["dataset"]["cameras"]
        indices = [r["index"] for r in self.job["requests"]]
        # collect() mutates/caches a DataFrame; never reuse a collected plan.
        frame = self.lerobot.read(self.job["dataset_root"], load_video_frames=cameras)
        data = (
            frame.where(self.daft.col("index").is_in(indices))
            .select("index", "episode_index", "frame_index", *cameras)
            .collect()
            .to_pydict()
        )
        positions = {
            int(index): position for position, index in enumerate(data["index"])
        }
        if len(positions) != len(indices) or len(data["index"]) != len(indices):
            raise ValueError("Daft returned missing or duplicate frames")
        ordered = []
        for request in self.job["requests"]:
            pos = positions[request["index"]]
            for camera in cameras:
                ordered.append(
                    Frame(
                        [
                            int(data["episode_index"][pos]),
                            int(data["frame_index"][pos]),
                            camera,
                        ],
                        data[camera][pos],
                        "HWC",
                    )
                )
        yield from chunks(ordered, self.job["config"]["batch_size"])

    def close(self):
        pass
