#!/usr/bin/env python3

import argparse
import csv
import importlib.metadata
import io
import json
import subprocess
import tempfile
from pathlib import Path

import numpy as np
import pandas as pd
import pyarrow.parquet as pq
from lerobot.datasets.lerobot_dataset import LeRobotDataset

LEROBOT_VERSION = "0.6.1"
FEATURES = {
    "observation.state": {
        "dtype": "float32",
        "shape": (2,),
        "names": ["x", "y"],
    },
    "action": {"dtype": "float32", "shape": (1,), "names": None},
}


def sql_quote(value: str | Path) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def run_duckdb(duckdb: Path, extension: Path, sql: str) -> list[list[str]]:
    command = [
        str(duckdb),
        "-unsigned",
        "-csv",
        "-noheader",
        "-c",
        f"LOAD {sql_quote(extension)}; {sql}",
    ]
    result = subprocess.run(command, check=True, text=True, capture_output=True)
    return list(csv.reader(io.StringIO(result.stdout)))


def create_reference_dataset(root: Path, fps: int = 10) -> None:
    dataset = LeRobotDataset.create(
        repo_id="conformance/reference",
        root=root,
        fps=fps,
        robot_type="conformance",
        features=FEATURES,
        use_videos=False,
    )
    episodes = (
        ("pick", ((1.0, 2.0, 0.25), (3.0, 4.0, 0.50))),
        ("place", ((5.0, 6.0, 0.75),)),
    )
    for task, frames in episodes:
        for x, y, action in frames:
            dataset.add_frame(
                {
                    "task": task,
                    "observation.state": np.asarray([x, y], dtype=np.float32),
                    "action": np.asarray([action], dtype=np.float32),
                }
            )
        dataset.save_episode()
    dataset.finalize()


def assert_duckdb_reads_reference(duckdb: Path, extension: Path, root: Path) -> None:
    root_sql = sql_quote(root)
    rows = run_duckdb(
        duckdb,
        extension,
        'SELECT count(*), min("index"), max("index"), '
        "count(DISTINCT episode_index), round(sum(action), 2) "
        f"FROM lerobot_scan({root_sql});",
    )
    assert rows == [["3", "0", "2", "2", "1.5"]], rows

    rows = run_duckdb(
        duckdb,
        extension,
        'SELECT episode_index, frame_index, "index" ' f"FROM lerobot_scan({root_sql}, episode_indices := [1]);",
    )
    assert rows == [["1", "0", "2"]], rows

    rows = run_duckdb(
        duckdb,
        extension,
        f"SELECT total_episodes, total_frames, total_tasks, fps FROM lerobot_info({root_sql});",
    )
    assert rows == [["2", "3", "2", "10"]], rows

    rows = run_duckdb(
        duckdb,
        extension,
        f"SELECT task_index, task FROM lerobot_tasks({root_sql}) ORDER BY task_index;",
    )
    assert rows == [["0", "pick"], ["1", "place"]], rows

    rows = run_duckdb(
        duckdb,
        extension,
        f"SELECT episode_index, length FROM lerobot_episodes({root_sql}) ORDER BY episode_index;",
    )
    assert rows == [["0", "2"], ["1", "1"]], rows


def create_duckdb_dataset(duckdb: Path, extension: Path, root: Path, fps: int = 10) -> None:
    features_json = (
        '{"observation.state":{"dtype":"float32","shape":[2],'
        '"names":["x","y"]},"action":{"dtype":"float32",'
        '"shape":[1],"names":null}}'
    )
    run_duckdb(
        duckdb,
        extension,
        "COPY ("
        "SELECT episode_index, task, "
        'array_value(x::FLOAT, y::FLOAT) AS "observation.state", action::FLOAT AS action '
        "FROM (VALUES "
        "(0::BIGINT, 'pick'::VARCHAR, 1.0, 2.0, 0.25), "
        "(0::BIGINT, 'pick'::VARCHAR, 3.0, 4.0, 0.50), "
        "(1::BIGINT, 'place'::VARCHAR, 5.0, 6.0, 0.75)"
        ") frames(episode_index, task, x, y, action) "
        "ORDER BY episode_index, x"
        f") TO {sql_quote(root)} (FORMAT lerobot, FPS {fps}, ROBOT_TYPE 'conformance', "
        f"FEATURES {sql_quote(features_json)});",
    )


def assert_reference_reads_duckdb(root: Path) -> None:
    # COPY writes the pandas index description itself. It must not claim that
    # PyArrow/Pandas generated the file, and removing those provenance fields
    # must preserve the task index used by the official LeRobot reader.
    tasks_path = root / "meta/tasks.parquet"
    metadata = json.loads(pq.read_schema(tasks_path).metadata[b"pandas"])
    assert "creator" not in metadata and "pandas_version" not in metadata, metadata
    tasks = pd.read_parquet(tasks_path)
    assert tasks.index.name == "task" and list(tasks.index) == ["pick", "place"]
    assert list(tasks.columns) == ["task_index"]
    assert list(tasks["task_index"]) == [0, 1]

    dataset = LeRobotDataset(
        repo_id="conformance/duckdb",
        root=root,
        download_videos=False,
    )
    assert dataset.num_episodes == 2
    assert dataset.num_frames == 3
    assert dataset.fps == 10

    expected = (
        (0, 0, "pick", (1.0, 2.0), 0.25),
        (0, 1, "pick", (3.0, 4.0), 0.50),
        (1, 0, "place", (5.0, 6.0), 0.75),
    )
    for index, (episode, frame, task, state, action) in enumerate(expected):
        item = dataset[index]
        assert int(item["episode_index"]) == episode
        assert int(item["frame_index"]) == frame
        assert item["task"] == task
        np.testing.assert_allclose(item["observation.state"], state, rtol=0, atol=1e-6)
        np.testing.assert_allclose(item["action"], [action], rtol=0, atol=1e-6)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", required=True, type=Path)
    parser.add_argument("--extension", required=True, type=Path)
    args = parser.parse_args()

    installed = importlib.metadata.version("lerobot")
    if installed != LEROBOT_VERSION:
        raise RuntimeError(f"expected lerobot {LEROBOT_VERSION}, found {installed}")
    if not args.duckdb.is_file():
        raise FileNotFoundError(args.duckdb)
    if not args.extension.is_file():
        raise FileNotFoundError(args.extension)

    with tempfile.TemporaryDirectory(prefix="duckdb-lerobot-conformance-") as directory:
        workspace = Path(directory)
        reference_root = workspace / "reference"
        duckdb_root = workspace / "duckdb"
        create_reference_dataset(reference_root)
        assert_duckdb_reads_reference(args.duckdb, args.extension, reference_root)
        create_duckdb_dataset(args.duckdb, args.extension, duckdb_root)
        assert_reference_reads_duckdb(duckdb_root)
        for fps in (3, 30, 16777217, 16777219):
            reference = workspace / f"reference-fps-{fps}"
            actual_root = workspace / f"duckdb-fps-{fps}"
            create_reference_dataset(reference, fps)
            create_duckdb_dataset(args.duckdb, args.extension, actual_root, fps)
            expected = pq.read_table(sorted((reference / "data").rglob("*.parquet"))).column("timestamp").to_numpy()
            actual = pq.read_table(sorted((actual_root / "data").rglob("*.parquet"))).column("timestamp").to_numpy()
            assert actual.dtype == expected.dtype == np.float32
            np.testing.assert_array_equal(actual.view(np.uint32), expected.view(np.uint32))


if __name__ == "__main__":
    main()
