#!/usr/bin/env python3
"""Check the startup and RELRO layout of both release binaries."""

import argparse
from contextlib import contextmanager
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
from typing import NamedTuple

from verify import wait_live


MACHINES = {"amd64": "Advanced Micro Devices X86-64", "arm64": "AArch64", "armv6": "ARM"}
PAGE_SIZES = (4096, 16384, 65536)


class Segment(NamedTuple):
    offset: int
    address: int
    file_bytes: int
    memory_bytes: int
    align: int


def inspect_elf(path: Path, target: str):
    elf = subprocess.check_output(["readelf", "-hWlSd", path], text=True,
                                  env=dict(os.environ, LC_ALL="C"))
    machine = re.search(r"^\s*Machine:\s*(.+)$", elf, re.MULTILINE)
    assert machine and machine[1] == MACHINES[target], path
    assert re.search(r"^\s*Type:\s+DYN\b", elf, re.MULTILINE), f"{path} must be PIE"
    assert not re.search(r"^\s*INTERP\b|\(NEEDED\)", elf, re.MULTILINE), f"{path} must be static"
    segments = {"LOAD": [], "GNU_RELRO": []}
    for line in elf.splitlines():
        fields = line.split()
        if fields and fields[0] in segments:
            segments[fields[0]].append(Segment(*(int(fields[i], 16) for i in (1, 2, 4, 5, -1))))
    assert segments["LOAD"] and segments["LOAD"][0].offset == 0, path
    assert len(segments["GNU_RELRO"]) == 1, f"{path} must have one RELRO segment"
    relro, = segments["GNU_RELRO"]
    writable = [(section, int(start, 16), int(size, 16)) for section, start, size in re.findall(
        r"^\s*\[\s*\d+\]\s+(\.data|\.bss)\s+\S+\s+(\w+)\s+\w+\s+(\w+)", elf, re.MULTILINE)]
    assert any(section == ".bss" for section, _, _ in writable), path
    for page in (PAGE_SIZES if target == "arm64" else (4096,)):
        start, end = relro_range(relro, page)
        assert start < end and relro.address + relro.file_bytes <= end, (path, page, relro)
        for load in segments["LOAD"]:
            assert load.align >= page and (load.address - load.offset) % page == 0, (path, page, load)
        for section, address, size in writable:
            assert address >= end or address + size <= start, (path, page, section)
    return segments["LOAD"][0].address, relro, writable


def relro_range(relro: Segment, page: int) -> tuple[int, int]:
    return relro.address & -page, (relro.address + relro.memory_bytes) & -page


def check_mappings(pid: int, path: Path, layout, page: int) -> None:
    maps = []
    for line in Path(f"/proc/{pid}/maps").read_text().splitlines():
        area, perms, offset, _, _, *file = line.split(maxsplit=5)
        start, end = (int(bound, 16) for bound in area.split("-"))
        maps.append((start, end, perms, int(offset, 16), file))
    first_load, relro, writable = layout
    base = min(start for start, _, _, offset, file in maps if not offset and file == [str(path)])
    base -= first_load & -page

    def require_permissions(start: int, end: int, write: bool) -> None:
        cursor = start
        for left, right, perms, _, _ in maps:
            if right <= cursor:
                continue
            if left > cursor or perms[0] != "r" or (perms[1] == "w") != write:
                break
            cursor = min(right, end)
            if cursor == end:
                return
        raise AssertionError(f"{path} has incorrect permissions at {cursor:#x}")

    start, end = relro_range(relro, page)
    require_permissions(base + start, base + end, False)
    for _, start, size in writable:
        if size:
            require_permissions(base + start, base + start + size, True)


@contextmanager
def running(command):
    with subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE) as process:
        try:
            yield process
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def first_port() -> int:
    with socket.socket() as sock:
        for port in range(3000, 65536):
            try:
                sock.bind(("127.0.0.1", port))
            except OSError:
                continue
            return port
    raise RuntimeError("no free loopback port")


def check_replication(probe: Path, hub: Path, layouts: dict, page: int) -> None:
    with tempfile.TemporaryDirectory(prefix="quectop-startup-") as temporary:
        root = Path(temporary)
        keys = root / "keys"
        keys.write_text("startup-fixture " + "01" * 32 + "\n")
        keys.chmod(0o600)
        address = f"127.0.0.1:{first_port()}"
        data = root / "hub"
        with running([hub, "--data-dir", data, "--keys", keys, "--probes", address]) as server:
            with running([probe, "--data-dir", root / "probe", "--hub", address,
                          "--key-file", keys, "--interface", "lo", "--points", "16",
                          "--interval-ms", "100", "--publish-ms", "200"]) as client:
                replica = data / "h" / "startup-fixture" / "live.v1.bin"
                first = wait_live(server, replica)
                generated, = struct.unpack_from("<Q", first, 24)
                wait_live(server, replica, generated + 1)
                check_mappings(client.pid, probe, layouts[probe], page)
                check_mappings(server.pid, hub, layouts[hub], page)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="directory containing the two release binaries")
    parser.add_argument("--target", choices=MACHINES, required=True)
    parser.add_argument("--page-size", type=int, choices=PAGE_SIZES,
                        help="check the actual kernel page size, live replication, and mappings")
    args = parser.parse_args()
    host = os.uname().machine
    native = (host == "x86_64" and args.target == "amd64" or
              host == "aarch64" and args.target == "arm64" or
              host.startswith(("armv6", "armv7", "armv8l")) and args.target == "armv6")
    if args.page_size is not None and (not native or os.sysconf("SC_PAGE_SIZE") != args.page_size):
        parser.error("--page-size requires a native kernel with the requested page size")
    if native:
        runner = []
    elif args.target == "arm64":
        runner = ["qemu-aarch64"]
    elif args.target == "armv6":
        runner = ["qemu-arm", "-cpu", "arm1176"]
    else:
        parser.error("AMD64 startup requires an AMD64 host")
    probe, hub = [(args.directory / name).resolve(strict=True) for name in ("quectop", "quectop-hub")]
    layouts = {}
    for binary in (probe, hub):
        layouts[binary] = inspect_elf(binary, args.target)
        for flag in ("--help", "--version"):
            result = subprocess.run([*runner, binary, flag], check=True, timeout=10, capture_output=True)
            assert result.stdout.startswith(binary.name.encode() + b" ") and not result.stderr, binary
        print(f"{args.target} {binary.name} static PIE layout and startup passed")
    if args.page_size is not None:
        check_replication(probe, hub, layouts, args.page_size)
        print(f"{args.target} {args.page_size}-byte pages, replication and RELRO mappings passed")


if __name__ == "__main__":
    main()
