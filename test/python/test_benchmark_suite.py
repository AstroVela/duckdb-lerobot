"""Benchmark integrity regressions; no optional engine dependencies needed."""

import copy
import json
import subprocess
import sys
import tempfile
import unittest
import venv
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from benchmarks.common import digest, extension_info, load_config, validate_keys, verify_packages, write_json
from benchmarks.report import summarize, validate
from benchmarks.run import plan_requests, worker_python


class BenchmarkSuiteTest(unittest.TestCase):
    def test_python_override_preserves_virtual_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            environment = Path(directory) / "venv"
            venv.EnvBuilder(with_pip=False, symlinks=True).create(environment)
            python = worker_python(Path(directory), "unused", environment / "bin/python")
            prefix = subprocess.check_output(
                [str(python), "-I", "-c", "import sys; print(sys.prefix)"], text=True
            ).strip()
            self.assertEqual(Path(prefix).resolve(), environment.resolve())

    def test_rejects_stale_environment_and_wrong_binary_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lock = root / "requirements.txt"
            lock.write_text("numpy==2.2.6\nPillow==12.3.0\n")
            verify_packages({"numpy": "2.2.6", "pillow": "12.3.0"}, lock)
            with self.assertRaisesRegex(ValueError, "expected numpy"):
                verify_packages({"numpy": "2.5.3", "pillow": "12.3.0"}, lock)
            binary = root / "extension"
            binary.write_bytes(b"binary from another build")
            manifest = root / "manifest.json"
            write_json(manifest, {"extension_sha256": "0" * 64})
            with self.assertRaisesRegex(ValueError, "does not describe this extension"):
                extension_info(binary, manifest_path=manifest)

    def test_request_order_is_an_explicit_contract(self):
        frames = [{"index": i, "episode_index": i // 4, "frame_index": i % 4} for i in range(12)]
        case = {"name": "random", "rows": 8, "order": "random"}
        selected = plan_requests(frames, case, 1729)
        self.assertEqual(selected, plan_requests(frames, case, 1729))
        self.assertNotEqual(selected, frames[:8])
        self.assertEqual(len({r["index"] for r in selected}), 8)
        with self.assertRaisesRegex(ValueError, "keys/order"):
            validate_keys([[0, 1, "front"], [0, 0, "front"]], [[0, 0, "front"], [0, 1, "front"]])
        with self.assertRaisesRegex(ValueError, "more rows"):
            plan_requests(frames, {**case, "rows": 13}, 1729)

    def test_checked_in_configs_are_valid(self):
        configs = Path(__file__).resolve().parents[2] / "benchmarks/configs"
        for path in configs.glob("*.toml"):
            with self.subTest(path=path):
                self.assertTrue(load_config(path)["cases"])

    def test_statistics_reject_unusable_samples(self):
        self.assertEqual(summarize([1, 2, 3, 4, 5]), {"median": 3, "iqr": 2})
        for samples in ([], [float("nan")], [float("inf")], [0], [-1]):
            with self.subTest(samples=samples), self.assertRaises(ValueError):
                summarize(samples)

    def test_report_rejects_incomplete_or_incomparable_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            requests = [{"index": 0, "episode_index": 0, "frame_index": 0}]
            evidence = {
                "images": 1,
                "bytes": 12,
                "records": [{"key": [0, 0, "front"], "shape": [2, 2, 3], "sha256": "a" * 64}],
            }
            for engine in ["duckdb", "daft"]:
                write_json(root / (engine + ".json"), evidence)
            config = {
                "engines": ["duckdb", "daft"],
                "repeats": 2,
                "dataset": {"cameras": ["front"]},
                "cases": [{"name": "case", "rows": 1}],
            }
            engine_result = {
                "warm_seconds": [1.0, 2.0],
                "environment": {"packages": {}, "machine": {}},
                "counters": {"images": 1, "bytes": 12},
            }
            result = {
                "schema_version": 1,
                "status": "complete",
                "config": config,
                "config_sha256": digest(config),
                "dataset": {},
                "dataset_manifest_sha256": digest({}),
                "machine": {},
                "cases": [
                    {
                        "name": "case",
                        "pixels_equal": True,
                        "requests": requests,
                        "requests_sha256": digest(requests),
                        "engines": {
                            engine: {
                                **engine_result,
                                "validation": {
                                    "file": engine + ".json",
                                    "sha256": digest(evidence),
                                },
                            }
                            for engine in config["engines"]
                        },
                    }
                ],
            }
            validate(result, root)
            incomplete = copy.deepcopy(result)
            incomplete["status"] = "failed"
            with self.assertRaisesRegex(ValueError, "complete"):
                validate(incomplete, root)
            missing = copy.deepcopy(result)
            del missing["cases"][0]["engines"]["daft"]
            with self.assertRaisesRegex(ValueError, "all configured engines"):
                validate(missing, root)
            # A self-consistent candidate digest still cannot conceal changed pixels.
            different = copy.deepcopy(result)
            changed = json.loads(json.dumps(evidence))
            changed["records"][0]["sha256"] = "b" * 64
            write_json(root / "daft.json", changed)
            different["cases"][0]["engines"]["daft"]["validation"]["sha256"] = digest(changed)
            with self.assertRaisesRegex(ValueError, "pixel digests differ"):
                validate(different, root)
            with self.assertRaisesRegex(ValueError, "validation digest"):
                validate(result, root)


if __name__ == "__main__":
    unittest.main()
