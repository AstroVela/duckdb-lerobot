"""Generate original, deterministic seeds; no third-party dataset inputs."""

import argparse
import hashlib
from pathlib import Path


def seeds():
    for path in (
        "",
        ".",
        "..",
        "../data",
        "data/../secret",
        "data//file",
        "data/file.parquet",
        "data/chunk-000/file-001.parquet",
        "camera.front",
        "C:\\data",
        "\\\\server\\share",
        "..\\secret",
        "https://example.invalid/a",
        "%2e%2e/file",
        "NUL.txt",
        "COM¹",
        "CONOUT$",
        "camera ",
        "camera.",
        "a\x00b",
        "a\nb",
        "./ ",
        "/",
        " / ",
    ):
        yield bytes([0, 0, 0]) + path.encode()
    numbers = (
        "0",
        "1",
        "-1",
        "30",
        "0.5",
        "0.01",
        "10",
        "3.5",
        "true",
        "false",
        "NaN",
        "Infinity",
        "-Infinity",
        "1e-300",
        "1e300",
        "9223372036854775807",
        "9223372036854775808",
        "9007199254740993",
        "18446744073709551615",
        "18446744073709551616",
        "-9223372036854775808",
        "32768",
        "32769",
    )
    for mode, count in ((1, 17), (3, 12)):
        for option in range(count):
            for kind in range(6):
                for number in numbers:
                    yield bytes([mode, option, kind]) + number.encode()
    for option in (0, 1):
        for kind in range(6):
            for number in numbers:
                yield bytes([3, option | 0x80, kind]) + number.encode()
    for codec in ("libaom-av1", "libsvtav1", "invalid"):
        yield bytes([1, 8, 5]) + codec.encode()
    for fps in (0, 29, 59, 255):
        for value in ("0.5", "1.5", "-0.5", "-1.5", "9.223372036854776e18", "-9.223372036854776e18"):
            yield bytes([3, fps, 3]) + value.encode()
    for schema, features in (
        (0, "{}"),
        (1, '{"action":{"dtype":"float32","shape":[1]}}'),
        (1, '{"action":{"dtype":"float32","shape":[null]}}'),
        (1, '{"action":{"dtype":"float32","shape":[0]}}'),
        (1, '{"action":{"dtype":"float32","shape":[100000]}}'),
        (1, '{"action":{"dtype":"float32","shape":[100001]}}'),
        (1, '{"action":{"dtype":"float32","shape":[1,100001]}}'),
        (1, '{"action":{"dtype":"float32","shape":[100001,1]}}'),
        (1, '{"action":{"dtype":"string","shape":[100001]}}'),
        (1, '{"action":{"dtype":"float32","shape":[9223372036854775807,8]}}'),
        (1, '{"action":{"dtype":"float32","shape":[1]},"Action":{"dtype":"float32","shape":[1]}}'),
        (2, '{"camera":{"dtype":"video","shape":[16,16,3]}}'),
        (2, '{"camera":{"dtype":"image","shape":[16,16,3]}}'),
        (2, '{"camera":{"dtype":"video","shape":[16,16,1],"info":{"is_depth_map":true}}}'),
        (0, '{"..\\\\secret":{"dtype":"video","shape":[16,16,3]}}'),
        (0, "{"),
        (0, "null"),
        (0, "[]"),
    ):
        yield bytes([2, schema, 0]) + features.encode()
    for regression in sorted((Path(__file__).parent / "regressions").glob("*.bin")):
        yield regression.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for seed in seeds():
        if not 3 <= len(seed) <= 4096:
            raise ValueError("Seed must fit the harness's 3..4096 byte input contract")
        (args.output / hashlib.sha256(seed).hexdigest()).write_bytes(seed)


if __name__ == "__main__":
    main()
