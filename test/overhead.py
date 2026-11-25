#!/usr/bin/env python3
"""Check refactor call overhead in optimized AMD64 and ARM assembly."""

import argparse
from pathlib import Path
import re
import shlex
import subprocess


ROOT = Path(__file__).resolve().parent.parent
ACCESSORS = ("service_reply_string", "service_property_string", "service_property_counter")
CALL = r"\b(?:call|bl|b|jmp)\s+{}(?:\.[\w.]+)?(?:\s|$)"


def check_assembly(path: Path) -> None:
    source = path.read_text()
    for name in ACCESSORS:
        assert not re.search(r"^" + name + r"(?:\.[\w.]+)?:", source, re.MULTILINE), name
        assert not re.search(CALL.format(name), source, re.MULTILINE), name
    # Only frequency-path construction needs the forward decimal formatter.
    assert len(re.findall(CALL.format("decimal"), source, re.MULTILINE)) <= 1
    observer = re.search(r"^observe_systemd:.*?\.size\s+observe_systemd,", source,
                         re.MULTILINE | re.DOTALL)
    if observer:
        assert not re.search(CALL.format("length"), observer.group()), "unit-path rescan"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=Path("/tmp/quectop-overhead"))
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    flags = next(line.split("=", 1)[1] for line in (ROOT / "Makefile").read_text().splitlines()
                 if line.startswith("CFLAGS ="))
    targets = {
        "amd64": ["cc"],
        "arm64": ["clang", "--target=aarch64-linux-gnu"],
        "armv6": ["clang", "--target=arm-linux-gnueabi", "-march=armv6", "-marm", "-mfloat-abi=soft"],
    }
    for name, compiler in targets.items():
        assembly = args.output_dir / (name + ".s")
        command = compiler + shlex.split(flags) + ["-S", "main.c", "-o", str(assembly)]
        with (args.output_dir / (name + ".log")).open("w") as log:
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        check_assembly(assembly)
        print(f"{name}: checked accessors inlined; no extra PID formatter calls or unit-path scan")


if __name__ == "__main__":
    main()
