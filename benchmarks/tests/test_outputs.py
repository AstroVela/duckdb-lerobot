"""Exercise output normalization without silently rounding or changing layout."""

import unittest
import numpy as np
import torch

from benchmarks.adapters.base import Frame
from benchmarks.workloads.video_read import as_hwc
from benchmarks.workloads.tensor_batch import as_chw


class OutputContractTest(unittest.TestCase):
    def test_square_rgb_image_uses_declared_layout(self):
        # All dimensions are 3: shape heuristics cannot distinguish CHW from HWC.
        hwc = np.arange(27, dtype=np.uint8).reshape(3, 3, 3)
        chw = torch.from_numpy(hwc.copy()).permute(2, 0, 1)
        frame = Frame([0, 0, "front"], chw, "CHW")
        actual = as_hwc(frame)
        np.testing.assert_array_equal(actual, hwc)
        self.assertTrue(actual.flags.c_contiguous)
        np.testing.assert_array_equal(
            as_chw(Frame(frame.key, hwc, "HWC")).numpy(), chw.numpy()
        )

    def test_float_output_is_rejected_instead_of_requantized(self):
        frame = Frame([0, 0, "front"], np.zeros((4, 4, 3), dtype=np.float32), "HWC")
        with self.assertRaises(ValueError):
            as_hwc(frame)
        with self.assertRaises(ValueError):
            as_chw(frame)

    def test_readonly_blob_is_safe_for_torch(self):
        pixels = bytes(range(12))
        frame = Frame(
            [0, 0, "front"],
            np.frombuffer(pixels, dtype=np.uint8).reshape(2, 2, 3),
            "HWC",
        )
        tensor = as_chw(frame)
        tensor[0, 0, 0] = 255
        self.assertEqual(frame.image[0, 0, 0], 0)


if __name__ == "__main__":
    unittest.main()
