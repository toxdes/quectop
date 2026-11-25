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
FILES = ('1h.v1.bin', '1d.v1.bin', '7d.v1.bin', '30d.v1.bin', '90d.v1.bin')


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


def layout_widths(cores, frequencies, frequency_bits, interfaces):
    return (10,) * (cores + 9) + (12,) + (frequency_bits,) * frequencies + (14,) * (2 * interfaces)


def record_size(width, cores, frequencies, frequency_bits, interfaces):
    widths = layout_widths(cores, frequencies, frequency_bits, interfaces)
    low_bits = sum(widths[cores + 7:cores + 10 + frequencies])
    offset_bits = max(1, (width - 1).bit_length())
    return (32 + len(widths) + 2 * sum(widths) + low_bits + len(widths) * offset_bits + 7) // 8 + 4


def unpack_record(data, width, cores, frequencies, frequency_bits, interfaces):
    if not any(data[:4]):
        return None
    assert fnv(data[:-4]) == struct.unpack_from('<I', data, len(data) - 4)[0]
    read = reader(data[:-4])
    start = read(32)
    widths = layout_widths(cores, frequencies, frequency_bits, interfaces)
    valid = [read(1) for _ in widths]
    low_from, low_to = cores + 7, cores + 10 + frequencies  # memory through frequencies
    mean = [read(bits) for bits in widths]
    low = [read(bits) if low_from <= k < low_to else None for k, bits in enumerate(widths)]
    values = [mean, low, [read(bits) for bits in widths]]
    peaks = [read(max(1, (width - 1).bit_length())) for _ in widths]
    assert all(valid)
    assert all(0 <= offset < width for offset in peaks)
    return start, values, peaks


def wait_live(process, path, minimum_generated_ms=0):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(process.stderr.read().decode())
        if path.exists():
            data = path.read_bytes()
            if len(data) >= 148 and fnv(data[:-4]) == struct.unpack_from('<I', data, len(data) - 4)[0]:
                if struct.unpack_from('<Q', data, 24)[0] >= minimum_generated_ms:
                    return data
        time.sleep(0.02)
    raise AssertionError('agent did not publish valid live data')


