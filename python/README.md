# DuckDB LeRobot → TorchCodec

Optional Python adapter: select frames/windows with SQL, then decode uint8 CHW
Torch tensors in batches. The C++ extension does not depend on PyTorch.

## Install

Build the extension using the [repository instructions](../README.md). Install
PyTorch, a [compatible TorchCodec version](https://github.com/meta-pytorch/torchcodec#compatibility-with-torch-versions)
and shared FFmpeg libraries for your platform, then from the repository:

```bash
pip install ./python
# Alternatively, let pip resolve Torch/TorchCodec as well:
pip install './python[torchcodec]'
```

The DuckDB Python version must match the extension build (currently 1.5.5).

## SQL → Tensor

```python
import duckdb
import torch
from duckdb_lerobot import TorchCodecReader

dataset = "/absolute/path/to/local_dataset"
con = duckdb.connect(config={"allow_unsigned_extensions": True})
con.execute("LOAD 'build/release/extension/lerobot/lerobot.duckdb_extension'")

requests = """
    SELECT frame_index AS request_id, episode_index, frame_index,
           'observation.images.front' AS video_key, delta_index
    FROM lerobot_scan(?)
    CROSS JOIN range(3) AS offsets(delta_index)
    WHERE episode_index = ? AND frame_index BETWEEN 0 AND 15
    ORDER BY frame_index, delta_index
"""

# Example for a 10 FPS dataset; offsets must align with its actual FPS.
with TorchCodecReader(con, dataset, batch_size=32, device="cpu") as reader:
    for batch in reader.batches(
        requests, [dataset, 0], delta_timestamps=[-0.1, 0.0, 0.1]
    ):
        images = torch.stack(batch.images)  # [rows, 3, H, W], uint8
        padding = [target.is_padding for target in batch.targets]
        # Pass images and padding to your training pipeline.
con.close()
```

Each SQL row selects one camera and one delta. Output preserves duplicates and
request order, including padded targets. Different camera sizes are supported;
stack only tensors with the same shape. Images are views: clone before in-place
transforms because duplicate targets may share storage.

Use `device="cuda"` for supported CUDA hardware/codecs. CUDA is opt-in; the CPU
path is tested. Create a separate connection/reader per worker and avoid CUDA
decoding in forked workers. The reader keeps at most `max_cached_decoders` video
decoders (default 4); `batch_size` limits output rows, not total process memory.

This first version reads trusted local, constant-frame-rate RGB videos at their
original size. It checks decoded timestamps against `tolerance` (default 1e-4 s)
and rejects misalignment. Download remote datasets first. Python opens videos
directly; DuckDB filesystem credentials and sandbox rules do not apply. Restricted
DuckDB connections (`enable_external_access=false`) are rejected.

Use a dedicated connection during iteration. Close a partially consumed iterator
before closing the reader; `contextlib.closing(reader.batches(...))` is useful
when breaking early. `reader.close()` releases cached decoders, not the connection.

## Verify

```bash
pip install './python[test]'
LEROBOT_EXTENSION="$PWD/build/release/extension/lerobot/lerobot.duckdb_extension" \
  pytest python/tests
```

Compare both complete CPU paths through contiguous Torch batches:

```bash
python benchmark/torchcodec_pipeline.py \
  --dataset /absolute/path/to/local_dataset \
  --extension build/release/extension/lerobot/lerobot.duckdb_extension \
  --camera observation.images.front --rows 16 100 \
  --output build/torchcodec-pipeline.json
```

The adapter source is Apache-2.0. Optional dependencies retain their own licenses;
TorchCodec video decoding still uses FFmpeg.
