#!/usr/bin/env python3
"""Check CPU topologies, five packed histories, precision, spikes and execution."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SPANS = (3600, 86400, 604800, 2592000, 7776000)
FILES = ('1h.v7.bin', '1d.v7.bin', '7d.v7.bin', '30d.v7.bin', '90d.v7.bin')


def fnv(data, value=2166136261):
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def reader(data):
    packed = int.from_bytes(data, 'little')

    def read(bits):
        nonlocal packed
        value = packed & ((1 << bits) - 1)
        packed >>= bits
        return value
    return read


def unpack_record(data, width, cores, frequencies, frequency_bits, interfaces):
    if not any(data[:4]):
        return None
    assert fnv(data[:-4]) == struct.unpack_from('<I', data, len(data) - 4)[0]
    read = reader(data[:-4])
    start = read(32)
    widths = (10,) * (cores + 5) + (12,) + (frequency_bits,) * frequencies + (14,) * (2 * interfaces)
    valid = [read(1) for _ in widths]
    values = [[read(bits) for bits in widths] for _ in range(3)]
    peaks = [read(max(1, (width - 1).bit_length())) for _ in widths]
    assert all(valid)
    assert all(0 <= offset < width for offset in peaks)
    return start, values, peaks


def wait_live(process, path, minimum_age=0):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(process.stderr.read().decode())
        if path.exists():
            data = path.read_bytes()
            if len(data) >= 148 and fnv(data[:-4]) == struct.unpack_from('<I', data, len(data) - 4)[0]:
                if struct.unpack_from('<Q', data, 24)[0] >= minimum_age:
                    return data
        time.sleep(0.02)
    raise AssertionError('collector did not publish valid live data')


def frames(directory, ids):
    baseline = [[1000, 0, 1000, 1000, 0, 0, 0, 0] for _ in ids]
    deltas = [[90, 0, 0, 10, 0, 0, 0, 0] if i % 2 == 0
              else [0, 0, 10, 890, 0, 0, 0, 0] for i in range(len(ids))]
    paths = []
    for step in range(4):
        counters = [[a + step * b for a, b in zip(base, delta)]
                    for base, delta in zip(baseline, deltas)]
        aggregate = [sum(row[k] for row in counters) for k in range(8)]
        # A CPU can disappear while aggregate counters retain its earlier ticks.
        paths.append(directory / f'cpu-{step}')
        lines = ['cpu  ' + ' '.join(map(str, aggregate))]
        lines += [f'cpu{identifier} ' + ' '.join(map(str, row))
                  for i, (identifier, row) in enumerate(zip(ids, counters))
                  if not (step == 2 and i == len(ids) - 1)]
        lines += ['intr ' + '0 ' * 6000]  # Large non-CPU row must not grow scratch RAM.
        paths[-1].write_text('\n'.join(lines) + '\n')
    return paths


CODEC_CHECK = r'''
const fs=require('fs');
const source=fs.readFileSync('web/app.js','utf8');
const spans=[3600,86400,604800,2592000,7776000];
const api=new Function('spans','skipped','schemaVersion','diskSchemaVersion',source.slice(source.indexOf('function magic'),source.indexOf('function configurePlots'))+';return {decodeLive,decodeHistory};')(spans,0,7,2);
const read=name=>{const b=fs.readFileSync(process.argv.at(-1)+'/'+name);return b.buffer.slice(b.byteOffset,b.byteOffset+b.byteLength);};
const live=api.decodeLive(read('live.v7.bin')),n=live.cores;
if(live.latest[n+5]!==54.2||live.interfaces&&(live.latest[live.format.network]!==8500000||live.latest[live.format.network+1]!==100000))throw Error('precision');
for(let i=0;i<5;i++){
 const name=['1h.v7.bin','1d.v7.bin','7d.v7.bin','30d.v7.bin','90d.v7.bin'][i];
 const rows=api.decodeHistory(read(name),i,live);
 const combined=rows.concat(live.active[i]?[live.active[i]]:[],i===0?live.pending:[]);
 const spike=combined.find(r=>r.max[0]===100&&r.peak[0]===1700019050000);
 if(!spike)throw Error('spike or peak time lost: '+name);
 if(!rows.length)throw Error('no committed records');
 const bad=read(name),bytes=new Uint8Array(bad),at=64+live.metadataBytes+Math.floor(rows[0].start/1000/live.widths[i])%999*live.sizes[i];
 bytes[at+5]^=1;
 if(api.decodeHistory(bad,i,live).length!==rows.length-1)throw Error('corrupt record recovery');
}
if(!live.pending.length)throw Error('pending one-hour windows absent');
console.log('C/JS precision, five histories, pending data and corruption checks: '+n+' CPUs');
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arm-binary', type=Path)
    parser.add_argument('--arm32-binary', type=Path)
    parser.add_argument('--fixture-target', choices=('native', 'arm64', 'armv6'), default='native')
    parser.add_argument('--fixtures-only', action='store_true')
    parser.add_argument('--linker', help='path to LLD for cross-compiled fixtures')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='quectop-verify-') as temporary:
        temp = Path(temporary)
        harness = temp / 'core'
        compiler = ['cc']
        fixture_runner = []
        flags = ['-Os', '-ffreestanding', '-fno-builtin', '-fno-pie', '-std=c11',
                 '-Wall', '-Wextra', '-Werror']
        sources = [str(ROOT / 'test/core.c')]
        linking = ['-no-pie']
        if args.fixture_target != 'native':
            compiler = ['clang', '--target=' + ('arm-linux-gnueabi' if args.fixture_target == 'armv6' else 'aarch64-linux-gnu'), '-fuse-ld=lld']
            if args.linker:
                compiler[-1] = '--ld-path=' + args.linker
            if args.fixture_target == 'armv6':
                compiler += ['-march=armv6', '-marm', '-mfloat-abi=soft']
                fixture_runner = ['qemu-arm', '-cpu', 'arm1176']
            else:
                fixture_runner = ['qemu-aarch64']
            flags += ['-DQUECTOP_TEST_FREESTANDING', '-fno-stack-protector',
                      '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
                      '-ffunction-sections', '-fdata-sections']
            sources += [str(ROOT / 'arm.c'), str(ROOT / 'start.S')]
            linking = ['-nostdlib', '-static', '-Wl,-no-pie,--gc-sections,--build-id=none,-z,noexecstack']
        subprocess.run(compiler + flags + sources + linking + ['-o', str(harness)], check=True)
        sizes = {}
        history_hashes = {}
        for ids in ([0], [0, 7], [2, 4, 17, 31], list(range(8)), list(range(16)), list(range(256))):
            case = temp / str(len(ids))
            case.mkdir()
            cpu_frames = frames(case, ids)
            subprocess.run(fixture_runner + [str(harness), str(case), *map(str, cpu_frames)], check=True)
            cores = len(ids)
            total = 0
            history_digest = hashlib.sha256()
            live = (case / 'live.v7.bin').read_bytes()
            interfaces = struct.unpack_from("<I", live, 108)[0]
            frequencies, frequency_bits, metadata_bytes = struct.unpack_from("<3I", live, 120)
            metrics = cores + 6 + frequencies + 2 * interfaces
            value_bits = 10 * (cores + 5) + 12 + frequencies * frequency_bits + 28 * interfaces
            cpu_id_bits = struct.unpack_from('<I', live, 56)[0]
            at = 144 + metadata_bytes + (metrics + value_bits + 7) // 8
            active = []
            for i in range(5):
                w = struct.unpack_from('<I', live, 64 + i * 4)[0]
                size = (32 + metrics + 3 * value_bits + metrics * max(1, (w - 1).bit_length()) + 7) // 8 + 4
                active.append(unpack_record(live[at:at + size], w, cores, frequencies, frequency_bits, interfaces))
                at += size
            for index, name in enumerate(FILES):
                data = (case / name).read_bytes()
                history_digest.update(data)
                version, points, span, width, interval, size, tier, n, metrics, header, id_bits = struct.unpack_from('<11I', data, 8)
                assert (version, points, span, interval, tier, n, metrics) == (6, 1000, SPANS[index], 5000, index, cores, cores + 6 + frequencies + 2 * interfaces)
                assert fnv(data[64:header], fnv(data[:60])) == struct.unpack_from('<I', data, 60)[0]
                read_ids = reader(data[64:header])
                assert [read_ids(id_bits) for _ in ids] == ids
                assert len(data) == header + 999 * size
                total += len(data)
                records = [unpack_record(data[at:at + size], width, cores, frequencies, frequency_bits, interfaces)
                           for at in range(header, len(data), size)]
                records = [record for record in records if record]
                spike = [record for record in records + ([active[index]] if active[index] else []) if record[1][2][0] == 1000 and record[0] + record[2][0] == 1700019050]
                assert len(spike) == 1, (cores, name, 'recent spike lost')
                for _, values, _ in records:
                    assert all(row[cores + 5] == 2048 + 542 for row in values)
                    for interface_index in range(interfaces):
                        rx = cores + 6 + frequencies + 2 * interface_index
                        assert all(row[rx] == 850 | (2 << 10) | (2 << 12) for row in values)
                        assert all(row[rx + 1] == 100 | (1 << 12) for row in values)
            sizes[cores] = total
            history_hashes[cores] = history_digest.hexdigest()
            subprocess.run(['bun', '-e', CODEC_CHECK, str(case)], cwd=ROOT, check=True)
            even, odd = (cores + 1) // 2, cores // 2
            expected = ((even * 90 + odd * 10) * 1000 + (even * 100 + odd * 900) // 2) // (even * 100 + odd * 900)
            env = dict(os.environ, QUECTOP_TEST_EXPECT_OVERALL=f'{expected / 10:.1f}%')
            subprocess.run(['bun', str(ROOT / 'test/frontend.js'), str(case)], cwd=ROOT, env=env, check=True)

        commands = [[str(ROOT / 'quectop')]]
        if args.arm_binary:
            commands.append(['qemu-aarch64', str(args.arm_binary.resolve())])
        if args.arm32_binary:
            commands += [['qemu-arm', '-cpu', cpu, str(args.arm32_binary.resolve())]
                         for cpu in ('arm1176', 'cortex-a7')]
        for command_index, command in enumerate([] if args.fixtures_only else commands):
            executable = Path(command[-1]).read_bytes()
            assert executable[-16:-8] == b'QUECTOPA'
            payload_size = struct.unpack_from('<Q', executable, len(executable) - 8)[0]
            payload_at = len(executable) - 16 - payload_size
            elf64 = executable[4] == 2
            machine = struct.unpack_from('<H', executable, 18)[0]
            alignment = {40: 65536, 62: 4096, 183: 65536}[machine]
            assert payload_at % alignment == 0
            phoff = struct.unpack_from('<Q' if elf64 else '<I', executable, 32 if elf64 else 28)[0]
            phsize, phcount = struct.unpack_from('<HH', executable, 54 if elf64 else 42)
            for index in range(phcount):
                fields = struct.unpack_from('<IIQQQQQQ' if elf64 else '<8I', executable, phoff + index * phsize)
                assert fields[0] not in (2, 3), 'dynamic executable'
                offset, length = (fields[2], fields[5]) if elf64 else (fields[1], fields[4])
                if fields[0] == 1:
                    assert (offset + length + alignment - 1) // alignment * alignment <= payload_at
            output = temp / f'execution-{command_index}'
            output.mkdir()
            live_dir = output / 'ram'
            live_dir.mkdir()
            cli = command + ['--output', str(output), '--live-dir', str(live_dir),
                             '--interval-ms', '100', '--publish-ms', '200', '--interface', 'lo']
            process = subprocess.Popen(cli, stderr=subprocess.PIPE)
            try:
                first = wait_live(process, live_dir / 'live.v7.bin')
                expected_assets = {'index.html': (ROOT / 'web/index.html').read_bytes(),
                                   'style.css': (ROOT / 'web/style.css').read_bytes(),
                                   'app.js': (ROOT / '.build/app.js').read_bytes()}
                digest = hashlib.sha256()
                for name in ('style.css', 'app.js', 'index.html'):
                    data = expected_assets[name]
                    assert (output / name).read_bytes() == data, name
                    digest.update(name.encode('ascii') + b'\0' + struct.pack('<Q', len(data)) + data)
                expected_revision = digest.hexdigest()[:16] + '\n'
                assert (output / 'assets.revision').read_text() == expected_revision
                asset_times = {name: (output / name).stat().st_mtime_ns
                               for name in (*expected_assets, 'assets.revision')}
                generated = struct.unpack_from('<Q', first, 24)[0]
                later = wait_live(process, live_dir / 'live.v7.bin', generated + 200)
                assert not (output / 'live.v7.bin').exists()
                cores, id_bits, metrics = struct.unpack_from('<3I', later, 52)
                assert cores == sum(line.startswith('cpu') and line[3:4].isdigit()
                                    for line in Path('/proc/stat').read_text().splitlines())
                metadata_bytes = struct.unpack_from("<I", later, 128)[0]
                read = reader(later[144 + metadata_bytes:])
                valid = [read(1) for _ in range(metrics)]
                assert all(valid[:cores + 4])  # CPU, aggregate and RAM.
                conflict = subprocess.run(cli, capture_output=True, timeout=5)
                assert conflict.returncode == 1 and b'another collector' in conflict.stderr
            finally:
                process.terminate()
                process.wait(timeout=5)
            restart = subprocess.Popen(cli, stderr=subprocess.PIPE)
            try:
                wait_live(restart, live_dir / 'live.v7.bin', generated + 201)
                assert asset_times == {name: (output / name).stat().st_mtime_ns
                                       for name in asset_times}, 'matching assets were rewritten'
            finally:
                restart.terminate()
                restart.wait(timeout=5)
            for damage in ('revision', 'missing'):
                if damage == 'revision':
                    (output / 'assets.revision').write_text('outdated\n')
                    (output / 'index.html').write_text('old dashboard')
                else:
                    (output / 'style.css').unlink()
                stale = struct.unpack_from('<Q', (live_dir / 'live.v7.bin').read_bytes(), 24)[0]
                recovery = subprocess.Popen(cli, stderr=subprocess.PIPE)
                try:
                    wait_live(recovery, live_dir / 'live.v7.bin', stale + 1)
                    assert (output / 'assets.revision').read_text() == expected_revision
                    for name, data in expected_assets.items():
                        assert (output / name).read_bytes() == data
                finally:
                    recovery.terminate()
                    recovery.wait(timeout=5)
            bad = subprocess.run(cli + ['--points', '500'], capture_output=True, timeout=5)
            assert bad.returncode == 1 and b'configuration differs' in bad.stderr
        print(json.dumps({'fixture_target': args.fixture_target,
                          'history_bytes_by_cpu_count': sizes,
                          'history_hashes_by_cpu_count': history_hashes, 'checks': 'passed'}))


if __name__ == '__main__':
    main()
