"""Run the pinned defect checks on every extension C++ translation unit."""

import argparse
import concurrent.futures
import json
import re
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--clang-tidy", default="clang-tidy-18")
    parser.add_argument("--jobs", default=2, type=int)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    version = subprocess.check_output([args.clang_tidy, "--version"], text=True)
    if not re.search(r"LLVM version 18\.", version):
        parser.error("clang-tidy 18 is required")
    root = Path(__file__).resolve().parents[2]
    build = args.build_dir.resolve()
    commands = json.loads((build / "compile_commands.json").read_text())
    sources = set((root / "src").rglob("*.cpp"))
    selected = {}
    for entry in commands:
        source = (Path(entry["directory"]) / entry["file"]).resolve()
        # Both static and loadable extension commands exist. Analyze once,
        # using the static target's real FFmpeg-enabled compiler arguments.
        command = entry.get("command", " ".join(entry.get("arguments", [])))
        if source in sources and "lerobot_extension.dir/" in command:
            selected[source] = entry
    missing = sources - selected.keys()
    if not sources or missing:
        raise RuntimeError(f"Missing extension compilation units: {sorted(map(str, missing))}")
    output = build / "tidy"
    output.mkdir(exist_ok=True)
    (output / "compile_commands.json").write_text(json.dumps(list(selected.values())))
    header_filter = "^" + re.escape(str(root / "src")) + "/"

    def check(source):
        result = subprocess.run(
            [
                args.clang_tidy,
                str(source),
                f"-p={output}",
                f"--config-file={root / '.clang-tidy'}",
                f"--header-filter={header_filter}",
                "--quiet",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=600,
        )
        (output / (source.stem + ".log")).write_text(result.stdout)
        print(f"{source.relative_to(root)}: {'PASS' if result.returncode == 0 else 'FAIL'}", flush=True)
        if result.returncode:
            print(result.stdout, flush=True)
        return result.returncode == 0

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        passed = list(pool.map(check, sorted(selected)))
    if not all(passed):
        raise SystemExit(1)
    print(f"Checked all {len(passed)} extension C++ translation units.")


if __name__ == "__main__":
    main()
