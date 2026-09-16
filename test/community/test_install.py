"""Install the signed community binary and exercise it without a source build.

Run with DuckDB 1.5.5: python -m unittest discover -s test/community -v.
The installed revision is reported because community releases advance separately
from this repository's HEAD. Network or installation failures fail the test.
"""

import json
from pathlib import Path
import tempfile
import unittest

import duckdb


def sql_path(path):
    return "'" + Path(path).as_posix().replace("'", "''") + "'"


class CommunityInstallTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory(prefix="lerobot-community-")
        cls.addClassCleanup(tmp.cleanup)
        cls.root = Path(tmp.name)
        cls.db = duckdb.connect(
            config={
                "extension_directory": str(cls.root / "extensions"),
                "allow_unsigned_extensions": "false",
                "threads": "2",
            }
        )
        cls.addClassCleanup(cls.db.close)
        cls.db.execute("INSTALL lerobot FROM community")
        cls.db.execute("LOAD lerobot")

    def test_community_install(self):
        row = self.db.execute(
            "SELECT loaded, installed, extension_version, install_mode, installed_from "
            "FROM duckdb_extensions() WHERE extension_name = 'lerobot'"
        ).fetchone()
        self.assertIsNotNone(row)
        self.assertEqual(row[:2], (True, True))
        self.assertTrue(row[2])
        self.assertEqual(row[3:], ("REPOSITORY", "community"))
        self.assertFalse(self.db.execute("SELECT current_setting('allow_unsigned_extensions')").fetchone()[0])
        print(json.dumps({"duckdb": duckdb.__version__, "lerobot": row[2], "repository": row[4]}), flush=True)

    def test_numeric_roundtrip(self):
        dataset = sql_path(self.root / "numeric")
        self.db.execute(
            f"""COPY (
                SELECT 0::BIGINT AS episode_index,
                       'pick up a block' AS task, i::FLOAT AS action
                FROM range(10) AS frames(i) ORDER BY i
            ) TO {dataset} (
                FORMAT lerobot, FPS 10,
                FEATURES '{{"action": {{"dtype": "float32", "shape": [1]}}}}'
            )"""
        )
        rows = self.db.execute(
            f"SELECT episode_index, frame_index, timestamp, action FROM lerobot_scan({dataset}) ORDER BY frame_index"
        ).fetchall()
        self.assertEqual(len(rows), 10)
        for index, row in enumerate(rows):
            self.assertEqual(row[:2], (0, index))
            self.assertAlmostEqual(row[2], index / 10, delta=1e-6)
            self.assertEqual(row[3], float(index))

    def test_video_and_image_roundtrip(self):
        dataset = sql_path(self.root / "visual")
        features = json.dumps(
            {
                "observation.images.front": {"dtype": "video", "shape": [64, 64, 3]},
                "observation.images.still": {"dtype": "image", "shape": [64, 64, 3]},
            }
        )
        colors = [(200, 40, 20), (20, 40, 200)]
        self.db.execute(
            f"""COPY (
                SELECT 0::BIGINT AS episode_index, 'two colors' AS task,
                       from_hex(repeat(CASE WHEN i = 0 THEN 'c82814' ELSE '1428c8' END, 4096))
                           AS "observation.images.front",
                       from_hex(repeat(CASE WHEN i = 0 THEN 'c82814' ELSE '1428c8' END, 4096))
                           AS "observation.images.still"
                FROM range(2) AS frames(i) ORDER BY i
            ) TO {dataset} (FORMAT lerobot, FPS 10, FEATURES '{features}', ENCODER_THREADS 1)"""
        )
        videos = list((self.root / "visual" / "videos").rglob("*.mp4"))
        self.assertEqual(len(videos), 1)
        self.assertGreater(videos[0].stat().st_size, 0)

        frames = self.db.execute(
            f"""SELECT episode_index, frame_index, width, height, channels, image
            FROM lerobot_video_frames({dataset}, [0],
                video_keys := ['observation.images.front'], frame_indices := [0, 1])
            ORDER BY frame_index"""
        ).fetchall()
        self.assertEqual(len(frames), 2)
        for index, row in enumerate(frames):
            self.assertEqual(row[:5], (0, index, 64, 64, 3))
            self.assertEqual(len(row[5]), 64 * 64 * 3)
            # AV1 uses lossy YUV here. Check actual channel values and frame order
            # with room for conversion/quantization differences across platforms.
            for channel, expected in enumerate(colors[index]):
                self.assertLessEqual(max(abs(value - expected) for value in row[5][channel::3]), 8)

        images = self.db.execute(
            f"""SELECT frame_index,
                lerobot_decode_image(("observation.images.still").bytes) AS decoded
            FROM lerobot_scan({dataset}) ORDER BY frame_index"""
        ).fetchall()
        self.assertEqual(len(images), 2)
        for index, (frame_index, decoded) in enumerate(images):
            self.assertEqual(frame_index, index)
            self.assertEqual((decoded["width"], decoded["height"], decoded["channels"]), (64, 64, 3))
            self.assertEqual(decoded["image"], bytes(colors[index]) * (64 * 64))


if __name__ == "__main__":
    unittest.main()
