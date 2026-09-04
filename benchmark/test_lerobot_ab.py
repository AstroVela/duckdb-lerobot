from types import SimpleNamespace
from unittest import TestCase

from benchmark import lerobot_ab


def benchmark_args() -> SimpleNamespace:
    return SimpleNamespace(
        batch_size=16,
        camera=["observation.image"],
        cluster_gap=10.0,
        codec_threads=1,
        dataset="/tmp/a'dataset",
        decode_threads=8,
        height=0,
        max_cached_decoders=8,
        max_output_bytes=64 * 1024 * 1024,
        max_pending_targets=4096,
        producer_threads=4,
        target_buffer_size=256,
        tolerance=1e-4,
        width=0,
    )


class LeRobotBenchmarkTest(TestCase):
    def test_timing_uses_small_materialization_summary(self) -> None:
        calls = 0

        def execute() -> dict[str, int]:
            nonlocal calls
            calls += 1
            return {"frame_rows": 2, "decoded_images": 2}

        durations, summary = lerobot_ab.time_callable(execute, warmups=1, repeats=2)

        self.assertEqual(calls, 3)
        self.assertEqual(len(durations), 2)
        self.assertEqual(summary, {"frame_rows": 2, "decoded_images": 2})

    def test_duckdb_timing_consumes_blobs_without_hash_or_output_sort(self) -> None:
        args = benchmark_args()
        selected = [
            {"episode_index": 12, "frame_index": 45},
            {"episode_index": 12, "frame_index": 46},
        ]

        decode = lerobot_ab.duckdb_decode_query(
            args, selected, hash_images=False, order_output=False
        )
        timed = lerobot_ab.duckdb_timed_query(args, selected)
        validation = lerobot_ab.duckdb_decode_query(
            args, selected, hash_images=True, order_output=True
        )
        source_profiles = lerobot_ab.source_profile_queries(args, selected)

        self.assertIn("VALUES (12, 45), (12, 46)", timed)
        self.assertIn("octet_length(image)", timed)
        self.assertNotIn("sha256(image)", timed)
        self.assertFalse(
            decode.strip().endswith("ORDER BY episode_index, frame_index, video_key")
        )
        self.assertIn("sha256(image) AS sha256", validation)
        self.assertTrue(
            validation.strip().endswith(
                "ORDER BY episode_index, frame_index, video_key"
            )
        )
        self.assertIn("'/tmp/a''dataset'", validation)
        self.assertNotIn("producer_threads", timed)
        self.assertIn("producer_threads := 4", source_profiles[0][1])
