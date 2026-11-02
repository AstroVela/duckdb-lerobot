import hashlib
import json
import os
import shutil
import weakref
from pathlib import Path

import duckdb
import pytest

from duckdb_lerobot import TorchCodecReader


@pytest.fixture
def connection():
    extension = os.environ.get("LEROBOT_EXTENSION")
    if not extension:
        pytest.skip("Set LEROBOT_EXTENSION to a DuckDB 1.5.5 lerobot extension")
    con = duckdb.connect(config={"allow_unsigned_extensions": True, "threads": 2})
    con.execute("LOAD '" + extension.replace("'", "''") + "'")
    yield con
    con.close()


@pytest.fixture
def dataset(connection, tmp_path):
    root = tmp_path / "dataset's videos"
    path = str(root).replace("'", "''")
    features = json.dumps(
        {
            "front": {
                "dtype": "video",
                "shape": [24, 32, 3],
                "names": ["height", "width", "channels"],
            },
            "side": {
                "dtype": "video",
                "shape": [16, 16, 3],
                "names": ["height", "width", "channels"],
            },
        }
    )
    connection.execute(f"""
        COPY (
            SELECT (i // 4)::BIGINT AS episode_index, 'test' AS task,
                   from_hex(repeat(lpad(hex(i * 25), 2, '0') || '2040', 32*24)) AS front,
                   from_hex(repeat('10' || lpad(hex(i * 25), 2, '0') || '30', 16*16)) AS side
            FROM range(8) AS frames(i) ORDER BY i
        ) TO '{path}' (FORMAT lerobot, FPS 10, ENCODER_THREADS 1, FEATURES '{features}')
    """)
    return root


REQUESTS = """
    SELECT 9::BIGINT AS request_id, episode_index, frame_index, video_key, delta_index
    FROM lerobot_scan(?)
    CROSS JOIN (VALUES ('front'), ('side')) AS cameras(video_key)
    CROSS JOIN range(3) AS offsets(delta_index)
    WHERE episode_index = ? AND frame_index IN (0, 3)
    ORDER BY frame_index DESC, video_key, delta_index
"""


def test_pixels_windows_duplicates_order_and_batches(connection, dataset):
    import torch

    params = [str(dataset), 1]
    deltas = [-0.1, 0, 0.1]
    expected = connection.execute(
        f"""
        SELECT target_id, request_id, target_frame_index, video_key, is_padding,
               height, width, sha256(image), video_timestamp
        FROM lerobot_video_targets(?, (
            SELECT requests.*, row_number() OVER () - 1 AS target_id
            FROM ({REQUESTS}) AS requests
        ), delta_timestamps := ?)
        ORDER BY target_id
    """,
        [str(dataset), *params, deltas],
    ).fetchall()
    actual = []
    with TorchCodecReader(
        connection, dataset, batch_size=5, max_cached_decoders=1
    ) as reader:
        sizes = []
        for batch in reader.batches(REQUESTS, params, delta_timestamps=deltas):
            sizes.append(len(batch.targets))
            assert len(reader._decoders) <= 1
            for target, image, pts in zip(
                batch.targets, batch.images, batch.decoded_timestamps
            ):
                assert image.dtype == torch.uint8
                assert image.device.type == "cpu"
                pixels = image.permute(1, 2, 0).contiguous().numpy().tobytes()
                actual.append(
                    (
                        target.target_id,
                        target.request_id,
                        target.target_frame_index,
                        target.video_key,
                        target.is_padding,
                        image.shape[1],
                        image.shape[2],
                        hashlib.sha256(pixels).hexdigest(),
                        target.video_timestamp,
                    )
                )
                assert abs(pts - target.video_timestamp) <= reader.tolerance
        assert sizes == [5, 5, 2]
    assert actual == expected
    assert any(row[4] for row in actual)
    assert not reader._decoders
    assert connection.execute("SELECT 42").fetchone() == (42,)


