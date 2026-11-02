#!/usr/bin/env python3
"""Compare a COPY-created dataset with the real lerobot-lancedb converter output."""

import argparse
import importlib.metadata
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

from lerobot.annotations.steerable_pipeline.reader import iter_episodes
from lerobot.annotations.steerable_pipeline.staging import EpisodeStaging
from lerobot.annotations.steerable_pipeline.writer import LanguageColumnsWriter, speech_atom
from lerobot.datasets.io_utils import load_info, write_info
from lerobot.datasets.language import (
    SAY_TOOL_SCHEMA,
    language_events_arrow_type,
    language_feature_info,
    language_persistent_arrow_type,
)
from lerobot_lancedb.convert import _storage_type, convert


def quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def run(args, sql, error=None):
    result = subprocess.run(
        [
            str(args.duckdb),
            "-unsigned",
            "-json",
            "-c",
            f"LOAD {quote(args.extension)}; {sql}",
        ],
        capture_output=True,
        text=True,
        timeout=120,
    )
    if error is not None:
        assert result.returncode != 0 and error in result.stderr, (
            result.stdout,
            result.stderr,
        )
        return
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout) if result.stdout.strip() else []


def check_language(args, base, source, missing):
    # Use the official writer's inferred nested Arrow types, rather than hand-built structs.
    parquet, lance = base / "language-parquet", base / "language-lance"
    shutil.copytree(source, parquet)
    records = list(iter_episodes(parquet))
    assert [record.episode_index for record in records] == [0, 1]
    staging = base / "language-staging"
    for record in records:
        episode = record.episode_index
        staged = EpisodeStaging(staging, episode)
        staged.write(
            "plan",
            [
                {
                    "role": "assistant",
                    "content": f"Move object {episode}",
                    "style": "plan",
                    "timestamp": record.frame_timestamps[0],
                    "camera": None,
                    "tool_calls": None,
                }
            ],
        )
        staged.write(
            "interjections",
            [
                speech_atom(record.frame_timestamps[0], f"Moving object {episode}"),
                {
                    "role": "assistant",
                    "content": f"Still moving {episode}",
                    "style": "interjection",
                    "timestamp": record.frame_timestamps[1],
                    "camera": None,
                    "tool_calls": None,
                },
            ],
        )
    LanguageColumnsWriter().write_all(records, staging, parquet)
    info = load_info(parquet)
    info.features = {**info.features, **language_feature_info()}
    info.tools = [SAY_TOOL_SCHEMA]
    write_info(info, parquet)
    convert(lance, root=parquet)

    schema = "DESCRIBE SELECT * FROM lerobot_scan({});"
    reference_schema = run(args, schema.format(quote(parquet)))
    assert {"language_persistent", "language_events"} <= {row["column_name"] for row in reference_schema}
    assert run(args, schema.format(quote(lance))) == reference_schema
    for query in (
        'SELECT * FROM lerobot_scan({}) ORDER BY "index";',
        'SELECT action FROM lerobot_scan({}) ORDER BY "index";',
        'SELECT * FROM lerobot_scan({}) WHERE episode_index = 1 ORDER BY "index";',
        'SELECT * FROM lerobot_scan({}, episode_indices := [0]) ORDER BY "index";',
        'SELECT * FROM lerobot_scan({}, episode_indices := [1, 0, 1]) ORDER BY "index";',
    ):
        reference = run(args, query.format(quote(parquet)))
        assert reference, query
        assert run(args, query.format(quote(lance))) == reference, query
    empty = 'SELECT * FROM lerobot_scan({}, episode_indices := []);'
    for root in (parquet, lance):
        assert run(args, empty.format(quote(root))) == []
        assert run(args, "DESCRIBE " + empty.format(quote(root))) == reference_schema

    # Tool arguments must remain a nested struct, including when projected directly.
    nested = (
        'SELECT episode_index, language_events[1].tool_calls[1].function.arguments.text AS speech '
        'FROM lerobot_scan({}) WHERE frame_index = 0 ORDER BY episode_index;'
    )
    expected = [{"episode_index": episode, "speech": f"Moving object {episode}"} for episode in range(2)]
    assert run(args, nested.format(quote(parquet))) == expected
    assert run(args, nested.format(quote(lance))) == expected

    # Each run opens a fresh process: metadata must work without loading Lance or opening frames.lance.
    for function, order in (("episodes", "episode_index"), ("tasks", "task_index"), ("stats", "feature")):
        query = f"SELECT * FROM lerobot_{function}({{}}) ORDER BY {order};"
        reference = run(args, missing + query.format(quote(parquet)))
        assert reference, function
        assert run(args, missing + query.format(quote(lance))) == reference, function
    temporal = (
        "SELECT * FROM lerobot_temporal_targets({}, "
        "(SELECT * FROM (VALUES (1, 0, 1, 0), (2, 1, 1, 0)) "
        "AS requests(request_id, episode_index, frame_index, delta_index))) ORDER BY request_id;"
    )
    reference = run(args, missing + temporal.format(quote(parquet)))
    assert len(reference) == 2
    assert run(args, missing + temporal.format(quote(lance))) == reference
    check_canonical_language(args, base, parquet)


