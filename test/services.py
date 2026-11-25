#!/usr/bin/env python3
"""Verify optional OpenRC service snapshots and whole-cgroup process memory."""

import argparse
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile

from verify import fnv, frames

ROOT = Path(__file__).resolve().parent.parent


def write(root: Path, name: str, value: str) -> None:
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value)


def receive(peer: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = peer.recv(size - len(data))
        if not chunk:
            raise AssertionError("D-Bus peer disconnected")
        data.extend(chunk)
    return bytes(data)


def bus_string(value: bytes) -> bytes:
    return struct.pack("<I", len(value)) + value + b"\0"


def check_systemd(command: list[str], root: Path, truncated: int = 0) -> None:
    peer, probe = socket.socketpair()
    with peer, probe:
        peer.settimeout(5)
        probe.setblocking(False)
        process = subprocess.Popen(
            command + [str(root), str(probe.fileno()), "truncated" if truncated else "valid"],
            pass_fds=(probe.fileno(),), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        probe.close()
        try:
            bodies = [bus_string(b"/unit"), b"\x01s\0\0" + bus_string(b"inactive"),
                      b"\x01t\0" + b"\0" * 5 + struct.pack("<Q", 987654),
                      b"\x01s\0\0" + bus_string(b"/openrc.worker")]
            for index, body in enumerate(bodies):
                fixed = receive(peer, 16)
                body_bytes, serial, header_bytes = struct.unpack_from("<III", fixed, 4)
                body_at = (16 + header_bytes + 7) & ~7
                request = fixed + receive(peer, body_at + body_bytes - 16)
                if index == 3 and not truncated:
                    interface_bytes = struct.unpack_from("<I", request, body_at)[0]
                    assert request[body_at + 4:body_at + 4 + interface_bytes] == b"org.freedesktop.systemd1.Service"
                header = b"\x05\x01u\0" + struct.pack("<I", serial)
                if index == 3 and truncated:
                    # Put a valid string header at the scratch buffer's boundary.
                    header += b"\x06\x01s\0" + bus_string(b"x" * 4051)
                    body = (b"\x01s\0\0" + struct.pack("<I", 2))[:truncated]
                reply = b"l\x02\0\x01" + struct.pack("<III", len(body), index + 1, len(header))
                reply += header
                reply += b"\0" * (-len(reply) % 8) + body
                peer.sendall(reply)
            stdout, stderr = process.communicate(timeout=5)
            assert process.returncode == 0, stderr.decode()
            assert not stdout and not stderr, (stdout, stderr)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("native", "arm64", "armv6"), default="native")
    parser.add_argument("--linker", help="path to LLD")
    parser.add_argument("--sanitize", action="store_true", help="enable native address/undefined sanitizers")
    args = parser.parse_args()
    if args.sanitize and args.target != "native":
        parser.error("--sanitize requires --target native")
    compiler = ["cc"]
    runner = []
    flags = ["-Os", "-std=c11", "-Wall", "-Wextra", "-Werror", "-ffreestanding", "-fno-builtin"]
    if args.sanitize:
        flags += ["-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    sources = [str(ROOT / "test/services.c"), str(ROOT / "vendor/monocypher/monocypher.c"),
               str(ROOT / "vendor/monocypher/monocypher-ed25519.c")]
    linking = []
    if args.target != "native":
        compiler = ["clang", "--ld-path=" + args.linker] if args.linker else ["clang", "-fuse-ld=lld"]
        if args.target == "armv6":
            compiler += ["--target=arm-linux-gnueabi", "-march=armv6", "-marm", "-mfloat-abi=soft"]
            runner = ["qemu-arm", "-cpu", "arm1176"]
        else:
            compiler += ["--target=aarch64-linux-gnu"]
            runner = ["qemu-aarch64"]
        flags += ["-DQUECTOP_TEST_FREESTANDING", "-fno-stack-protector", "-fno-pie",
                  "-ffunction-sections", "-fdata-sections"]
        sources += [str(ROOT / "boot.c"), str(ROOT / "arm.c"), str(ROOT / "start.S")]
        linking = ["-nostdlib", "-static", "-Wl,-no-pie,--gc-sections,--build-id=none,-z,noexecstack"]
    with tempfile.TemporaryDirectory(prefix="quectop-services-") as temporary:
        root = Path(temporary)
        binary = root / "services"
        subprocess.run(compiler + flags + sources + linking + ["-o", str(binary)], check=True)
        write(root, "openrc/started/worker", "")
        write(root, "openrc/options/worker/child_pid", "101\n")
        write(root, "cgroup/openrc.worker/memory.current", "987654\n")
        write(root, "cgroup/openrc.worker/cgroup.procs", "101\n102\n")
        write(root, "cgroup/openrc.worker/children/cgroup.procs", "102\n103\n")
        for pid in (101, 102, 103, 104):
            write(root, f"proc/{pid}/stat", f"{pid} (worker ) name) S " + "0 " * 18 + "100\n")
        rollup = ("Rss: 400 kB\nPss: 300 kB\nPrivate_Clean: 50 kB\n"
                  "Private_Dirty: 150 kB\nPss_Anon: 150 kB\nPss_File: 100 kB\nPss_Shmem: 50 kB\n")
        for pid in (101, 102, 103):
            write(root, f"proc/{pid}/smaps_rollup", rollup)
        write(root, "cgroup/openrc.denied/cgroup.procs", "104\n")
        write(root, "proc/104/status", "Name:\tworker\nVmRSS:\t123 kB\n")
        write(root, "cgroup/openrc.partial/cgroup.procs", "101\n999\n")
        write(root, "cgroup/openrc.empty/cgroup.procs", "")
        write(root, "cgroup/openrc.malformed/cgroup.procs", "abc\n")
        write(root, "cgroup/openrc.hugepid/cgroup.procs", "4294967296\n")
        write(root, "cgroup/openrc.truncated/cgroup.procs", "101")
        for depth in range(18):
            write(root, "cgroup/openrc.depth/" + "child/" * depth + "cgroup.procs", "")
        for truncated in range(3, 9):
            check_systemd(runner + [str(binary)], root, truncated)
        check_systemd(runner + [str(binary)], root)
        result = subprocess.run(runner + [str(binary), str(root)],
                                capture_output=True, timeout=15)
        assert result.returncode == 0, result.stderr.decode()
        core = root / "core"
        subprocess.run(compiler + flags + [str(ROOT / "test/core.c")] + sources[1:] +
                       linking + ["-o", str(core)], check=True)
        fixture = root / "fixture"
        fixture.mkdir()
        inputs = frames(fixture, [0])
        subprocess.run(runner + [str(core), str(fixture), *map(str, inputs)], check=True)
        snapshot = fixture / "live.v1.bin"
        live = bytearray(snapshot.read_bytes()[:-4] + result.stdout)
        struct.pack_into("<I", live, 132, 1)
        snapshot.write_bytes(live + struct.pack("<I", fnv(live)))
        (fixture / "service-entry.bin").write_bytes(result.stdout)
        subprocess.run(["bun", str(ROOT / "test/services.js"), str(fixture)], cwd=ROOT, check=True)
        subprocess.run(["bun", str(ROOT / "test/frontend.js"), str(fixture)], cwd=ROOT,
                       env=dict(os.environ, QUECTOP_TEST_SERVICES="1"), check=True)
    print(f"{args.target}: systemd framing, OpenRC states, descendant totals, duplicate PIDs, missing/partial data and RSS fallback passed")


if __name__ == "__main__":
    main()