def check_data_directory(command, root):
    home = root / 'home'
    env = dict(os.environ, HOME=str(home), XDG_STATE_HOME=str(root / 'unused'))
    explicit = root / 'explicit' / 'nested'
    for options, directory in (([], home / '.local/state/quectop'),
                               (['--data-dir', str(explicit)], explicit),
                               (['--data-dir', str(explicit)], explicit),
                               (['--data-dir', 'relative/nested/'], root / 'relative/nested')):
        snapshot = directory / 'live.v1.bin'
        minimum_generated_ms = (struct.unpack_from('<Q', snapshot.read_bytes(), 24)[0] + 1
                                if snapshot.exists() else 0)
        process = subprocess.Popen(command + options, cwd=root, env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            wait_live(process, snapshot, minimum_generated_ms)
        finally:
            process.terminate()
            stdout, stderr = process.communicate(timeout=10)
        selected = options[-1] if options else str(directory)
        assert stdout == f'quectop: data directory: {selected}\n'.encode(), stdout
        assert not stderr, stderr
    assert not (root / 'unused').exists()
    blocker = root / 'file'
    blocker.write_text('not a directory')
    for options, home_value in ((['--data-dir', str(blocker)], str(home)),
                                (['--data-dir', str(blocker / 'child')], str(home)),
                                (['--data-dir', ''], str(home)),
                                (['--data-dir', 'x' * 4096], str(home)),
                                ([], ''), ([], 'relative')):
        result = subprocess.run(command + options, cwd=root,
                                env=dict(env, HOME=home_value), capture_output=True, timeout=10)
        assert result.returncode == 1 and result.stderr.startswith(b'quectop'), result
        assert not result.stdout, result.stdout
    no_home = dict(env)
    del no_home['HOME']
    result = subprocess.run(command, env=no_home, capture_output=True, timeout=10)
    assert result.returncode == 1 and b'HOME' in result.stderr


def check_temporary_files(command, root):
    names = ('live.v1.bin', 'disks.v1.bin')
    for kind in ('hardlink', 'fifo'):
        directory = root / kind
        directory.mkdir()
        sentinels = []
        for name in names:
            temporary = directory / f'.{name}.tmp'
            if kind == 'hardlink':
                sentinel = root / name
                sentinel.write_bytes(b'keep this file intact\n')
                temporary.hardlink_to(sentinel)
                sentinels.append(sentinel)
            else:
                os.mkfifo(temporary)
        process = subprocess.Popen(command + ['--data-dir', str(directory)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            wait_live(process, directory / 'live.v1.bin')
        finally:
            process.terminate()
            process.communicate(timeout=5)
        for sentinel in sentinels:
            assert sentinel.read_bytes() == b'keep this file intact\n', sentinel.name


def frames(directory, ids):
    baseline = [[1000, 0, 1000, 1000, 0, 0, 0, 0] for _ in ids]
    deltas = [[90, 0, 0, 10, 0, 0, 0, 0] if i % 2 == 0
              else [0, 0, 10, 880, 10, 0, 0, 0] for i in range(len(ids))]
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
const NativeTextDecoder=globalThis.TextDecoder;
let decoderCount=0;
globalThis.TextDecoder=class extends NativeTextDecoder{constructor(...args){super(...args);decoderCount++;}};
const api=require('./web/codec.js');
const read=name=>{const b=fs.readFileSync(process.argv.at(-1)+'/'+name);return b.buffer.slice(b.byteOffset,b.byteOffset+b.byteLength);};
const live=api.decodeLive(read('live.v1.bin')),n=live.cores;
if(decoderCount!==1)throw Error('extra live snapshot decoder');
if(live.hostname!==require('os').hostname())throw Error('host name: '+live.hostname);
if(live.latest[live.format.temp]!==54.2||live.interfaces&&(live.latest[live.format.network]!==8500000||live.latest[live.format.network+1]!==100000))throw Error('precision');
for(let i=0;i<5;i++){
 const name=['1h.v1.bin','1d.v1.bin','7d.v1.bin','30d.v1.bin','90d.v1.bin'][i];
 const rows=api.decodeHistory(read(name),i,live);
 const combined=rows.concat(live.active[i]?[live.active[i]]:[],i===0?live.pending:[]);
 const spike=combined.find(r=>r.max[0]===100&&r.peak[0]===1700019050000);
 if(!spike)throw Error('spike or peak time lost: '+name);
 if(!rows.length)throw Error('no committed records');
 const bad=read(name),bytes=new Uint8Array(bad),at=64+live.metadataBytes+Math.floor(rows[0].start/1000/live.widths[i])%999*live.sizes[i];
 bytes[at+5]^=1;
 const damaged=api.skipped;
 if(api.decodeHistory(bad,i,live).length!==rows.length-1||api.skipped!==damaged+1)throw Error('corrupt record recovery');
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
    parser.add_argument('--sanitize', action='store_true', help='enable native address/undefined sanitizers')
    args = parser.parse_args()
    if args.sanitize and args.fixture_target != 'native':
        parser.error('--sanitize requires --fixture-target native')
    with tempfile.TemporaryDirectory(prefix='quectop-verify-') as temporary:
        temp = Path(temporary)
        harness = temp / 'core'
        compiler = ['cc']
        fixture_runner = []
        flags = ['-Os', '-ffreestanding', '-fno-builtin', '-fno-pie', '-std=c11',
                 '-Wall', '-Wextra', '-Werror']
        if args.sanitize:
            flags += ['-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        sources = [str(ROOT / 'test/core.c'), str(ROOT / 'vendor/monocypher/monocypher.c'),
                   str(ROOT / 'vendor/monocypher/monocypher-ed25519.c')]
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
            sources += [str(ROOT / 'boot.c'), str(ROOT / 'arm.c'), str(ROOT / 'start.S')]
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
            live = (case / 'live.v1.bin').read_bytes()
            interfaces = struct.unpack_from("<I", live, 108)[0]
            frequencies, frequency_bits, metadata_bytes = struct.unpack_from("<3I", live, 120)
            expected_metrics = cores + 10 + frequencies + 2 * interfaces
            value_bits = sum(layout_widths(cores, frequencies, frequency_bits, interfaces))
            cpu_id_bits = struct.unpack_from('<I', live, 56)[0]
            at = 144 + metadata_bytes + (expected_metrics + value_bits + 7) // 8
            active = []
            for i in range(5):
                w = struct.unpack_from('<I', live, 64 + i * 4)[0]
                size = record_size(w, cores, frequencies, frequency_bits, interfaces)
                active.append(unpack_record(live[at:at + size], w, cores, frequencies, frequency_bits, interfaces))
                at += size
            for index, name in enumerate(FILES):
                data = (case / name).read_bytes()
                history_digest.update(data)
                version, points, span, width, interval, size, tier, n, metrics, header, id_bits = struct.unpack_from('<11I', data, 8)
                assert (version, points, span, interval, tier, n, metrics) == (1, 1000, SPANS[index], 5000, index, cores, expected_metrics)
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
                    assert all(row[cores + 9] == 2048 + 542 for row in values)
                    for interface_index in range(interfaces):
                        rx = cores + 10 + frequencies + 2 * interface_index
                        assert values[1][rx] is None and values[1][rx + 1] is None
                        assert all(row[rx] == 850 | (2 << 10) | (2 << 12) for row in (values[0], values[2]))
                        assert all(row[rx + 1] == 100 | (1 << 12) for row in (values[0], values[2]))
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
            startup = temp / f'startup-{command_index}'
            startup.mkdir()
            check_data_directory(command, startup)
            check_temporary_files(command, startup)
            executable = Path(command[-1]).read_bytes()
            elf64 = executable[4] == 2
            phoff = struct.unpack_from('<Q' if elf64 else '<I', executable, 32 if elf64 else 28)[0]
            phsize, phcount = struct.unpack_from('<HH', executable, 54 if elf64 else 42)
            assert struct.unpack_from('<H', executable, 16)[0] == 3, 'not a PIE'
            for index in range(phcount):
                kind = struct.unpack_from('<I', executable, phoff + index * phsize)[0]
                assert kind != 3, 'needs a dynamic loader'
            output = temp / f'execution-{command_index}'
            output.mkdir()
            live_dir = output / 'ram'
            live_dir.mkdir()
            cli = command + ['--output', str(output), '--live-dir', str(live_dir),
                             '--interval-ms', '100', '--publish-ms', '200', '--interface', 'lo']
            process = subprocess.Popen(cli, stderr=subprocess.PIPE)
            try:
                first = wait_live(process, live_dir / 'live.v1.bin')
                assert not any((output / name).exists() for name in ('index.html', 'app.js', 'style.css'))
                generated = struct.unpack_from('<Q', first, 24)[0]
                later = wait_live(process, live_dir / 'live.v1.bin', generated + 200)
                assert not (output / 'live.v1.bin').exists()
                cores, id_bits, metrics = struct.unpack_from('<3I', later, 52)
                assert cores == sum(line.startswith('cpu') and line[3:4].isdigit()
                                    for line in Path('/proc/stat').read_text().splitlines())
                metadata_bytes = struct.unpack_from("<I", later, 128)[0]
                read = reader(later[144 + metadata_bytes:])
                valid = [read(1) for _ in range(metrics)]
                assert all(valid[:cores + 4]) and valid[cores + 7]  # CPU, aggregate, iowait and RAM.
                if Path('/proc/pressure/io').exists():
                    assert all(valid[cores + 4:cores + 7]), 'PSI stall shares'
                conflict = subprocess.run(cli, capture_output=True, timeout=5)
                assert conflict.returncode == 1 and b'quectop: another quectop owns' in conflict.stderr
            finally:
                process.terminate()
                process.wait(timeout=5)
            bad = subprocess.run(cli + ['--points', '500'], capture_output=True, timeout=5)
            assert bad.returncode == 1 and b'configuration differs' in bad.stderr
        print(json.dumps({'fixture_target': args.fixture_target,
                          'history_bytes_by_cpu_count': sizes,
                          'history_hashes_by_cpu_count': history_hashes, 'checks': 'passed'}))


if __name__ == '__main__':
    main()
