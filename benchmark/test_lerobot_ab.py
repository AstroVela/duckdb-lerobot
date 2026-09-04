import hashlib
import io
import json
import sys
from contextlib import redirect_stderr, redirect_stdout
from copy import deepcopy
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from types import ModuleType
from unittest import TestCase
from unittest.mock import patch

from benchmark import lerobot_ab


DATASET_REVISION = "0123456789abcdef0123456789abcdef01234567"
OTHER_DATASET_REVISION = "fedcba9876543210fedcba9876543210fedcba98"


def benchmark_args() -> SimpleNamespace:
    return SimpleNamespace(
        all_episodes=False,
        batch_size=16,
        camera=["observation.image"],
        cache_state="warm-process",
        cluster_gap=10.0,
        codec_threads=1,
        dataset="/tmp/a'dataset",
        decode_threads=8,
        duckdb_cli=None,
        duckdb_load=[],
        engine="duckdb",
        episode=0,
        extension=None,
        height=0,
        lerobot_root=None,
        max_cached_decoders=8,
        max_output_bytes=64 * 1024 * 1024,
        max_pending_targets=4096,
        producer_threads=4,
        no_profile=True,
        output="",
        repeats=1,
        revision=DATASET_REVISION,
        rows=2,
        target_buffer_size=256,
        tolerance=1e-4,
        video_backend="pyav",
        warmups=0,
        width=0,
    )


