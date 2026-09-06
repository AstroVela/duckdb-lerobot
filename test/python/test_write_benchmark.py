"""Regressions for whole-process video benchmark accounting; no DuckDB needed."""

import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "lerobot_copy_write",
    Path(__file__).resolve().parents[2] / "benchmark/lerobot_copy_write.py",
)
benchmark = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(benchmark)


class WriteBenchmarkTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="lerobot-measurement-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.cli = self.directory / "fixture-cli"

    def script(self, body):
        self.cli.write_text(f"#!{sys.executable}\n" + body)
        self.cli.chmod(0o700)

    @unittest.skipUnless(
        sys.platform == "linux" and hasattr(os, "wait4"), "Linux process accounting"
    )
    def test_includes_last_write_before_immediate_exit(self):
        self.script(
            "import os, sys\n"
            "path = sys.stdin.read()\n"
            "with open(path, 'wb', buffering=0) as output:\n"
            "    output.write(b'x' * (8 * 1024 * 1024))\n"
            "    os.fsync(output.fileno())\n"
            "os._exit(0)\n"
        )
        destination = self.directory / "last-write"
        elapsed, counters = benchmark.run_sql_measured(self.cli, str(destination))
        self.assertGreater(elapsed, 0)
        self.assertEqual(destination.stat().st_size, 8 * 1024 * 1024)
        self.assertIsNotNone(counters["fs_output_blocks"])
        if counters["fs_output_blocks"] == 0:
            self.skipTest("filesystem does not account output blocks (e.g. tmpfs)")
        # Linux reports 512-byte block units. A polling sample can see only a
        # prefix of this final write, whereas wait4 includes the full lifetime.
        self.assertGreaterEqual(counters["fs_output_blocks"], 8 * 1024 * 1024 // 512)

    @unittest.skipUnless(
        sys.platform == "linux" and hasattr(os, "wait4"), "Linux peak RSS accounting"
    )
    def test_peak_rss_is_per_child_not_previous_child_maximum(self):
        self.script(
            "import sys\n"
            "allocation = bytearray(int(sys.stdin.read()))\n"
            "for i in range(0, len(allocation), 4096): allocation[i] = 1\n"
        )
        _, large = benchmark.run_sql_measured(self.cli, str(64 * 1024 * 1024))
        _, small = benchmark.run_sql_measured(self.cli, "1")
        self.assertGreater(
            large["peak_rss_bytes"] - small["peak_rss_bytes"], 32 * 1024 * 1024
        )

    @unittest.skipUnless(os.name == "posix", "executable fixture uses a POSIX shebang")
    def test_nonzero_exit_preserves_diagnostic_and_can_retry(self):
        self.script("import sys\nprint('fixture close failed')\nsys.exit(7)\n")
        with self.assertRaisesRegex(
            RuntimeError, "(?s)exited with 7.*fixture close failed"
        ):
            benchmark.run_sql_measured(self.cli, "")
        self.script("pass\n")
        benchmark.run_sql_measured(self.cli, "")

    @unittest.skipUnless(
        os.name == "posix" and hasattr(os, "wait4"), "POSIX child interruption"
    )
    def test_interrupted_wait_reaps_child_before_raising(self):
        self.script("import time\ntime.sleep(30)\n")
        child_pid = None

        def interrupt_wait(pid, _options):
            nonlocal child_pid
            child_pid = pid
            raise KeyboardInterrupt

        with patch.object(benchmark.os, "wait4", side_effect=interrupt_wait):
            with self.assertRaises(KeyboardInterrupt):
                benchmark.run_sql_measured(self.cli, "")
        self.assertIsNotNone(child_pid)
        with self.assertRaises(ChildProcessError):
            os.waitpid(child_pid, os.WNOHANG)

    @unittest.skipUnless(os.name == "posix", "executable fixture uses a POSIX shebang")
    def test_missing_wait4_keeps_metrics_unknown(self):
        self.script("pass\n")

        class NoWait4:
            pass

        with patch.object(benchmark, "os", NoWait4()):
            elapsed, counters = benchmark.run_sql_measured(self.cli, "")
        self.assertGreater(elapsed, 0)
        self.assertTrue(all(value is None for value in counters.values()))

    def test_ratios_do_not_convert_missing_samples_to_zero(self):
        def sample(reads, writes):
            return {
                "process_io": {"fs_input_blocks": reads, "fs_output_blocks": writes}
            }

        result = benchmark.io_scaling([sample(0, 2), sample(0, 4)], sample(0, 6))
        self.assertEqual(result["small_fs_input_blocks_median"], 0)
        self.assertIsNone(result["fs_input_blocks_ratio"])
        self.assertEqual(result["fs_output_blocks_ratio"], 2)
        result = benchmark.io_scaling([sample(None, 2), sample(2, 4)], sample(4, None))
        self.assertIsNone(result["small_fs_input_blocks_median"])
        self.assertIsNone(result["fs_input_blocks_ratio"])
        self.assertIsNone(result["large_fs_output_blocks"])
        self.assertIsNone(result["fs_output_blocks_ratio"])


if __name__ == "__main__":
    unittest.main()
