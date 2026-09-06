#!/usr/bin/env python3
"""Compare numeric episode statistics with pinned LeRobot's actual routines."""

import argparse
import importlib.metadata
import json
import tempfile
from pathlib import Path

import numpy as np
import pyarrow.parquet as pq
from lerobot.datasets.compute_stats import compute_episode_stats

from test_bidirectional import LEROBOT_VERSION, run_duckdb, sql_quote

SQL_TYPES = {
    "float32": "FLOAT",
    "float64": "DOUBLE",
    "int8": "TINYINT",
    "int16": "SMALLINT",
    "int32": "INTEGER",
    "int64": "BIGINT",
    "uint8": "UTINYINT",
    "uint16": "USMALLINT",
    "uint32": "UINTEGER",
    "uint64": "UBIGINT",
    "bool": "BOOLEAN",
}


def scenario(root: Path, width: int, rows: int, dtypes: list[str]) -> tuple[str, dict, dict]:
    features = {dtype: {"dtype": dtype, "shape": [width]} for dtype in dtypes}
    data = {}
    columns = []
    for dtype in dtypes:
        values = (np.arange(rows)[:, None] + np.arange(width)[None, :]) % 7
        if dtype.startswith("uint"):
            expression = "(i + {d}) % 7"
        elif dtype == "bool":
            values = values % 2 == 0
            expression = "((i + {d}) % 7) % 2 = 0"
        else:
            values = values - 3
            if dtype.startswith("float"):
                values = values / 2
                expression = "(((i + {d}) % 7) - 3) / 2.0"
            else:
                expression = "((i + {d}) % 7) - 3"
        data[dtype] = values.astype(dtype)
        leaves = [f"({expression.format(d=d)})::{SQL_TYPES[dtype]}" for d in range(width)]
        value = leaves[0] if width == 1 else "array_value(" + ",".join(leaves) + ")"
        columns.append(f'{value} AS "{dtype}"')
    sql = (
        "COPY (SELECT 0::BIGINT AS episode_index, 'stats oracle' AS task, "
        + ",".join(columns)
        + f" FROM range({rows}) t(i) ORDER BY i) TO {sql_quote(root)} "
        f"(FORMAT lerobot, FPS 30, FEATURES {sql_quote(json.dumps(features))});"
    )
    return sql, data, features


def check(root: Path, data: dict, features: dict) -> int:
    reference = compute_episode_stats(data, features)
    episode = pq.read_table(next((root / "meta/episodes").rglob("*.parquet"))).to_pylist()[0]
    dataset = json.loads((root / "meta/stats.json").read_text())
    checks = 0
    for feature, stats in reference.items():
        for name, expected in stats.items():
            # This dyadic fixture keeps reductions exact before division. It
            # exercises shape/chunk/pairwise boundaries without masking a
            # changed reader behind a floating-point tolerance.
            for actual in (episode[f"stats/{feature}/{name}"], dataset[feature][name]):
                np.testing.assert_array_equal(actual, expected, err_msg=f"{root.name}/{feature}/{name}")
                checks += 1
    return checks


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", type=Path, required=True)
    parser.add_argument("--extension", type=Path, required=True)
    args = parser.parse_args()
    for name, version in (("lerobot", LEROBOT_VERSION), ("numpy", "2.2.6")):
        if importlib.metadata.version(name) != version:
            raise RuntimeError(f"expected {name}=={version}")
    cases = [(width, rows, ["float32", "float64"]) for width in (1, 14, 64, 65, 256) for rows in (1, 7, 8, 129, 2050)]
    cases += [(65, 1, list(SQL_TYPES)), (65, 129, list(SQL_TYPES))]
    checks = 0
    with tempfile.TemporaryDirectory(prefix="lerobot-stats-oracle-") as directory:
        for index, (width, rows, dtypes) in enumerate(cases):
            root = Path(directory) / f"case-{index}-{width}-{rows}"
            sql, data, features = scenario(root, width, rows, dtypes)
            run_duckdb(args.duckdb, args.extension, "SET threads=1; " + sql)
            checks += check(root, data, features)
    print(f"Pinned LeRobot numeric statistics: {len(cases)} cases, {checks} exact array comparisons passed")


if __name__ == "__main__":
    main()