@pytest.mark.parametrize("threads", [1, 8])
def test_input_order_across_parallel_chunks(connection, dataset, monkeypatch, threads):
    # More than one DuckDB vector, producer chunk and output batch. All IDs
    # repeat; only the sequence of frame/camera/delta occurrences identifies
    # the input order. No video I/O is needed to exercise target routing.
    connection.execute(f"SET threads={threads}")
    query = """
        SELECT 9::BIGINT AS request_id, (i % 2)::BIGINT AS episode_index,
               (i % 4)::BIGINT AS frame_index,
               CASE WHEN i % 3 = 0 THEN 'front' ELSE 'side' END AS video_key,
               (i % 3)::BIGINT AS delta_index, -7::BIGINT AS target_id
        FROM range(?) AS inputs(i)
        ORDER BY (i * 7919) % 12289, i
    """
    # The expected sequence comes directly from the caller's SELECT, not from
    # another invocation of lerobot_video_targets or its execution counter.
    expected = connection.execute(query, [12289]).fetchall()
    for video in dataset.rglob("*.mp4"):
        video.unlink()
    with TorchCodecReader(connection, dataset, batch_size=257) as reader:
        monkeypatch.setattr(reader, "_decode", lambda targets: targets)
        actual = [
            target
            for batch in reader.batches(query, [12289], delta_timestamps=[-0.1, 0, 0.1])
            for target in batch
        ]
    assert [target.target_id for target in actual] == list(range(len(expected)))
    assert [
        (t.request_id, t.episode_index, t.frame_index, t.video_key, t.delta_index)
        for t in actual
    ] == [row[:5] for row in expected]
    for target in actual:
        resolved = target.frame_index + target.delta_index - 1
        assert target.target_frame_index == max(0, min(3, resolved))
        assert target.is_padding == (resolved < 0 or resolved > 3)


def test_empty_does_not_load_decoder_and_early_close(connection, dataset, monkeypatch):
    reader = TorchCodecReader(connection, dataset, batch_size=1)
    with monkeypatch.context() as patch:
        patch.setattr(
            reader, "_decoder", lambda _: pytest.fail("Empty selection opened video")
        )
        assert list(reader.batches(REQUESTS, [str(dataset), 99])) == []
    iterator = reader.batches(
        REQUESTS, [str(dataset), 0], delta_timestamps=[-0.1, 0, 0.1]
    )
    next(iterator)
    with pytest.raises(RuntimeError, match="Only one"):
        next(reader.batches(REQUESTS, [str(dataset), 0]))
    iterator.close()
    reader.close()
    assert list(reader.batches(REQUESTS, [str(dataset), 99])) == []


def test_timestamp_mismatch_rejected(connection, dataset):
    # Keep the route valid while moving frame timestamps away from the media PTS.
    data = next((dataset / "data").rglob("*.parquet"))
    connection.execute(
        "CREATE TEMP TABLE changed AS SELECT * FROM read_parquet(?)", [str(data)]
    )
    connection.execute(
        "UPDATE changed SET timestamp = timestamp + 0.002 WHERE frame_index = 0"
    )
    connection.execute(
        "COPY changed TO '" + str(data).replace("'", "''") + "' (FORMAT parquet)"
    )
    with TorchCodecReader(connection, dataset) as reader:
        with pytest.raises(ValueError, match="timestamp mismatch"):
            list(reader.batches("""
                SELECT 1 AS request_id, 0 AS episode_index, 0 AS frame_index,
                       'front' AS video_key, 0 AS delta_index
            """))


def test_metadata_projection_does_not_open_video(connection, dataset, monkeypatch):
    for video in dataset.rglob("*.mp4"):
        video.unlink()
    with TorchCodecReader(connection, dataset) as reader:
        seen = []

        def metadata_only(targets):
            seen.extend(targets)
            return targets

        monkeypatch.setattr(reader, "_decode", metadata_only)
        list(
            reader.batches(REQUESTS, [str(dataset), 1], delta_timestamps=[-0.1, 0, 0.1])
        )
        assert len(seen) == 12


def test_local_only_and_restricted_connection(connection, dataset):
    with pytest.raises(ValueError, match="locally"):
        TorchCodecReader(connection, "hf://datasets/org/dataset")
    with TorchCodecReader(connection, dataset) as reader:
        with pytest.raises(ValueError, match="inside"):
            reader._decoder(str(dataset.parent))
        connection.execute("SET enable_external_access=false")
        with pytest.raises(ValueError, match="enable_external_access"):
            list(reader.batches(REQUESTS, [str(dataset), 0]))


def test_depth_is_rejected_before_decode(connection, dataset, monkeypatch):
    info_path = dataset / "meta/info.json"
    info = json.loads(info_path.read_text())
    feature = info["features"]["front"]
    feature["shape"][2] = 1
    feature["info"].update(
        {
            "is_depth_map": True,
            "video.depth_min": 0.0,
            "video.depth_max": 10.0,
            "video.shift": 0.0,
            "video.use_log": False,
            "video.pix_fmt": "gray12le",
        }
    )
    info_path.write_text(json.dumps(info))
    with TorchCodecReader(connection, dataset) as reader:
        monkeypatch.setattr(
            reader, "_decoder", lambda _: pytest.fail("Depth video opened")
        )
        with pytest.raises(ValueError, match="RGB"):
            list(reader.batches("""
                SELECT 0 AS request_id, 0 AS episode_index, 0 AS frame_index,
                       'front' AS video_key, 0 AS delta_index
            """))


