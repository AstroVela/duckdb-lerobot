"""Consume contiguous CPU uint8 NCHW Torch batches, including conversion/stack."""

import hashlib
import numpy as np
import torch

from benchmarks.common import request_keys, validate_keys


def as_chw(frame):
    image = frame.image
    if not isinstance(image, torch.Tensor):
        image = np.asarray(image)
        # DuckDB BLOBs expose read-only memory; give Torch a writable buffer.
        if not image.flags.writeable:
            image = image.copy()
        image = torch.from_numpy(image)
    if frame.layout == "HWC":
        image = image.permute(2, 0, 1)
    elif frame.layout != "CHW":
        raise ValueError("unknown image layout")
    if (
        image.ndim != 3
        or image.shape[0] != 3
        or image.dtype != torch.uint8
        or image.device.type != "cpu"
    ):
        raise ValueError("expected CPU RGB uint8 CHW tensor")
    return image


def consume(adapter, job, validate=False):
    keys, records, batch_shapes = [], [], []
    total_bytes = 0
    for frames in adapter.batches():
        batch = torch.stack([as_chw(frame) for frame in frames]).contiguous()
        batch_shapes.append(list(batch.shape))
        total_bytes += batch.numel()
        for frame, image in zip(frames, batch, strict=True):
            keys.append(frame.key)
            height, width, channels = job["camera_shapes"][frame.key[2]]
            if list(image.shape) != [channels, height, width]:
                raise ValueError("decoded resolution differs from the dataset manifest")
            if validate:
                records.append(
                    {
                        "key": frame.key,
                        "shape": list(image.shape),
                        "sha256": hashlib.sha256(image.numpy()).hexdigest(),
                    }
                )
    validate_keys(
        keys, request_keys(job["requests"], job["config"]["dataset"]["cameras"])
    )
    size = job["config"]["batch_size"]
    expected = [min(size, len(keys) - offset) for offset in range(0, len(keys), size)]
    if [shape[0] for shape in batch_shapes] != expected:
        raise ValueError("adapter did not deliver the agreed batch boundaries")
    return {
        "images": len(keys),
        "bytes": total_bytes,
        "batch_shapes": batch_shapes,
        "records": records,
    }
