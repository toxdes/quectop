#!/usr/bin/env python3
"""Run libc-free ABI checks natively or under ARM user-mode QEMU."""

import argparse
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("native", "arm64", "armv6"), default="native")
    parser.add_argument("--linker", help="path to LLD")
    args = parser.parse_args()
    compiler = ["clang", "--ld-path=" + args.linker] if args.linker else ["clang", "-fuse-ld=lld"]
    runners = [[]]
    if args.target == "armv6":
        compiler += ["--target=arm-linux-gnueabi", "-march=armv6", "-marm", "-mfloat-abi=soft"]
        runners = [["qemu-arm", "-cpu", cpu] for cpu in ("arm1176", "cortex-a7")]
    elif args.target == "arm64":
        compiler += ["--target=aarch64-linux-gnu"]
        runners = [["qemu-aarch64"]]
    with tempfile.TemporaryDirectory(prefix="quectop-abi-") as temporary:
        directory = Path(temporary)
        binary = directory / "abi"
        subprocess.run(compiler + [
            "-Os", "-std=c11", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
            "-fno-builtin", "-fno-stack-protector", "-fno-unwind-tables",
            "-fno-asynchronous-unwind-tables", "-fno-pie", "-ffunction-sections",
            "-fdata-sections", str(ROOT / "test/abi.c"), str(ROOT / "boot.c"),
            str(ROOT / "vendor/monocypher/monocypher.c"), str(ROOT / "vendor/monocypher/monocypher-ed25519.c"), str(ROOT / "arm.c"),
            str(ROOT / "start.S"), "-nostdlib", "-static",
            "-Wl,-no-pie,--gc-sections,--build-id=none,-z,noexecstack,-s", "-o", str(binary),
        ], check=True)
        boundary = [0, 1, 2, 3, 7, 255, 256, 65535, 65536,
                    (1 << 31) - 1, 1 << 31, (1 << 32) - 1, 1 << 32,
                    (1 << 32) + 1, (1 << 63) - 1, 1 << 63, (1 << 64) - 1]
        pairs = [(n, d) for n in boundary for d in boundary if d]
        rng = random.Random(0xA6EA)
        pairs += [(rng.getrandbits(width), rng.getrandbits(rng.choice((8, 16, 32, 64))) or 1)
                  for width in (32, 64) for _ in range(1024)]
        vectors = directory / "division.bin"
        vectors.write_bytes(b"".join(struct.pack("<4Q", n, d, n // d, n % d) for n, d in pairs))
        for index, runner in enumerate(runners):
            data = directory / str(index)
            data.mkdir()
            subprocess.run(runner + [str(binary), str(data), str(vectors)], check=True, timeout=30)
            actual = struct.unpack("<6Q", (data / "abi.fs.bin").read_bytes())
            expected = os.statvfs(data)
            assert actual[0] == expected.f_bsize
            assert actual[1] == expected.f_blocks
            assert actual[4] == expected.f_files
            assert actual[3] <= actual[2] <= actual[1]
            assert actual[5] <= actual[4]
            assert (data / "abi.sparse").stat().st_size == (1 << 33) + 1234
        print(f"{args.target}: {len(pairs)} division vectors; large files, filesystem layout, clocks, poll and mmap passed")


if __name__ == "__main__":
    main()