@pytest.mark.parametrize(
    "option,value",
    [
        ("batch_size", 0),
        ("batch_size", True),
        ("max_cached_decoders", -1),
        ("max_video_bytes", 0),
        ("max_video_bytes", True),
        ("num_ffmpeg_threads", 0),
        ("tolerance", float("nan")),
        ("device", "mps"),
    ],
)
def test_invalid_options(connection, dataset, option, value):
    with pytest.raises(ValueError):
        TorchCodecReader(connection, dataset, **{option: value})


@pytest.fixture
def lance_connection(connection):
    extension = os.environ.get("LANCE_EXTENSION")
    if not extension:
        pytest.skip("Set LANCE_EXTENSION to a compatible Lance Blob extension")
    connection.execute("LOAD '" + extension.replace("'", "''") + "'")
    return connection


@pytest.fixture
def lance_dataset(lance_connection, dataset, tmp_path):
    converter = pytest.importorskip("lerobot_lancedb.convert")
    root = tmp_path / "lance's videos"
    converter.convert(root, root=dataset)
    return root


def _blob_storage(root, kind):
    lance = pytest.importorskip("lance")
    pa = pytest.importorskip("pyarrow")
    from lance.blob import Blob, BlobArray

    path = root / "videos.lance"
    ds = lance.dataset(path)
    rows = ds.to_table(
        columns=["video_key", "chunk_index", "file_index", "file_size"],
        with_row_id=True,
    ).to_pylist()
    payloads = []
    for row in rows:
        with ds.take_blobs("video_bytes", ids=[row.pop("_rowid")])[0] as blob:
            payloads.append(blob.read())
    values = payloads
    if kind == "external":
        values = []
        for index, payload in enumerate(payloads):
            external = root / f"external-{index}.bin"
            external.write_bytes(b"prefix!" + payload + b"!suffix")
            values.append(Blob.from_uri(external.as_uri(), 7, len(payload)))
    blobs = BlobArray.from_pylist(values)
    threshold = max(map(len, payloads)) + 1
    metadata = {
        "lance-encoding:blob-inline-size-threshold": str(
            threshold if kind == "inline" else 0
        ),
        "lance-encoding:blob-dedicated-size-threshold": str(
            threshold if kind in ("inline", "packed") else 1
        ),
        "lance-encoding:blob-pack-file-size-threshold": str(threshold * 4),
    }
    table = pa.Table.from_pylist(rows).append_column(
        pa.field("video_bytes", blobs.type, metadata=metadata), blobs
    )
    shutil.rmtree(path)
    lance.write_dataset(
        table,
        path,
        data_storage_version="2.2",
        enable_stable_row_ids=True,
        max_rows_per_file=1,
        allow_external_blob_outside_bases=True,
    )
    descriptors = lance.dataset(path).to_table(columns=["video_bytes"]).to_pylist()
    expected = {"inline": 0, "packed": 1, "dedicated": 2, "external": 3}[kind]
    assert all(row["video_bytes"]["kind"] == expected for row in descriptors)


@pytest.mark.parametrize("kind", ["inline", "packed", "dedicated", "external"])
def test_lance_pixels_windows_and_decoder_lifetime(
    lance_connection, dataset, lance_dataset, kind
):
    import torch

    _blob_storage(lance_dataset, kind)
    # Remove the source MP4s so no accidental local-file fallback can succeed.
    for video in dataset.rglob("*.mp4"):
        video.unlink()
    deltas = [-0.1, 0, 0.1]
    expected = lance_connection.execute(
        f"""
        SELECT target_id, sha256(image) FROM lerobot_video_targets(?, (
            SELECT requests.*, row_number() OVER () - 1 AS target_id
            FROM ({REQUESTS}) AS requests
        ), delta_timestamps := ?) ORDER BY target_id
        """,
        [str(lance_dataset), str(lance_dataset), 1, deltas],
    ).fetchall()
    actual = []
    references = []
    with TorchCodecReader(
        lance_connection, lance_dataset, batch_size=5, max_cached_decoders=1
    ) as reader:
        sizes = []
        for batch in reader.batches(
            REQUESTS, [str(lance_dataset), 1], delta_timestamps=deltas
        ):
            sizes.append(len(batch.targets))
            assert len(reader._decoders) == 1
            references.extend(weakref.ref(d) for d in reader._decoders.values())
            for target, image, pts in zip(
                batch.targets, batch.images, batch.decoded_timestamps
            ):
                assert target.video_path.startswith("lance-blob://")
                assert image.dtype == torch.uint8
                assert abs(pts - target.video_timestamp) <= reader.tolerance
                pixels = image.permute(1, 2, 0).contiguous().numpy().tobytes()
                actual.append((target.target_id, hashlib.sha256(pixels).hexdigest()))
        assert sizes == [5, 5, 2]
        assert any(ref() is None for ref in references)
    assert actual == expected
    assert not reader._decoders
    assert all(ref() is None for ref in references)
    assert lance_connection.execute("SELECT 42").fetchone() == (42,)


