"""Consume ordered contiguous RGB uint8 HWC NumPy arrays in Python."""

import hashlib
import numpy as np

from benchmarks.common import request_keys, validate_keys


def as_hwc(frame):
    image = frame.image
    if hasattr(image, "detach"):
        if image.device.type != "cpu":
            raise ValueError("CPU benchmark received a non-CPU tensor")
        image = image.detach().numpy()
    image = np.asarray(image)
    if frame.layout == "CHW":
        image = image.transpose(1, 2, 0)
    elif frame.layout != "HWC":
        raise ValueError("unknown image layout")
    if image.ndim != 3 or image.shape[2] != 3 or image.dtype != np.uint8:
        raise ValueError(f"expected RGB uint8, got {image.shape} {image.dtype}")
    return np.ascontiguousarray(image)


def consume(adapter, job, validate=False):
    keys, records = [], []
    total_bytes = 0
    for frames in adapter.batches():
        # Conversion, order checks and materialization are in the timed path.
        images = [as_hwc(frame) for frame in frames]
        for frame, image in zip(frames, images, strict=True):
            keys.append(frame.key)
            total_bytes += image.nbytes
            if list(image.shape) != job["camera_shapes"][frame.key[2]]:
                raise ValueError("decoded resolution differs from the dataset manifest")
            if validate:
                records.append(
                    {
                        "key": frame.key,
                        "shape": list(image.shape),
                        "sha256": hashlib.sha256(image).hexdigest(),
                    }
                )
    validate_keys(
        keys, request_keys(job["requests"], job["config"]["dataset"]["cameras"])
    )
    return {"images": len(keys), "bytes": total_bytes, "records": records}
