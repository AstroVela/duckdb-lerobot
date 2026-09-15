import hashlib
import json
import os

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
        SELECT target_ordinal, request_id, target_frame_index, video_key, is_padding,
               height, width, sha256(image), video_timestamp
        FROM lerobot_video_targets(?, ({REQUESTS}), delta_timestamps := ?)
        ORDER BY target_ordinal
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
                        target.target_ordinal,
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
            list(
                reader.batches("""
                SELECT 1 AS request_id, 0 AS episode_index, 0 AS frame_index,
                       'front' AS video_key, 0 AS delta_index
            """)
            )


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
            list(
                reader.batches("""
                SELECT 0 AS request_id, 0 AS episode_index, 0 AS frame_index,
                       'front' AS video_key, 0 AS delta_index
            """)
            )


@pytest.mark.parametrize(
    "option,value",
    [
        ("batch_size", 0),
        ("batch_size", True),
        ("max_cached_decoders", -1),
        ("num_ffmpeg_threads", 0),
        ("tolerance", float("nan")),
        ("device", "mps"),
    ],
)
def test_invalid_options(connection, dataset, option, value):
    with pytest.raises(ValueError):
        TorchCodecReader(connection, dataset, **{option: value})