def test_lance_blob_reads_preserve_large_pending_result(
    lance_connection, lance_dataset
):
    # More than six DuckDB vectors; each output batch reopens a cached blob
    # using connection.execute while the relation still has pending rows.
    query = """
        SELECT i AS request_id, 0 AS episode_index, (i % 4) AS frame_index,
               'front' AS video_key, 0 AS delta_index
        FROM range(12289) t(i) ORDER BY (i * 7919) % 12289, i
    """
    expected = lance_connection.execute(query).fetchall()
    with TorchCodecReader(lance_connection, lance_dataset, batch_size=257) as reader:
        actual = [target for batch in reader.batches(query) for target in batch.targets]
    assert [t.target_id for t in actual] == list(range(len(expected)))
    assert [(t.request_id, t.frame_index) for t in actual] == [
        (row[0], row[2]) for row in expected
    ]


def test_lance_size_limit_rejects_before_payload_read(lance_connection, lance_dataset):
    path = lance_connection.execute(
        "SELECT video_path FROM lerobot_video_routes(?, [0]) LIMIT 1",
        [str(lance_dataset)],
    ).fetchone()[0]
    with TorchCodecReader(lance_connection, lance_dataset, max_video_bytes=1) as reader:
        with pytest.raises(ValueError, match="exceeds max_video_bytes"):
            reader._decoder(path)
        assert not reader._decoders


def test_lance_cached_decoder_rechecks_access(lance_connection, lance_dataset):
    path = lance_connection.execute(
        "SELECT video_path FROM lerobot_video_routes(?, [0]) LIMIT 1",
        [str(lance_dataset)],
    ).fetchone()[0]
    with TorchCodecReader(lance_connection, lance_dataset) as reader:
        decoder = reader._decoder(path)
        assert reader._decoder(path) is decoder
        lance_connection.execute("SET enable_external_access=false")
        with pytest.raises(duckdb.Error, match="[Pp]ermission|[Dd]isabled|[Aa]ccess"):
            reader._decoder(path)


@pytest.mark.parametrize("cache", [False, True])
def test_lance_warm_decoder_secret_revocation(
    lance_connection, lance_dataset, monkeypatch, cache
):
    # Reuse the authenticated loopback fixture, which never logs credentials.
    monkeypatch.syspath_prepend(
        str(Path(__file__).resolve().parents[2] / "test/conformance")
    )
    from test_session_settings import s3_server, sql_string

    objects = {
        f"dataset/{file.relative_to(lance_dataset).as_posix()}": file.read_bytes()
        for file in lance_dataset.rglob("*")
        if file.is_file()
    }
    # V2 manifest names sort newest first. Supply reverse insertion order so
    # the fixture must honor S3's lexicographical listing contract.
    objects = dict(sorted(objects.items(), reverse=True))
    with s3_server(objects) as (endpoint, counts):
        secret = (
            "CREATE OR REPLACE SECRET reader_video_access (TYPE LANCE, "
            "SCOPE 's3://fixture-bucket/dataset/', ACCESS_KEY_ID 'fixture-session-id', "
            "SECRET_ACCESS_KEY 'fixture-session-key', REGION 'us-east-1', "
            f"ENDPOINT {sql_string('http://' + endpoint)}, ALLOW_HTTP true, "
            "VIRTUAL_HOSTED_STYLE_REQUEST false)"
        )
        # A denied parent scope makes missing secrets fail on loopback too.
        lance_connection.execute(
            secret.replace("reader_video_access", "reader_fallback")
            .replace("s3://fixture-bucket/dataset/", "s3://fixture-bucket/")
            .replace("fixture-session-id", "fixture-denied")
        )
        lance_connection.execute(secret)
        lance_connection.execute(f"SET enable_external_file_cache={str(cache).lower()}")
        remote = lance_connection.execute(
            "SELECT lance_blob_uri(?, 'video_bytes', _rowid) "
            "FROM 's3://fixture-bucket/dataset/videos.lance' LIMIT 1",
            ["s3://fixture-bucket/dataset/videos.lance"],
        ).fetchone()[0]
        with TorchCodecReader(lance_connection, lance_dataset) as reader:
            decoder = reader._decoder(remote)
            assert reader._decoder(remote) is decoder
            for change in (
                "DROP SECRET reader_video_access",
                secret.replace("fixture-session-id", "fixture-denied"),
            ):
                lance_connection.execute(change)
                with pytest.raises(duckdb.Error, match="403|[Ff]orbidden|[Aa]ccess"):
                    reader._decoder(remote)
                lance_connection.execute(secret)
                assert reader._decoder(remote) is decoder
        assert counts["accepted"] > 0
        assert counts["denied"] > 0


