#!/usr/bin/env python3
"""Run against the installed, extension-bearing Vane wheel; no runtime downloads."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
import time
import unittest


def quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def black_frame_digest(width=16, height=16):
    # The committed long-20701.mp4 fixture contains uniformly black frames.
    # Derive the expected RGB24 bytes directly, independently of either runner.
    return hashlib.md5(bytes(width * height * 3)).hexdigest()


def fixture(root):
    import pyarrow as pa
    import pyarrow.parquet as pq

    (root / "meta/episodes").mkdir(parents=True)
    (root / "data").mkdir()
    (root / "videos").mkdir()
    source = Path(__file__).resolve().parents[1] / "data/lerobot/long-20701.mp4"
    episodes = [0, 2, 5, 9]  # Real datasets need not have contiguous episode IDs.
    routes, frames = [], []
    for index, episode in enumerate(episodes):
        shutil.copyfile(source, root / f"videos/{index}.mp4")
        routes.append(
            {
                "episode_index": episode,
                "length": 4,
                "dataset_from_index": index * 4,
                "dataset_to_index": (index + 1) * 4,
                "data/chunk_index": 0,
                "data/file_index": index // 2,
                "videos/camera/chunk_index": 0,
                "videos/camera/file_index": index,
                "videos/camera/from_timestamp": 0.0,
                "videos/camera/to_timestamp": 4 / 30,
            }
        )
        for frame in range(4):
            frames.append(
                {
                    "episode_index": episode,
                    "frame_index": frame,
                    "index": index * 4 + frame,
                    "timestamp": frame / 30,
                    "task_index": 0,
                    "action": float(index * 4 + frame),
                }
            )
    pq.write_table(pa.Table.from_pylist(routes), root / "meta/episodes/0.parquet")
    for shard in range(2):
        pq.write_table(
            pa.Table.from_pylist(frames[shard * 8 : (shard + 1) * 8]),
            root / f"data/{shard}.parquet",
        )
    pq.write_table(pa.table({"task_index": [0], "task": ["pick"]}), root / "meta/tasks.parquet")
    (root / "meta/info.json").write_text(
        json.dumps(
            {
                "codebase_version": "v3.0",
                "fps": 30,
                "total_episodes": 4,
                "total_frames": 16,
                "total_tasks": 1,
                "data_path": "data/{file_index}.parquet",
                "video_path": "videos/{file_index}.mp4",
                "features": {
                    "action": {"dtype": "float32", "shape": [1]},
                    "camera": {"dtype": "video", "shape": [16, 16, 3]},
                },
            }
        )
    )
    (root / "meta/stats.json").write_text(json.dumps({"action": {"count": [16], "min": [0], "max": [15]}}))


class WorkerBarrier:
    directory = ""

    def __call__(self, table):
        import pyarrow as pa
        import ray

        node = str(ray.get_runtime_context().get_node_id())
        directory = Path(self.directory)
        (directory / (node + ".ready")).touch()
        deadline = time.monotonic() + 45
        while len(list(directory.glob("*.ready"))) < 2:
            if time.monotonic() > deadline:
                raise AssertionError("both Ray workers must consume video input")
            time.sleep(0.05)
        return pa.table(
            {
                "episode_index": table.column("episode_index"),
                "digest": table.column("digest"),
                "node": [node] * table.num_rows,
            }
        )


class LeRobotRuntime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import vane

        cls.vane = vane
        cls.distributed = os.environ.get("VANE_RUNNER") == "ray"
        cls.cluster = None
        cls.runner = None
        cls.reads = cls.writes = 0
        if cls.distributed:
            import ray
            from ray.cluster_utils import Cluster

            os.environ["VANE_FTE_DYNAMIC_SCAN_MAX_SPLITS_PER_PARTITION"] = "1"
            cls.cluster = Cluster(shutdown_at_exit=False)
            cls.addClassCleanup(cls.cluster.shutdown)
            cls.cluster.add_node(num_cpus=0, include_dashboard=False, object_store_memory=1024**3)
            for _ in range(2):
                cls.cluster.add_node(
                    num_cpus=1,
                    include_dashboard=False,
                    object_store_memory=1024**3,
                )
            ray.init(address=cls.cluster.address, log_to_driver=True)
            cls.addClassCleanup(ray.shutdown)
            vane.set_runner_ray(noop_if_initialized=True)
            cls.addClassCleanup(vane.teardown_runner)
            cls.runner = vane.get_or_create_runner()
            assert cls.runner.name == "ray"
            read, write = cls.runner.run_iter_tables, cls.runner.run_write

            def dispatch_read(plan):
                assert isinstance(plan, vane.ray_cxx.PyLogicalPlan)
                cls.reads += 1
                return read(plan)

            def dispatch_write(plan):
                assert isinstance(plan, vane.ray_cxx.PyLogicalPlan)
                cls.writes += 1
                return write(plan)

            cls.runner.run_iter_tables = dispatch_read
            cls.runner.run_write = dispatch_write
        else:
            assert os.environ.get("VANE_RUNNER") == "local-fast"
        config = {
            "threads": "2",
            "autoinstall_known_extensions": "false",
            "autoload_known_extensions": "false",
            "allow_unsigned_extensions": "true",
        }
        cls.connection = vane.connect(config=config)
        cls.addClassCleanup(cls.connection.close)
        assert cls.connection.sql("SELECT 1")._get_runner_type() == os.environ["VANE_RUNNER"]
        # Only local developer smoke tests may load an external build artifact.
        extension = os.environ.get("LEROBOT_VANE_TEST_EXTENSION")
        if extension:
            assert not cls.distributed
            load = f"LOAD {quote(Path(extension).resolve())}"
        else:
            load = "LOAD lerobot"
        cls.connection.execute(load)
        cls.temporary = tempfile.TemporaryDirectory(prefix="vane-lerobot-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.workspace = Path(cls.temporary.name)
        cls.root = cls.workspace / "source"
        fixture(cls.root)
        cls.path = quote(cls.root)

    def query(self, sql, expected):
        before = self.reads
        actual = self.connection.sql(sql).fetchall()
        self.assertEqual(actual, expected)
        if self.distributed:
            self.assertEqual(self.reads, before + 1, "query bypassed Ray")
        return actual

    def test_scan_metadata_and_join(self):
        p = self.path
        self.query(f"SELECT count(*), sum(action) FROM lerobot_scan({p})", [(16, 120.0)])
        self.query(
            f"SELECT episode_index, frame_index, action FROM lerobot_scan({p}, episode_indices := [2,9]) "
            "WHERE frame_index >= 2 ORDER BY episode_index, frame_index",
            [(2, 2, 6.0), (2, 3, 7.0), (9, 2, 14.0), (9, 3, 15.0)],
        )
        self.query(f"SELECT total_episodes, total_frames FROM lerobot_info({p})", [(4, 16)])
        self.query(
            f"SELECT episode_index, length FROM lerobot_episodes({p}) ORDER BY episode_index",
            [(0, 4), (2, 4), (5, 4), (9, 4)],
        )
        self.query(f"SELECT task_index, task FROM lerobot_tasks({p})", [(0, "pick")])
        self.query(
            f"SELECT feature, stats->>'$.count[0]' FROM lerobot_stats({p})",
            [("action", "16")],
        )
        self.query(
            f"SELECT count(*) FROM lerobot_scan({p}) JOIN lerobot_tasks({p}) USING (task_index)",
            [(16,)],
        )
        self.query(
            f"SELECT * FROM lerobot_video_routes({p}, [0,2,5,9]) ORDER BY episode_index, video_key",
            [
                (
                    episode,
                    "camera",
                    str(self.root / f"videos/{index}.mp4"),
                    0,
                    index,
                    0.0,
                    4 / 30,
                    30,
                )
                for index, episode in enumerate((0, 2, 5, 9))
            ],
        )
        self.query(
            f"SELECT root, component, cached, entries, bytes > 0 FROM lerobot_cache_info({p}) ORDER BY component",
            [(str(self.root), component, True, 1, True) for component in ("data", "video")],
        )

    def test_frames_windows_and_empty_splits(self):
        p = self.path
        frames = f"lerobot_video_frames({p}, [0,2,5,9], width := 8, height := 8, frame_indices := [0,3])"
        rows = self.query(
            f"SELECT episode_index, frame_index, width, height, md5(image) FROM {frames} "
            "ORDER BY episode_index, frame_index",
            [(episode, frame, 8, 8, black_frame_digest(8, 8)) for episode in (0, 2, 5, 9) for frame in (0, 3)],
        )
        self.assertEqual(len(rows), 8)
        self.query(f"SELECT count(*) FROM lerobot_video_frames({p}, []::BIGINT[])", [(0,)])
        self.query(
            f"SELECT count(*) FROM lerobot_video_frames({p}, [0], frame_indices := []::BIGINT[])",
            [(0,)],
        )
        requests = (
            "[{'request_id':7,'episode_index':9,'frame_index':3}," "{'request_id':7,'episode_index':0,'frame_index':0}]"
        )
        self.query(
            f"SELECT request_id, request_ordinal, delta_ordinal, episode_index, target_frame_index, "
            f"is_padding, md5(image) FROM lerobot_video_windows({p}, {requests}, "
            "delta_timestamps := [-0.03333333333333333, 0.0, 0.03333333333333333]) "
            "ORDER BY request_ordinal, delta_ordinal",
            [
                (7, 0, 0, 9, 2, False, black_frame_digest()),
                (7, 0, 1, 9, 3, False, black_frame_digest()),
                (7, 0, 2, 9, 3, True, black_frame_digest()),
                (7, 1, 0, 0, 0, True, black_frame_digest()),
                (7, 1, 1, 0, 0, False, black_frame_digest()),
                (7, 1, 2, 0, 1, False, black_frame_digest()),
            ],
        )
        relation = self.connection.sql(f"SELECT * FROM {frames}")
        plan = self.vane.ray_cxx.PyLogicalPlan.from_duckdb_relation(relation, "lerobot-splits").to_physical_plan(
            self.connection
        )
        self.assertGreaterEqual(sum(map(len, plan.scan_split_batch_map().values())), 4)

    def test_targets_have_global_ordinals(self):
        p = self.path
        requests = (
            f"(SELECT index AS target_id, 7 AS request_id, episode_index, frame_index, "
            f"0 AS delta_index FROM lerobot_scan({p}))"
        )
        temporal = f"lerobot_temporal_targets({p}, {requests})"
        self.query(
            f"SELECT target_id, target_frame_index, is_padding FROM {temporal} ORDER BY target_id",
            [(index, index % 4, False) for index in range(16)],
        )
        self.query(
            f"SELECT count(*), count(DISTINCT target_ordinal), min(target_ordinal), max(target_ordinal) "
            f"FROM {temporal}",
            [(16, 16, 0, 15)],
        )
        video_requests = requests.replace("0 AS delta_index", "'camera' AS video_key, 0 AS delta_index")
        video = f"lerobot_video_targets({p}, {video_requests})"
        self.query(
            f"SELECT target_id, target_frame_index, md5(image) FROM {video} ORDER BY target_id",
            [(index, index % 4, black_frame_digest()) for index in range(16)],
        )
        self.query(
            f"SELECT count(*), count(DISTINCT target_ordinal), min(target_ordinal), max(target_ordinal) "
            f"FROM {video}",
            [(16, 16, 0, 15)],
        )

    def test_bound_snapshot_survives_metadata_removal(self):
        sql = (
            f"SELECT episode_index, frame_index, md5(image) FROM lerobot_video_frames({self.path}, [0,2]) "
            "ORDER BY episode_index, frame_index"
        )
        expected = [(episode, frame, black_frame_digest()) for episode in (0, 2) for frame in range(4)]
        if self.distributed:
            plan = self.vane.ray_cxx.PyLogicalPlan.from_duckdb_relation(self.connection.sql(sql), "lerobot-snapshot")
        else:
            self.connection.execute("PREPARE lerobot_snapshot AS " + sql)
        meta, hidden = self.root / "meta", self.root / "bound-meta"
        meta.rename(hidden)
        try:
            if self.distributed:
                actual = [
                    tuple(row.values()) for table in self.runner.run_iter_tables(plan) for row in table.to_pylist()
                ]
            else:
                actual = self.connection.execute("EXECUTE lerobot_snapshot").fetchall()
            self.assertEqual(actual, expected)
        finally:
            hidden.rename(meta)

    def test_targets_with_sparse_camera_routes(self):
        import pyarrow as pa
        import pyarrow.parquet as pq

        root = self.workspace / "sparse-cameras"
        fixture(root)
        info_path = root / "meta/info.json"
        info = json.loads(info_path.read_text())
        info["features"]["wrist"] = info["features"]["camera"].copy()
        info["video_path"] = "videos/{video_key}/{file_index}.mp4"
        info_path.write_text(json.dumps(info))
        episodes_path = root / "meta/episodes/0.parquet"
        episodes = pq.read_table(episodes_path).to_pylist()
        for index, episode in enumerate(episodes):
            key = "wrist" if index == 0 else "camera"
            for field in (
                "chunk_index",
                "file_index",
                "from_timestamp",
                "to_timestamp",
            ):
                camera = f"videos/camera/{field}"
                episode[f"videos/wrist/{field}"] = episode[camera] if index == 0 else None
                if index == 0:
                    episode[camera] = None
            directory = root / "videos" / key
            directory.mkdir(exist_ok=True)
            (root / f"videos/{index}.mp4").rename(directory / f"{index}.mp4")
        pq.write_table(pa.Table.from_pylist(episodes), episodes_path)
        requests = (
            f"(SELECT index AS target_id, 7 AS request_id, episode_index, frame_index, "
            "CASE WHEN episode_index = 0 THEN 'wrist' ELSE 'camera' END AS video_key, "
            f"0 AS delta_index FROM lerobot_scan({quote(root)}))"
        )
        rows = self.query(
            "SELECT target_id, video_key, md5(image) FROM "
            f"lerobot_video_targets({quote(root)}, {requests}) ORDER BY target_id",
            [(index, "wrist" if index < 4 else "camera", black_frame_digest()) for index in range(16)],
        )
        self.assertEqual(len(rows), 16)
        self.assertEqual([row[1] for row in rows], ["wrist"] * 4 + ["camera"] * 12)

    def test_bind_errors_remain_exceptions(self):
        with self.assertRaisesRegex(Exception, "does not exist"):
            self.connection.execute("SELECT * FROM lerobot_no_such_function()")
        with self.assertRaises(Exception):
            self.connection.execute(f"SELECT * FROM lerobot_video_frames({self.path}, [0], width := -1)")

    def copy(self, source, destination, extra=""):
        if not extra:
            extra = ", FEATURES " + quote(json.dumps({"action": {"dtype": "float32", "shape": [1]}}))
        before = self.writes
        result = self.connection.execute(
            f"COPY ({source}) TO {quote(destination)} (FORMAT lerobot, FPS 30{extra})"
        ).fetchall()
        if self.distributed:
            self.assertEqual(self.writes, before + 1, "COPY bypassed Ray")
        return result

    def test_copy_numeric_video_and_rollback(self):
        root = self.workspace / "copy"
        source = (
            f"SELECT index // 4 AS episode_index, 'pick' AS task, action::FLOAT AS action "
            f"FROM lerobot_scan({self.path}) ORDER BY index"
        )
        self.assertEqual(self.copy(source, root), [(16,)])
        self.query(
            f"SELECT count(*), sum(action) FROM lerobot_scan({quote(root)})",
            [(16, 120.0)],
        )
        self.query(
            f"SELECT episode_index, min(frame_index), max(frame_index) FROM lerobot_scan({quote(root)}) "
            "GROUP BY episode_index ORDER BY episode_index",
            [(i, 0, 3) for i in range(4)],
        )
        with self.assertRaisesRegex(Exception, "already exists"):
            self.copy(source, root)
        failed = self.workspace / "failed"
        with self.assertRaisesRegex(Exception, "contiguous and ordered"):
            self.copy(
                "SELECT 2::BIGINT AS episode_index, 'bad' AS task, 1::FLOAT AS action",
                failed,
            )
        self.assertFalse(failed.exists())
        self.assertEqual(list(self.workspace.glob("*.vane-*")), [])
        self.assertEqual(list(self.workspace.glob("*.tmp-*")), [])
        self.assertEqual(
            self.copy(
                "SELECT 0::BIGINT AS episode_index, 'retry' AS task, 1::FLOAT AS action",
                failed,
            ),
            [(1,)],
        )
        video = self.workspace / "video-copy"
        video_source = (
            "SELECT i // 2 AS episode_index, 'camera' AS task, "
            "unhex(repeat('224466', 16*16)) AS camera FROM range(4) r(i) ORDER BY i"
        )
        features = json.dumps({"camera": {"dtype": "video", "shape": [16, 16, 3]}})
        self.assertEqual(
            self.copy(
                video_source,
                video,
                f", FEATURES {quote(features)}, RGB_CODEC 'libaom-av1'",
            ),
            [(4,)],
        )
        self.query(
            f"SELECT count(*), min(width), min(height), min(octet_length(image)) "
            f"FROM lerobot_video_frames({quote(video)}, [0,1])",
            [(4, 16, 16, 768)],
        )

    def test_copy_empty_and_worker_error(self):
        empty = self.workspace / "empty"
        self.assertEqual(
            self.copy(
                "SELECT 0::BIGINT AS episode_index, 'empty' AS task, 0::FLOAT AS action WHERE false",
                empty,
            ),
            [(0,)],
        )
        self.query(
            f"SELECT total_frames, total_episodes FROM lerobot_info({quote(empty)})",
            [(0, 0)],
        )
        failed = self.workspace / "worker-error"
        source = (
            f"SELECT index // 4 AS episode_index, 'fail' AS task, "
            f"(CASE WHEN index = 8 THEN error('injected worker error') ELSE action END)::FLOAT AS action "
            f"FROM lerobot_scan({self.path}) ORDER BY index"
        )
        with self.assertRaisesRegex(Exception, "injected worker error"):
            self.copy(source, failed)
        self.assertFalse(failed.exists())
        self.assertEqual(list(self.workspace.glob("*.vane-*")), [])
        self.assertEqual(list(self.workspace.glob("*.tmp-*")), [])

    def test_copy_preserves_values_under_hive_style_directory(self):
        import pyarrow.parquet as pq

        parent = self.workspace / "c2=999"
        parent.mkdir()
        destination = parent / "copy"
        source = "SELECT i // 2 AS episode_index, 'pick' AS task, i::FLOAT AS action FROM range(4) r(i) ORDER BY i"
        self.assertEqual(self.copy(source, destination), [(4,)])
        # Read physical files without interpreting the parent directory as a partition.
        rows = [
            row for path in (destination / "data").rglob("*.parquet") for row in pq.ParquetFile(path).read().to_pylist()
        ]
        self.assertEqual(
            sorted((row["index"], row["episode_index"], row["frame_index"], row["action"]) for row in rows),
            [(i, i // 2, i % 2, float(i)) for i in range(4)],
        )
        self.assertEqual(list(parent.glob("*.vane-*")), [])
        self.assertEqual(list(parent.glob("*.tmp-*")), [])

    def test_copy_video_ignores_binary_as_string(self):
        destination = self.workspace / "binary-video-copy"
        source = (
            "SELECT i // 2 AS episode_index, 'camera' AS task, "
            "unhex(repeat('ff0080', 16*16)) AS camera FROM range(4) r(i) ORDER BY i"
        )
        features = json.dumps({"camera": {"dtype": "video", "shape": [16, 16, 3]}})
        previous = self.connection.execute("SELECT current_setting('binary_as_string')").fetchone()[0]
        try:
            self.connection.execute("SET binary_as_string=true")
            self.assertEqual(
                self.copy(source, destination, f", FEATURES {quote(features)}, RGB_CODEC 'libaom-av1'"),
                [(4,)],
            )
            self.assertTrue(self.connection.execute("SELECT current_setting('binary_as_string')").fetchone()[0])
        finally:
            self.connection.execute(f"SET binary_as_string={str(previous).lower()}")
        self.query(
            f"SELECT episode_index, frame_index, width, height, octet_length(image) "
            f"FROM lerobot_video_frames({quote(destination)}, [0,1]) ORDER BY episode_index, frame_index",
            [(i // 2, i % 2, 16, 16, 768) for i in range(4)],
        )
        self.assertEqual(list(self.workspace.glob("*.vane-*")), [])
        self.assertEqual(list(self.workspace.glob("*.tmp-*")), [])

    def test_copy_destination_with_trailing_separator(self):
        destination = self.workspace / "trailing-copy"
        source = "SELECT 0::BIGINT AS episode_index, 'pick' AS task, 1::FLOAT AS action"
        self.assertEqual(self.copy(source, str(destination) + "///"), [(1,)])
        self.query(
            f"SELECT count(*), sum(action) FROM lerobot_scan({quote(destination)})",
            [(1, 1.0)],
        )
        failed = self.workspace / "trailing-failed"
        with self.assertRaisesRegex(Exception, "contiguous and ordered"):
            self.copy(source.replace("0::BIGINT", "2::BIGINT"), str(failed) + "/")
        self.assertFalse(failed.exists())
        self.assertEqual(list(self.workspace.glob("*.vane-*")), [])
        self.assertEqual(list(self.workspace.glob("*.tmp-*")), [])

    def test_copy_rejects_remote_destinations_before_creating_local_paths(self):
        source = "SELECT 0::BIGINT AS episode_index, 'remote' AS task, 1::FLOAT AS action"
        previous = Path.cwd()
        with tempfile.TemporaryDirectory(dir=self.workspace) as directory:
            try:
                os.chdir(directory)
                for destination in (
                    "s3://vane-copy-test/dataset",
                    "https://example.invalid/dataset",
                    "http://example.invalid/dataset/",
                    "s3://",
                ):
                    with self.subTest(destination=destination):
                        with self.assertRaisesRegex(Exception, "requires a shared local filesystem"):
                            self.copy(source, destination)
                        self.assertEqual(list(Path(directory).iterdir()), [])
            finally:
                os.chdir(previous)

    def test_copy_write_empty_file_false(self):
        source = (
            f"SELECT index // 4 AS episode_index, 'pick' AS task, action::FLOAT AS action "
            f"FROM lerobot_scan({self.path})"
        )
        empty_sources = {
            "constant": "SELECT 0::BIGINT AS episode_index, 'empty' AS task, 0::FLOAT AS action WHERE false",
            "filtered": source + " WHERE md5(index::VARCHAR) = 'never'",
        }
        options = ", FEATURES " + quote(json.dumps({"action": {"dtype": "float32", "shape": [1]}}))
        options += ", WRITE_EMPTY_FILE false"
        for name, empty_source in empty_sources.items():
            with self.subTest(source=name):
                destination = self.workspace / f"empty-disabled-{name}"
                for _ in range(2):
                    self.assertEqual(self.copy(empty_source, destination, options), [(0,)])
                    self.assertFalse(destination.exists())
                    self.assertEqual(list(self.workspace.glob("*.vane-*")), [])
                    self.assertEqual(list(self.workspace.glob("*.tmp-*")), [])
                self.assertEqual(self.copy(source + " ORDER BY index", destination, options), [(16,)])
                metadata = (destination / "meta/info.json").read_bytes()
                self.assertEqual(self.copy(empty_source, destination, options), [(0,)])
                self.assertEqual((destination / "meta/info.json").read_bytes(), metadata)
                self.query(
                    f"SELECT count(*), sum(action) FROM lerobot_scan({quote(destination)})",
                    [(16, 120.0)],
                )
                with self.assertRaisesRegex(Exception, "already exists"):
                    self.copy(source + " ORDER BY index", destination, options)
                self.assertEqual(list(self.workspace.glob("*.vane-*")), [])
                self.assertEqual(list(self.workspace.glob("*.tmp-*")), [])

    def test_worker_topology(self):
        if not self.distributed:
            self.skipTest("worker topology requires VANE_RUNNER=ray")
        import ray

        with tempfile.TemporaryDirectory(dir=self.workspace) as directory:
            WorkerBarrier.directory = directory
            rows = (
                self.connection.sql(
                    f"SELECT episode_index, md5(image) AS digest FROM lerobot_video_frames({self.path}, [0,2,5,9])"
                )
                .map_batches(
                    WorkerBarrier,
                    schema={
                        "episode_index": self.vane.sqltype("BIGINT"),
                        "digest": self.vane.sqltype("VARCHAR"),
                        "node": self.vane.sqltype("VARCHAR"),
                    },
                    batch_size=4,
                    cpus=1.0,
                    execution_backend="ray_actor",
                    actor_number=2,
                    target_max_batch_bytes=4096,
                )
                .fetchall()
            )
        expected_nodes = {
            str(n["NodeID"]) for n in ray.nodes() if n.get("Alive") and n.get("Resources", {}).get("CPU", 0) >= 1
        }
        self.assertEqual({row[2] for row in rows}, expected_nodes)
        self.assertEqual(len(expected_nodes), 2)
        self.assertEqual(len(rows), 16)
        self.assertEqual(
            sorted((row[0], row[1]) for row in rows),
            [(episode, black_frame_digest()) for episode in (0, 2, 5, 9) for _ in range(4)],
        )
        stats = ray.get(self.runner.query_driver_client.runner.fragment_stats.remote())
        self.assertEqual(len(stats["workers"]), 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
