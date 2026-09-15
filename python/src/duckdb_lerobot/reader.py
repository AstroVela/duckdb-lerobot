from __future__ import annotations

import math
from collections import OrderedDict
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any, Iterator, Sequence

if TYPE_CHECKING:
    from duckdb import DuckDBPyConnection
    from torch import Tensor


@dataclass(frozen=True)
class VideoTarget:
    request_id: int
    target_ordinal: int
    episode_index: int
    frame_index: int
    video_key: str
    delta_index: int
    delta_timestamp: float
    delta_frame_offset: int
    is_padding: bool
    target_frame_index: int
    timestamp: float
    video_path: str
    video_timestamp: float
    channels: int
    target_id: int


@dataclass(frozen=True)
class DecodedBatch:
    """Aligned rows and uint8 CHW tensors, permitting different camera sizes.

    Images are views; duplicate targets may share storage. Clone an image before
    an in-place transform. Retaining batches also retains their image storage.
    """

    targets: tuple[VideoTarget, ...]
    images: tuple[Tensor, ...]
    decoded_timestamps: tuple[float, ...]


class TorchCodecReader:
    """Resolve targets in DuckDB and decode local constant-frame-rate videos.

    The caller owns the connection and loads the matching lerobot extension.
    Use a dedicated connection while iterating: another query replaces its active
    result. Instances are single-threaded and must be constructed in each worker.
    Python opens trusted local videos directly, outside DuckDB's filesystem.
    """

    def __init__(
        self,
        connection: DuckDBPyConnection,
        dataset: str | Path,
        *,
        device: str = "cpu",
        batch_size: int = 32,
        max_cached_decoders: int = 4,
        num_ffmpeg_threads: int = 1,
        tolerance: float = 1e-4,
    ) -> None:
        root = str(dataset)
        if "://" in root:
            raise ValueError(
                "Download the dataset locally before using TorchCodecReader"
            )
        for name, value in (
            ("batch_size", batch_size),
            ("max_cached_decoders", max_cached_decoders),
            ("num_ffmpeg_threads", num_ffmpeg_threads),
        ):
            if isinstance(value, bool) or not isinstance(value, int) or value < 1:
                raise ValueError(f"{name} must be a positive integer")
        if not math.isfinite(tolerance) or tolerance < 0:
            raise ValueError("tolerance must be finite and non-negative")
        if (
            device != "cpu"
            and device != "cuda"
            and not (device.startswith("cuda:") and device[5:].isdigit())
        ):
            raise ValueError("device must be cpu, cuda, or cuda:<index>")
        self.connection = connection
        self.dataset = Path(root).expanduser().resolve(strict=True)
        if not self.dataset.is_dir():
            raise ValueError("dataset must be a local directory")
        self.device = device
        self.batch_size = batch_size
        self.max_cached_decoders = max_cached_decoders
        self.num_ffmpeg_threads = num_ffmpeg_threads
        self.tolerance = tolerance
        self._decoders: OrderedDict[str, Any] = OrderedDict()
        self._active = False

    def close(self) -> None:
        """Release cached decoders without closing the caller's connection."""
        if self._active:
            raise RuntimeError(
                "Close the active batch iterator before closing the reader"
            )
        self._decoders.clear()

    def __enter__(self) -> TorchCodecReader:
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def _decoder(self, path: str) -> Any:
        resolved = Path(path).resolve(strict=True)
        if not resolved.is_relative_to(self.dataset):
            raise ValueError("Video files must be inside the local dataset directory")
        key = str(resolved)
        if key in self._decoders:
            self._decoders.move_to_end(key)
            return self._decoders[key]
        try:
            from torchcodec.decoders import VideoDecoder
        except (ImportError, RuntimeError) as exc:
            raise RuntimeError(
                "TorchCodec could not load. Install a TorchCodec version compatible "
                "with your PyTorch build and its required shared FFmpeg libraries."
            ) from exc
        if len(self._decoders) >= self.max_cached_decoders:
            self._decoders.popitem(last=False)
        decoder = VideoDecoder(
            key,
            device=self.device,
            dimension_order="NCHW",
            num_ffmpeg_threads=self.num_ffmpeg_threads,
            seek_mode="exact",
        )
        self._decoders[key] = decoder
        return decoder

    def batches(
        self,
        requests_sql: str,
        parameters: Sequence[Any] = (),
        *,
        delta_timestamps: Sequence[float] = (0.0,),
    ) -> Iterator[DecodedBatch]:
        """Execute a target SELECT and yield at most batch_size image rows.

        Required columns: request_id, episode_index, frame_index, video_key,
        delta_index. Each input row selects one delta_timestamps entry. Use an
        explicit ORDER BY in the SELECT when request order matters. Positional
        '?' parameters are forwarded to DuckDB; identifiers are SQL, not values.
        The reader numbers input rows with a zero-based target_id before routing
        and restores that order afterwards. target_ordinal is the operator's
        execution-local counter and must not be used to align samples.
        """
        if self._active:
            raise RuntimeError("Only one batch iterator may be active per reader")
        deltas = [float(value) for value in delta_timestamps]
        if not deltas or not all(math.isfinite(value) for value in deltas):
            raise ValueError("delta_timestamps must contain finite values")
        query = requests_sql.strip().removesuffix(";")
        # Never select image, decoded_timestamp, width or height: these columns
        # would make DuckDB decode the video before TorchCodec sees the targets.
        columns = ", ".join(VideoTarget.__dataclass_fields__)
        # Capture the SELECT's order before the table-in/out operator can assign
        # counters in parallel. request_id may repeat, so it cannot identify an
        # individual occurrence. Project only the required input columns; the
        # adapter owns target_id, including when the SELECT has extra columns.
        ordered_requests = f"""
            SELECT request_id, episode_index, frame_index, video_key, delta_index,
                   row_number() OVER () - 1 AS target_id
            FROM ({query}) AS requests
        """
        sql = f"""
            SELECT {columns}
            FROM lerobot_video_targets(
                ?, ({ordered_requests}), delta_timestamps := ?, tolerance := ?
            )
            ORDER BY target_id
        """
        # Direct Python video I/O cannot honor DuckDB's restricted filesystem.
        if not self.connection.execute(
            "SELECT current_setting('enable_external_access')"
        ).fetchone()[0]:
            raise ValueError("TorchCodecReader requires enable_external_access=true")
        self._active = True
        try:
            result = self.connection.execute(
                sql, [str(self.dataset), *parameters, deltas, self.tolerance]
            )
            while rows := result.fetchmany(self.batch_size):
                targets = tuple(VideoTarget(*row) for row in rows)
                yield self._decode(targets)
        finally:
            self._active = False

    def _decode(self, targets: tuple[VideoTarget, ...]) -> DecodedBatch:
        images: list[Any] = [None] * len(targets)
        timestamps = [0.0] * len(targets)
        groups: dict[str, list[int]] = {}
        for position, target in enumerate(targets):
            if target.channels != 3:
                raise ValueError(
                    "TorchCodecReader supports RGB videos, not depth video"
                )
            groups.setdefault(target.video_path, []).append(position)
        for path, positions in groups.items():
            decoder = self._decoder(path)
            fps = decoder.metadata.average_fps
            begin = decoder.metadata.begin_stream_seconds
            if fps is None or not math.isfinite(fps) or fps <= 0 or begin is None:
                raise ValueError(
                    f"Video has no usable frame-rate/timestamp metadata: {path}"
                )
            # CFR index lookup avoids selecting the previous frame when a
            # float32 dataset timestamp falls just below the true video PTS.
            indices = [
                round((targets[p].video_timestamp - begin) * fps) for p in positions
            ]
            unique_indices = sorted(set(indices))
            frames = decoder.get_frames_at(indices=unique_indices)
            actual_pts = frames.pts_seconds.tolist()
            offsets = {index: offset for offset, index in enumerate(unique_indices)}
            for position, index in zip(positions, indices):
                offset = offsets[index]
                actual = float(actual_pts[offset])
                target = targets[position]
                if (
                    not math.isfinite(actual)
                    or abs(actual - target.video_timestamp) > self.tolerance
                ):
                    raise ValueError(
                        f"Video timestamp mismatch for {path}, request {target.request_id}: "
                        f"expected {target.video_timestamp}, decoded {actual}, "
                        f"tolerance {self.tolerance}. This reader requires aligned CFR video."
                    )
                images[position] = frames.data[offset]
                timestamps[position] = actual
        return DecodedBatch(targets, tuple(images), tuple(timestamps))