@pytest.mark.parametrize("cache", [False, True])
def test_lance_warm_decoder_payload_get_revocation(
    lance_connection, lance_dataset, monkeypatch, cache
):
    monkeypatch.syspath_prepend(
        str(Path(__file__).resolve().parents[2] / "test/conformance")
    )
    from test_session_settings import s3_server, sql_string

    _blob_storage(lance_dataset, "dedicated")
    objects = {
        f"dataset/{file.relative_to(lance_dataset).as_posix()}": file.read_bytes()
        for file in lance_dataset.rglob("*")
        if file.is_file()
    }
    payload_paths = {
        "/fixture-bucket/" + key for key in objects if key.endswith(".blob")
    }
    assert payload_paths
    revoked = False
    payload_gets = []

    def access(method, path, authorized):
        if method == "GET" and path in payload_paths:
            payload_gets.append(path)
            return authorized and not revoked
        return authorized

    with s3_server(objects, request_access=access) as (endpoint, counts):
        lance_connection.execute(
            "CREATE SECRET reader_payload_access (TYPE LANCE, "
            "SCOPE 's3://fixture-bucket/dataset/', ACCESS_KEY_ID 'fixture-session-id', "
            "SECRET_ACCESS_KEY 'fixture-session-key', REGION 'us-east-1', "
            f"ENDPOINT {sql_string('http://' + endpoint)}, ALLOW_HTTP true, "
            "VIRTUAL_HOSTED_STYLE_REQUEST false)"
        )
        lance_connection.execute(f"SET enable_external_file_cache={str(cache).lower()}")
        remote = lance_connection.execute(
            "SELECT lance_blob_uri(?, 'video_bytes', _rowid) "
            "FROM 's3://fixture-bucket/dataset/videos.lance' LIMIT 1",
            ["s3://fixture-bucket/dataset/videos.lance"],
        ).fetchone()[0]
        with TorchCodecReader(lance_connection, lance_dataset) as reader:
            decoder = reader._decoder(remote)
            assert payload_gets
            revoked = True
            payload_gets.clear()
            # HEAD and all manifest/descriptor reads remain authorized.
            assert (
                lance_connection.execute(
                    "SELECT size FROM read_blob(?)", [remote]
                ).fetchone()[0]
                > 0
            )
            assert not payload_gets
            with pytest.raises(duckdb.Error, match="403|[Ff]orbidden|[Aa]ccess"):
                reader._decoder(remote)
            assert payload_gets and counts["denied"] > 0
            revoked = False
            assert reader._decoder(remote) is decoder


def test_lance_decode_failure_releases_result(lance_connection, lance_dataset):
    lance = pytest.importorskip("lance")
    pa = pytest.importorskip("pyarrow")
    from lance.blob import BlobArray

    path = lance_dataset / "videos.lance"
    rows = (
        lance.dataset(path)
        .to_table(columns=["video_key", "chunk_index", "file_index", "file_size"])
        .to_pylist()
    )
    bad = b"invalid video"
    rows = [dict(row, file_size=len(bad)) for row in rows]
    blobs = BlobArray.from_pylist([bad] * len(rows))
    table = pa.Table.from_pylist(rows).append_column(
        pa.field("video_bytes", blobs.type), blobs
    )
    shutil.rmtree(path)
    lance.write_dataset(table, path, data_storage_version="2.2")
    with TorchCodecReader(lance_connection, lance_dataset) as reader:
        with pytest.raises(RuntimeError):
            list(reader.batches("""
                SELECT 0 AS request_id, 0 AS episode_index, 0 AS frame_index,
                       'front' AS video_key, 0 AS delta_index
            """))
        assert not reader._active
        assert not reader._decoders
        assert list(reader.batches("""
            SELECT 0 AS request_id, 0 AS episode_index, 0 AS frame_index,
                   'front' AS video_key, 0 AS delta_index WHERE false
        """)) == []
    assert lance_connection.execute("SELECT 42").fetchone() == (42,)