def synthetic_result(engine: str) -> dict:
    cameras = ["camera.left", "camera.right"]
    selected_frames = [
        {"episode_index": 0, "frame_index": 0},
        {"episode_index": 0, "frame_index": 1},
    ]
    rows = []
    for frame in selected_frames:
        for camera in cameras:
            pixels = (
                f"{frame['episode_index']}:{frame['frame_index']}:{camera}".encode()
            )
            rows.append(
                {
                    **frame,
                    "video_key": camera,
                    "shape": [1, 1, 3],
                    "sha256": hashlib.sha256(pixels).hexdigest(),
                }
            )
    rows = lerobot_ab.canonical_rows(rows)
    machine = {
        "hostname": "benchmark-host",
        "platform": "Linux-test",
        "machine": "x86_64",
        "processor": "",
        "python": f"3.12-{engine}",
        "cpu_count": 16,
        "cpu_model": "Synthetic CPU",
        "physical_cpu_count": 8,
        "memory_total_bytes": 64 * 1024**3,
    }
    return {
        "schema_version": lerobot_ab.BENCHMARK_SCHEMA_VERSION,
        "engine": engine,
        "dataset": "/tmp/multicamera",
        "revision": DATASET_REVISION,
        "episode": 0,
        "all_episodes": False,
        "requested_frame_rows": 2,
        "selected_frames": selected_frames,
        "cameras": cameras,
        "selected_camera_count": 2,
        "available_cameras": ["camera.left", "camera.overhead", "camera.right"],
        "available_camera_count": 3,
        "decoded_images": 4,
        "timed_summary": {"frame_rows": 2, "decoded_images": 4},
        "cache_state": "warm-process",
        "decode_median_seconds": {"duckdb": 1.0, "daft": 2.0, "lerobot": 3.0}[engine],
        "validation_seconds": 0.1,
        "configuration": {
            "delta_timestamps": [0.0],
            "video_backend": "pyav",
            "tolerance": 1e-4,
            "width": 0,
            "height": 0,
        },
        "machine": machine,
        "hardware_identity": lerobot_ab.hardware_identity(machine),
        "rows": rows,
    }


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

    def test_camera_inventory_helpers_are_strict(self) -> None:
        cameras = lerobot_ab.daft_available_cameras(
            [
                "episode_index",
                "videos/camera.right/video",
                "videos/camera.left/from_timestamp",
                "videos/camera.left/video",
            ]
        )
        self.assertEqual(cameras, ["camera.left", "camera.right"])

        args = benchmark_args()
        query = lerobot_ab.duckdb_available_cameras_query(
            args,
            [
                {"episode_index": 2, "frame_index": 1},
                {"episode_index": 1, "frame_index": 9},
                {"episode_index": 2, "frame_index": 2},
            ],
        )
        self.assertIn("[1, 2]", query)
        self.assertIn("ORDER BY video_key", query)

        with self.assertRaisesRegex(ValueError, "duplicate --camera"):
            lerobot_ab.validate_requested_cameras(["camera.left", "camera.left"])
        with self.assertRaisesRegex(RuntimeError, "available camera keys"):
            lerobot_ab.validate_camera_inventory("lerobot", ["camera.missing"], cameras)
        with self.assertRaisesRegex(ValueError, "40-character commit SHA"):
            lerobot_ab.validate_dataset_revision("main")
        with self.assertRaisesRegex(RuntimeError, "returned video keys"):
            lerobot_ab.validate_lerobot_items(
                [{"camera.left": b"left", "camera.right": b"right"}],
                ["camera.left"],
                ["camera.left", "camera.right"],
            )

    def test_duckdb_setup_closes_connections_on_camera_validation_error(self) -> None:
        args = benchmark_args()

        class FakePythonConnection:
            def __init__(self):
                self.last_sql = ""
                self.closed = False

            def execute(self, sql):
                self.last_sql = sql
                return self

            def fetchone(self):
                self.assert_query("PRAGMA version")
                return ("v1.5.0", "source-id")

            def fetchall(self):
                if "FROM lerobot_frames" in self.last_sql:
                    return [(0, 0), (0, 1)]
                if "SELECT DISTINCT video_key" in self.last_sql:
                    return [("camera.other",)]
                return []

            def assert_query(self, expected):
                if self.last_sql != expected:
                    raise AssertionError(
                        f"expected query {expected!r}, received {self.last_sql!r}"
                    )

            def close(self):
                self.closed = True

        python_connection = FakePythonConnection()
        duckdb_module = ModuleType("duckdb")
        duckdb_module.__version__ = "1.5.0"
        duckdb_module.connect = lambda: python_connection
        with patch.dict(sys.modules, {"duckdb": duckdb_module}):
            with self.assertRaisesRegex(RuntimeError, "available camera keys"):
                lerobot_ab.run_duckdb_python(args)
        self.assertTrue(python_connection.closed)

        class FakeCLIConnection:
            def __init__(self):
                self.executable = "/tmp/duckdb"
                self.closed = False

            def execute(self, sql):
                if sql == "PRAGMA version":
                    return [{"library_version": "v1.5.0", "source_id": "source-id"}]
                if "FROM lerobot_frames" in sql:
                    return [
                        {"episode_index": 0, "frame_index": 0},
                        {"episode_index": 0, "frame_index": 1},
                    ]
                if "SELECT DISTINCT video_key" in sql:
                    return [{"video_key": "camera.other"}]
                return []

            def close(self):
                self.closed = True

        args.duckdb_cli = "/tmp/duckdb"
        cli_connection = FakeCLIConnection()
        with patch.object(
            lerobot_ab, "DuckDBCLIConnection", return_value=cli_connection
        ):
            with self.assertRaisesRegex(RuntimeError, "available camera keys"):
                lerobot_ab.run_duckdb_cli(args)
        self.assertTrue(cli_connection.closed)

    def test_native_lerobot_decodes_only_requested_cameras(self) -> None:
        decode_calls: list[tuple[str, ...]] = []

        class Scalar:
            def __init__(self, value: int):
                self.value = value

            def item(self) -> int:
                return self.value

        class FakeReader:
            def _query_videos(self, query_timestamps, episode_index):
                del episode_index
                decode_calls.append(tuple(query_timestamps))
                return {
                    camera: f"{camera}:{timestamps[0]}".encode()
                    for camera, timestamps in query_timestamps.items()
                }

        class FakeDataset:
            def __init__(self, repo_id, **kwargs):
                del repo_id, kwargs
                self.meta = SimpleNamespace(
                    video_keys=["camera.left", "camera.overhead", "camera.right"]
                )
                self.reader = FakeReader()

            def __len__(self):
                return 2

            def __getitems__(self, indices):
                items = []
                for index in indices:
                    timestamps = {
                        camera: [index / 30.0] for camera in self.meta.video_keys
                    }
                    items.append(
                        {
                            "episode_index": Scalar(0),
                            "frame_index": Scalar(index),
                            **self.reader._query_videos(timestamps, 0),
                        }
                    )
                return items

        def make_fake_row(episode_index, frame_index, video_key, image):
            return {
                "episode_index": episode_index,
                "frame_index": frame_index,
                "video_key": video_key,
                "shape": [1, 1, 3],
                "sha256": hashlib.sha256(image).hexdigest(),
            }

        lerobot_module = ModuleType("lerobot")
        datasets_module = ModuleType("lerobot.datasets")
        dataset_module = ModuleType("lerobot.datasets.lerobot_dataset")
        dataset_module.LeRobotDataset = FakeDataset
        lerobot_module.datasets = datasets_module
        datasets_module.lerobot_dataset = dataset_module

        args = benchmark_args()
        args.camera = ["camera.right", "camera.left"]
        with (
            patch.dict(
                sys.modules,
                {
                    "lerobot": lerobot_module,
                    "lerobot.datasets": datasets_module,
                    "lerobot.datasets.lerobot_dataset": dataset_module,
                },
            ),
            patch.object(lerobot_ab, "make_row", side_effect=make_fake_row),
        ):
            _, execute, validate, extra = lerobot_ab.run_lerobot(args)
            summary = execute()
            rows = validate()

        self.assertEqual(summary, {"frame_rows": 2, "decoded_images": 4})
        self.assertEqual(len(rows), 4)
        self.assertTrue(all(len(row["sha256"]) == 64 for row in rows))
        self.assertEqual(
            extra["available_cameras"],
            ["camera.left", "camera.overhead", "camera.right"],
        )
        self.assertEqual(len(decode_calls), 4)
        self.assertTrue(
            all(call == ("camera.right", "camera.left") for call in decode_calls)
        )

    def test_multicamera_results_compare_counts_and_hashes(self) -> None:
        source = synthetic_result("duckdb")
        adapter_names = {
            "duckdb": "run_duckdb",
            "daft": "run_daft",
            "lerobot": "run_lerobot",
        }
        with TemporaryDirectory() as directory:
            paths = []
            results = []
            for engine, adapter_name in adapter_names.items():
                args = benchmark_args()
                args.engine = engine
                args.camera = list(source["cameras"])
                path = Path(directory) / f"{engine}.json"
                args.output = str(path)

                def fake_adapter(_args):
                    return (
                        0.25,
                        lambda: {"frame_rows": 2, "decoded_images": 4},
                        lambda: deepcopy(source["rows"]),
                        {"available_cameras": source["available_cameras"]},
                    )

                with (
                    patch.object(lerobot_ab, adapter_name, side_effect=fake_adapter),
                    patch.object(
                        lerobot_ab, "machine_info", return_value=source["machine"]
                    ),
                    redirect_stdout(io.StringIO()),
                ):
                    self.assertEqual(lerobot_ab.run_command(args), 0)

                paths.append(str(path))
                results.append(json.loads(path.read_text()))

            self.assertTrue(all(result["decoded_images"] == 4 for result in results))
            expected_hashes = [row["sha256"] for row in source["rows"]]
            self.assertTrue(
                all(
                    [row["sha256"] for row in result["rows"]] == expected_hashes
                    for result in results
                )
            )
            with redirect_stdout(io.StringIO()):
                self.assertEqual(
                    lerobot_ab.compare_command(SimpleNamespace(results=paths)), 0
                )

    def test_run_command_records_strict_multicamera_contract(self) -> None:
        source = synthetic_result("duckdb")
        machine = source["machine"]

        def fake_adapter(args):
            del args
            return (
                0.25,
                lambda: {"frame_rows": 2, "decoded_images": 4},
                lambda: deepcopy(source["rows"]),
                {"available_cameras": source["available_cameras"]},
            )

        args = benchmark_args()
        args.camera = list(source["cameras"])
        with TemporaryDirectory() as directory:
            args.output = str(Path(directory) / "duckdb.json")
            with (
                patch.object(lerobot_ab, "run_duckdb", side_effect=fake_adapter),
                patch.object(lerobot_ab, "machine_info", return_value=machine),
                redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(lerobot_ab.run_command(args), 0)
            result = json.loads(Path(args.output).read_text())

        self.assertEqual(result["schema_version"], lerobot_ab.BENCHMARK_SCHEMA_VERSION)
        self.assertEqual(result["selected_camera_count"], 2)
        self.assertEqual(result["available_camera_count"], 3)
        self.assertEqual(result["selected_frames"], source["selected_frames"])
        self.assertEqual(result["configuration"]["delta_timestamps"], [0.0])
        self.assertEqual(result["configuration"]["video_backend"], "pyav")
        self.assertEqual(
            result["hardware_identity"], lerobot_ab.hardware_identity(machine)
        )
        self.assertNotIn("median_seconds", result)

    def test_compare_rejects_incompatible_workloads(self) -> None:
        baseline = synthetic_result("duckdb")

        def different_rows(result):
            for frame in result["selected_frames"]:
                frame["frame_index"] += 10
            for row in result["rows"]:
                row["frame_index"] += 10

        def different_hardware(result):
            result["machine"]["cpu_model"] = "Different CPU"
            result["hardware_identity"] = lerobot_ab.hardware_identity(
                result["machine"]
            )

        def different_available_cameras(result):
            result["available_cameras"].append("camera.wrist")
            result["available_camera_count"] = 4

        changes = {
            "revision": lambda result: result.update(revision=OTHER_DATASET_REVISION),
            "cameras": lambda result: result["cameras"].reverse(),
            "selected_frames": different_rows,
            "delta_timestamps": lambda result: result["configuration"].update(
                delta_timestamps=[-0.1, 0.0]
            ),
            "video_backend": lambda result: result["configuration"].update(
                video_backend="torchcodec"
            ),
            "available_cameras": different_available_cameras,
            "hardware_identity": different_hardware,
        }

        with TemporaryDirectory() as directory:
            baseline_path = Path(directory) / "baseline.json"
            baseline_path.write_text(json.dumps(baseline))
            for field, change in changes.items():
                with self.subTest(field=field):
                    candidate = deepcopy(synthetic_result("lerobot"))
                    change(candidate)
                    candidate_path = Path(directory) / f"{field}.json"
                    candidate_path.write_text(json.dumps(candidate))
                    stderr = io.StringIO()
                    with redirect_stderr(stderr):
                        status = lerobot_ab.compare_command(
                            SimpleNamespace(
                                results=[str(baseline_path), str(candidate_path)]
                            )
                        )
                    self.assertEqual(status, 2)
                    self.assertIn("INCOMPARABLE", stderr.getvalue())

    def test_compare_reports_pixel_mismatch_after_contract_validation(self) -> None:
        baseline = synthetic_result("duckdb")
        candidate = synthetic_result("lerobot")
        candidate["rows"][0]["sha256"] = "0" * 64

        with TemporaryDirectory() as directory:
            baseline_path = Path(directory) / "baseline.json"
            candidate_path = Path(directory) / "candidate.json"
            baseline_path.write_text(json.dumps(baseline))
            candidate_path.write_text(json.dumps(candidate))
            stderr = io.StringIO()
            with redirect_stderr(stderr), redirect_stdout(io.StringIO()):
                status = lerobot_ab.compare_command(
                    SimpleNamespace(results=[str(baseline_path), str(candidate_path)])
                )
        self.assertEqual(status, 1)
        self.assertIn("PIXEL MISMATCH", stderr.getvalue())

    def test_compare_rejects_incomplete_timed_workload(self) -> None:
        baseline = synthetic_result("duckdb")
        candidate = synthetic_result("lerobot")
        candidate["timed_summary"]["decoded_images"] = 2

        with TemporaryDirectory() as directory:
            baseline_path = Path(directory) / "baseline.json"
            candidate_path = Path(directory) / "candidate.json"
            baseline_path.write_text(json.dumps(baseline))
            candidate_path.write_text(json.dumps(candidate))
            stderr = io.StringIO()
            with redirect_stderr(stderr):
                status = lerobot_ab.compare_command(
                    SimpleNamespace(results=[str(baseline_path), str(candidate_path)])
                )

        self.assertEqual(status, 2)
        self.assertIn("timed_summary decoded_images", stderr.getvalue())

    def test_compare_requires_at_least_two_results(self) -> None:
        stderr = io.StringIO()
        with redirect_stderr(stderr):
            status = lerobot_ab.compare_command(SimpleNamespace(results=["one.json"]))
        self.assertEqual(status, 2)
        self.assertIn("at least two", stderr.getvalue())
