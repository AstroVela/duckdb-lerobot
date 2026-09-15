#!/usr/bin/env python3
"""Install the locked Linux x86_64 / Python 3.12 CPU benchmark environments."""

import argparse
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env-root", type=Path, default=Path("build/benchmarks/envs"))
    parser.add_argument(
        "--engines",
        nargs="+",
        choices=["duckdb", "daft", "lerobot", "torchcodec"],
        default=["duckdb", "daft", "lerobot", "torchcodec"],
    )
    args = parser.parse_args()
    env = dict(os.environ, PYTHONNOUSERSITE="1")
    env.pop("PYTHONPATH", None)
    for engine in args.engines:
        destination = args.env_root.resolve() / engine
        subprocess.run(
            ["uv", "venv", "--python", "3.12", str(destination)], check=True, env=env
        )
        python = destination / "bin/python"
        subprocess.run(
            [
                "uv",
                "pip",
                "sync",
                "--python",
                str(python),
                "--index",
                "https://download.pytorch.org/whl/cpu",
                "--index-strategy",
                "unsafe-best-match",
                str(ROOT / "requirements" / f"{engine}.txt"),
            ],
            check=True,
            env=env,
        )
        if engine == "torchcodec":
            subprocess.run(
                [
                    "uv",
                    "pip",
                    "install",
                    "--python",
                    str(python),
                    "--no-deps",
                    str(ROOT.parent / "python"),
                ],
                check=True,
                env=env,
            )


if __name__ == "__main__":
    main()