def check_canonical_language(args, base, source):
    parquet, lance = base / "language-json-parquet", base / "language-json-lance"
    shutil.copytree(source, parquet)
    for path in (parquet / "data").rglob("*.parquet"):
        table = pq.read_table(path)
        for name, dtype in (
            ("language_persistent", language_persistent_arrow_type()),
            ("language_events", language_events_arrow_type()),
        ):
            values = table[name].to_pylist()
            for rows in values:
                for row in rows or []:
                    if row["tool_calls"] is not None:
                        row["tool_calls"] = [json.dumps(call) for call in row["tool_calls"]]
            # Build JSON's string storage before applying the official nested extension types.
            column = pa.array(values, type=_storage_type(dtype)).cast(dtype)
            table = table.set_column(table.schema.get_field_index(name), name, column)
        pq.write_table(table, path)
    convert(lance, root=parquet)

    schema = "DESCRIBE SELECT * FROM lerobot_scan({});"
    reference_schema = run(args, schema.format(quote(parquet)))
    assert run(args, schema.format(quote(lance))) == reference_schema
    for query in (
        'SELECT * FROM lerobot_scan({}) ORDER BY "index";',
        'SELECT language_persistent, language_events FROM lerobot_scan({}) ORDER BY "index";',
        'SELECT * FROM lerobot_scan({}, episode_indices := [1, 0, 1]) ORDER BY "index";',
        'SELECT to_json(language_persistent) AS persistent_json, to_json(language_events) AS events_json '
        'FROM lerobot_scan({}) ORDER BY "index";',
    ):
        reference = run(args, query.format(quote(parquet)))
        assert reference, query
        assert run(args, query.format(quote(lance))) == reference, query

    tool_calls = (
        'SELECT episode_index, typeof(language_persistent[1].tool_calls[1]) AS persistent_type, '
        'typeof(language_events[1].tool_calls[1]) AS event_type, '
        'to_json(language_events[1].tool_calls[1]) AS tool_call_json '
        'FROM lerobot_scan({}) WHERE frame_index = 0 ORDER BY episode_index;'
    )
    reference = run(args, tool_calls.format(quote(parquet)))
    assert [row["episode_index"] for row in reference] == [0, 1]
    for row in reference:
        assert row["persistent_type"] == row["event_type"] == "JSON", row
        # The CLI's JSON output must contain an object, rather than a doubly escaped JSON string.
        assert row["tool_call_json"] == {
            "function": {"arguments": {"text": f"Moving object {row['episode_index']}"}, "name": "say"},
            "type": "function",
        }
    assert run(args, tool_calls.format(quote(lance))) == reference


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, required=True)
    parser.add_argument("--extension", type=Path, required=True)
    args = parser.parse_args()
    assert importlib.metadata.version("lerobot-lancedb") == "0.3.1"
    assert importlib.metadata.version("lerobot") == "0.6.1"
    with tempfile.TemporaryDirectory(prefix="lerobot-lance-") as tmp:
        base = Path(tmp)
        parquet, lance = base / "parquet", base / "lance's data"
        features = {
            "observation.state": {"dtype": "float32", "shape": [2]},
            "action": {"dtype": "float32", "shape": [1]},
            "next.done": {"dtype": "bool", "shape": [1]},
            "counter": {"dtype": "int64", "shape": [1]},
        }
        run(
            args,
            f"COPY (SELECT (i // 3)::BIGINT AS episode_index, 'move' AS task, "
            '[i, i+1]::FLOAT[2] AS "observation.state", i::FLOAT AS action, '
            '(i % 2 = 0) AS "next.done", (9007199254740993 + i)::BIGINT AS counter '
            f"FROM range(6) t(i) ORDER BY i) TO {quote(parquet)} "
            f"(FORMAT lerobot, FPS 10, FEATURES {quote(json.dumps(features))});",
        )
        convert(lance, root=parquet)
        for function, order in (
            ("scan", '"index"'),
            ("episodes", "episode_index"),
            ("tasks", "task_index"),
            ("stats", "feature"),
        ):
            reference = run(
                args,
                f"SELECT * FROM lerobot_{function}({quote(parquet)}) ORDER BY {order};",
            )
            actual = run(
                args,
                f"SELECT * FROM lerobot_{function}({quote(lance)}) ORDER BY {order};",
            )
            assert actual == reference, function
        left = run(args, f"SELECT * FROM lerobot_info({quote(parquet)});")
        right = run(args, f"SELECT * EXCLUDE(storage_format) FROM lerobot_info({quote(lance)});")
        assert left == right
        for selection in ("[0]", "[1, 0, 1]", "[]"):
            query = "SELECT * FROM lerobot_scan({}, episode_indices := " + selection + ') ORDER BY "index";'
            assert run(args, query.format(quote(parquet))) == run(args, query.format(quote(lance)))
        schema_query = "DESCRIBE SELECT * FROM lerobot_scan({});"
        assert run(args, schema_query.format(quote(parquet))) == run(args, schema_query.format(quote(lance)))

        # Loading an installed extension still works when automatic installation/loading is disabled.
        settings = "SET autoinstall_known_extensions=false; SET autoload_known_extensions=false; "
        assert run(args, settings + f"SELECT count(*) AS n FROM lerobot_scan({quote(lance)});") == [{"n": 6}]
        (base / "empty-extensions").mkdir()
        missing = settings + f"SET extension_directory={quote(base / 'empty-extensions')}; "
        run(
            args,
            missing + f"SELECT * FROM lerobot_scan({quote(lance)});",
            "INSTALL lance; LOAD lance;",
        )
        failed_install = (
            missing
            + "SET autoinstall_known_extensions=true; "
            + f"SET autoinstall_extension_repository={quote(base / 'missing-repository')}; "
        )
        run(
            args,
            failed_install + f"SELECT * FROM lerobot_scan({quote(lance)});",
            "INSTALL lance; LOAD lance;",
        )
        run(
            args,
            missing + f"SELECT * FROM lerobot_video_routes({quote(lance)}, [0]);",
            "INSTALL lance; LOAD lance;",
        )
        # Official DuckDB 1.5.5 Lance can scan frames but lacks the Blob filesystem.
        for call in (
            f"lerobot_video_routes({quote(lance)}, [0])",
            f"lerobot_video_frames({quote(lance)}, [0])",
            f"lerobot_video_windows({quote(lance)}, [{{request_id: 1, episode_index: 0, frame_index: 0}}])",
            f"lerobot_video_targets({quote(lance)}, (SELECT 1 AS request_id, 0 AS episode_index, "
            "0 AS frame_index, 'camera' AS video_key, 0 AS delta_index))",
        ):
            run(
                args,
                f"SELECT * FROM {call};",
                "require a lance extension with lance_blob_uri",
            )
        # Temporal targets need only meta/, so a cold cache must not open frames.lance.
        temporal = (
            "SELECT target_frame_index FROM lerobot_temporal_targets({}, "
            "(SELECT 1 AS request_id, 0 AS episode_index, 1 AS frame_index, 0 AS delta_index));"
        )
        assert run(args, missing + temporal.format(quote(lance))) == run(args, temporal.format(quote(parquet)))
        # Plain v3 never requires Lance, even with no extension installation available.
        assert run(args, missing + f"SELECT count(*) AS n FROM lerobot_scan({quote(parquet)});") == [{"n": 6}]

        # A scoped Lance secret changes cache identity without exposing credentials in the cache key.
        warm = f"LOAD lance; SELECT count(*) FROM lerobot_scan({quote(lance)}); "
        secret = f"CREATE SECRET lance_fixture (TYPE LANCE, SCOPE {quote(lance)}, HF_TOKEN 'public-fixture'); "
        check = f"SELECT cached FROM lerobot_cache_info({quote(lance)}) WHERE component='data';"
        result = subprocess.run(
            [
                str(args.duckdb),
                "-unsigned",
                "-csv",
                "-noheader",
                "-c",
                f"LOAD {quote(args.extension)}; "
                + warm
                + secret
                + check
                + f"SELECT count(*) FROM lerobot_scan({quote(lance)}); "
                + secret.replace("CREATE SECRET", "CREATE OR REPLACE SECRET").replace(
                    "public-fixture", "public-fixture-2"
                )
                + check,
            ],
            capture_output=True,
            text=True,
            timeout=120,
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout.splitlines() == [
            "6",
            "true",
            "false",
            "6",
            "true",
            "false",
        ], result.stdout
        check_language(args, base, parquet, missing)
        print(
            "PASS: converter parity (5 functions), schema, episode filters, optional loading, video rejection, "
            "metadata-only temporal targets, Lance secret cache isolation, converter-backed language "
            "scan/schema/action/filters/nested arguments, canonical language JSON types/serialization, "
            "and metadata-only episodes/tasks/stats/temporal"
        )


if __name__ == "__main__":
    main()
