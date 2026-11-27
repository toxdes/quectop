#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["noiseprotocol==0.3.1"]
# ///
"""Run a real probe against quectop-hub: replication, resume, HTTP, device keys
and protocol rejection. Hand-written probes and hubs speak Noise through the
independent noiseprotocol package."""

import argparse
import http.client
import os
from pathlib import Path
import shlex
import socket
import struct
import subprocess
import tempfile
import time

from noise.connection import NoiseConnection


ROOT = Path(__file__).resolve().parent.parent
TIERS = ('1h', '1d', '7d', '30d', '90d')
RINGS = [f'{tier}.v1.bin' for tier in TIERS]


def fnv(data, value=2166136261):
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def wait(condition, message, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = condition()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError(message)


def get(port, path, method='GET', headers=None):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
    connection.request(method, path, headers=headers or {})
    response = connection.getresponse()
    body = response.read()
    connection.close()
    return response.status, dict(response.getheaders()), body


def ring_geometry(data):
    points, _, width, _, record_bytes = struct.unpack_from('<5I', data, 12)
    header_bytes = struct.unpack_from('<I', data, 44)[0]
    return points, width, record_bytes, header_bytes


def records(data):
    """Valid records, with their ring offset, by bucket start."""
    points, _, record_bytes, header_bytes = ring_geometry(data)
    found = {}
    for at in range(header_bytes, header_bytes + (points - 1) * record_bytes, record_bytes):
        record = data[at:at + record_bytes]
        if len(record) == record_bytes and fnv(record[:-4]) == struct.unpack_from('<I', record, record_bytes - 4)[0]:
            found[struct.unpack_from('<I', record)[0]] = at, record
    return found


def frame(kind, payload):
    return bytes([kind]) + len(payload).to_bytes(3, 'little') + payload


NOISE = b'Noise_NNpsk0_25519_ChaChaPoly_SHA512'


def message(body):
    return len(body).to_bytes(2, 'little') + body


def read_message(sock):
    """The next length-prefixed message; None once the peer closes."""
    head = read_exact(sock, 2)
    body = read_exact(sock, int.from_bytes(head, 'little')) if len(head) == 2 else b''
    return body if len(head) == 2 and len(body) == int.from_bytes(head, 'little') else None


def opening(identity, version=3):
    return b'QTOP' + bytes([version, len(identity)]) + identity


class Probe:
    """A hand-written probe: OPEN, Noise message 1, then sealed frames."""

    def __init__(self, port, identity, name=b'python', publish=1000, key=None, version=3):
        self.sock = socket.create_connection(('127.0.0.1', port))
        self.noise = NoiseConnection.from_name(NOISE)
        self.noise.set_as_initiator()
        self.noise.set_psks(psk=key or bytes(32))
        self.open = opening(identity, version)
        self.noise.set_prologue(self.open)
        self.noise.start_handshake()
        self.first = self.noise.write_message(struct.pack('<I', publish) + bytes([len(name)]) + name)

    def handshake(self):
        """True once the hub answers with a message 2 that proves the key."""
        self.sock.sendall(message(self.open) + message(self.first))
        reply = read_message(self.sock)
        if reply is None:
            return False
        self.noise.read_message(reply)
        return True

    def send(self, kind, payload):
        self.sock.sendall(message(self.noise.encrypt(frame(kind, payload))))

    def receive(self):
        return self.noise.decrypt(read_message(self.sock))


def read_exact(sock, count):
    data = b''
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            return data
        data += chunk
    return data


def closed_soon(sock):
    """Whether the peer closes within half a second."""
    sock.settimeout(0.5)
    try:
        return sock.recv(1) == b''
    except socket.timeout:
        return False
    except ConnectionResetError:
        return True


def closed(sock):
    sock.settimeout(5)
    try:
        return sock.recv(1) == b''
    except ConnectionResetError:
        return True


class Hub:
    def __init__(self, command, directory, http=True, extra=(), keys=None):
        self.command, self.directory, self.extra = command, directory, list(extra)
        # Never the caller's real ~/.config keys file.
        self.keys = keys or directory.parent / 'no-keys'
        self.command = command + ['--keys', str(self.keys)]
        self.probes, self.http = free_port(), free_port() if http else None
        self.process = None

    def start(self):
        http = ['--http', f'127.0.0.1:{self.http}'] if self.http else []
        self.process = subprocess.Popen(self.command + [
            '--data-dir', str(self.directory), '--probes', f'127.0.0.1:{self.probes}', *http, *self.extra],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        wait(lambda: self.alive() and self.listening(), 'hub did not start')

    def alive(self):
        if self.process.poll() is not None:
            raise AssertionError(self.process.stderr.read().decode())
        return True

    def listening(self):
        try:
            socket.create_connection(('127.0.0.1', self.probes)).close()
            return not self.http or get(self.http, '/hosts.txt')[0] == 200
        except OSError:
            return False

    def stop(self):
        self.process.terminate()
        self.process.wait(timeout=5)

    def hosts(self):
        """hosts.txt, read the way the dashboard reads it when HTTP is on."""
        if self.http:
            status, headers, body = get(self.http, '/hosts.txt')
            assert status == 200 and headers['Content-Type'].startswith('text/plain')
        else:
            body = (self.directory / 'hosts.txt').read_bytes()
        return [line.split('\t') for line in body.decode().splitlines()]


def check_http(hub, identity):
    """Built-in server: the same tree a static server would serve, nothing more."""
    for path, name in (('/', 'index.html'), ('/index.html', 'index.html'),
                       ('/style.css', 'style.css'), ('/app.js', 'app.js')):
        expected = (ROOT / '.build' / name).read_bytes()
        assert (hub.directory / name).read_bytes() == expected, name
        status, headers, body = get(hub.http, path)
        assert status == 200 and body == expected, path
        assert headers['X-Content-Type-Options'] == 'nosniff'
        assert 'Access-Control-Allow-Origin' not in headers, 'same-origin only without --cors'
        status, headers, body = get(hub.http, path, method='HEAD')
        assert status == 200 and not body and int(headers['Content-Length']) == len(expected)
    assert get(hub.http, '/')[1]['Content-Type'].startswith('text/html')
    assert get(hub.http, '/app.js')[1]['Content-Type'].startswith('text/javascript')
    status, headers, body = get(hub.http, f'/h/{identity}/1h.v1.bin?cache=1')
    assert status == 200 and body == (hub.directory / 'h' / identity / '1h.v1.bin').read_bytes()
    assert headers['Content-Type'] == 'application/octet-stream'
    for path in (f'/h/{identity}/name', f'/h/{identity}/../live.v1.bin', f'/h/{identity}/.live.v1.bin.tmp',
                 '/h/missing/live.v1.bin', f'/h/{identity}', f'/h/{identity}/', '/h/', '/h',
                 '/hosts.txt/', '/.assets.revision', '/.hosts.txt.tmp', '/../hub', '//etc/passwd',
                 '/h/x/y/z.bin', '/%2e%2e/hub'):
        assert get(hub.http, path)[0] == 404, path
    stray = hub.directory / 'x' / identity
    stray.mkdir(parents=True)
    (stray / 'live.v1.bin').write_bytes(b'not served')
    assert get(hub.http, f'/x/{identity}/live.v1.bin')[0] == 404, 'only h/ holds host files'
    alias = hub.directory / 'h' / 'alias'
    alias.symlink_to(identity, target_is_directory=True)
    fifo = hub.directory / 'unavailable.txt'
    os.mkfifo(fifo)
    try:
        assert get(hub.http, '/h/alias/live.v1.bin')[0] == 404, 'directory alias was followed'
        assert get(hub.http, '/unavailable.txt')[0] == 404, 'FIFO accepted'
        assert get(hub.http, '/hosts.txt')[0] == 200, 'HTTP stalled on a special file'
    finally:
        alias.unlink()
        fifo.unlink()
    assert get(hub.http, '/', method='POST')[0] == 405
    with socket.create_connection(('127.0.0.1', hub.http)) as sock:
        sock.sendall(b'GET / HTTP/1.1\r\nX: ' + b'a' * 3000)
        assert read_exact(sock, 12) == b'HTTP/1.1 431'
    with socket.create_connection(('127.0.0.1', hub.http)) as sock:
        sock.sendall(b'GET /index.html HTTP/1.1\r\n')  # incomplete: times out, never blocks others
        assert get(hub.http, '/hosts.txt')[0] == 200


def check_cors(command, directory, identity):
    """--cors lets a dashboard hosted elsewhere read the data; bad values never start."""
    for extra in (['--cors', 'https://example.com'],
                  ['--http', f'127.0.0.1:{free_port()}', '--cors', 'https://a.com\r\nX: y'],
                  ['--http', f'127.0.0.1:{free_port()}', '--cors', ''],
                  ['--http', f'127.0.0.1:{free_port()}', '--cors', 'a' * 257]):
        result = subprocess.run(command + ['--data-dir', str(directory), *extra], capture_output=True, timeout=5)
        assert result.returncode == 1 and b'--cors needs --http' in result.stderr, extra
    for origin in ('https://example.com', '*'):
        hub = Hub(command, directory, extra=['--cors', origin])
        hub.start()
        try:
            for path in ('/hosts.txt', f'/h/{identity}/live.v1.bin', '/missing.txt'):
                assert get(hub.http, path)[1]['Access-Control-Allow-Origin'] == origin, path
        finally:
            hub.stop()


def check_keys(command, probe_command, root, ring_headers):
    """Device keys: --add-device, the Noise handshake, replay, revocation, the
    exposure rule, and key files other users can read."""
    keys = root / 'keys-dir' / 'nested' / 'keys'
    run = lambda *args: subprocess.run(command + ['--keys', str(keys), *args], capture_output=True, timeout=5)
    exposed = run('--data-dir', str(root / 'keyed'), '--probes', f'0.0.0.0:{free_port()}')
    assert exposed.returncode == 1 and b'needs device keys' in exposed.stderr, exposed.stderr
    keys.parent.mkdir(parents=True, exist_ok=True)
    for mode in (0o644, 0o640):
        keys.write_bytes(b'# existing file\n')
        keys.chmod(mode)
        result = run('--add-device', 'laptop')
        assert result.returncode == 1 and not result.stdout, result.stderr
        assert keys.read_bytes() == b'# existing file\n'
    keys.unlink()
    os.mkfifo(keys, 0o600)
    result = run('--add-device', 'laptop')
    assert result.returncode == 1 and not result.stdout, result.stderr
    assert keys.is_fifo()
    keys.unlink()
    keys.parent.rmdir()  # exercise directory and key creation below
    added = run('--add-device', 'laptop')
    assert added.returncode == 0, added.stderr
    name, hex_key = added.stdout.decode().split()
    assert name == 'laptop' and len(hex_key) == 64 and keys.stat().st_mode & 0o777 == 0o600
    laptop = bytes.fromhex(hex_key)
    for bad in ('laptop', '.hidden', 'a b'):
        assert run('--add-device', bad).returncode == 1, bad
    with keys.open('a') as file:
        file.write('# hand-edited, no newline')
    phone = bytes.fromhex(run('--add-device', 'phone').stdout.decode().split()[1])
    assert keys.read_text().count('\n') == 3, keys.read_text()

    hub = Hub(command, root / 'keyed', http=False, keys=keys)
    hub.start()
    listed = lambda identity: [r for r in hub.hosts() if r[0] == identity and r[1] == '1']
    try:
        def stream(identity, key):
            probe = Probe(hub.probes, identity, key=key)
            assert probe.handshake(), identity
            for tier, header in enumerate(ring_headers):
                probe.send(2, bytes([tier]) + header)
            assert probe.receive()[:4] == bytes([3, 20, 0, 0])
            return probe
        good = stream(b'laptop', laptop)
        wait(lambda: listed('laptop'), 'keyed device not accepted')
        with keys.open('a') as file:  # a line with trailing junk lists nothing
            file.write(f'junk {laptop.hex()} extra\n')
        for identity, key in ((b'laptop', phone), (b'laptop', None), (b'tablet', laptop), (b'tablet', None),
                               (b'junk', laptop)):
            probe = Probe(hub.probes, identity, key=key)
            assert not probe.handshake() and closed(probe.sock), (identity, key)
        # Message 1 can be replayed, but the replayer cannot seal a frame and the
        # real device stays connected.
        replayed = socket.create_connection(('127.0.0.1', hub.probes))
        replayed.sendall(message(good.open) + message(good.first))
        assert read_message(replayed) is not None
        replayed.sendall(message(bytes(40)))
        assert closed(replayed), 'replayed handshake accepted a frame'
        assert listed('laptop') and not closed_soon(good.sock), 'replay disconnected the real device'
        good.sock.close()
        # Lines added or removed while the hub runs apply to the next connection.
        assert run('--add-device', 'tablet').returncode == 0
        keys.write_text(''.join(line + '\n' for line in keys.read_text().splitlines() if not line.startswith('laptop')))
        assert not Probe(hub.probes, b'laptop', key=laptop).handshake(), 'removed key still accepted'
        keys.chmod(0o640)  # a keys file others can read authorizes nobody
        assert not Probe(hub.probes, b'phone', key=phone).handshake(), 'exposed keys file still accepted'
        keys.chmod(0o600)

        key_file = root / 'device.key'
        key_file.write_text(f'phone {phone.hex()}\n')
        key_file.chmod(0o600)
        probe = subprocess.Popen(probe_command + [
            '--data-dir', str(root / 'keyed-probe'), '--interval-ms', '100', '--publish-ms', '200',
            '--interface', 'lo', '--hub', f'127.0.0.1:{hub.probes}', '--key-file', str(key_file)],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            wait(lambda: listed('phone'), 'probe with --key-file not accepted')
            wait(lambda: (hub.directory / 'h' / 'phone' / 'live.v1.bin').exists(), 'keyed probe not streaming')
        finally:
            probe.terminate()
            probe.wait(timeout=5)
            assert not probe.stderr.read()
        silent = socket.create_connection(('127.0.0.1', hub.probes))
        started = time.monotonic()
        silent.settimeout(15)
        assert silent.recv(1) == b''
        assert time.monotonic() - started < 13, 'silent connection kept its slot'
    finally:
        hub.process.terminate()
        hub.process.wait(timeout=5)
    log = hub.process.stderr.read().decode()
    assert 'rejected device laptop' in log and 'rejected device tablet' in log, log
    assert 'keys must be a private regular file' in log, log
    keys.chmod(0o644)
    started = run('--data-dir', str(root / 'keyed'), '--probes', f'127.0.0.1:{free_port()}')
    assert started.returncode == 1 and b'private regular file' in started.stderr, started.stderr
    exposed_key = root / 'exposed.key'
    exposed_key.write_text(key_file.read_text())
    exposed_key.chmod(0o604)

    for args, expected in ((['--key-file', str(key_file)], b'--key-file needs --hub'),
                           (['--hub', '127.0.0.1:1', '--key-file', str(keys.parent)], b'--key-file'),
                           (['--hub', '127.0.0.1:1', '--key-file', str(exposed_key)], b'private regular file'),
                           (['--hub', 'https://example.com'], b'not a URL')):
        result = subprocess.run(probe_command + ['--data-dir', str(root / 'unused')] + args,
                                capture_output=True, timeout=5)
        assert result.returncode == 1 and expected in result.stderr, (args, result.stderr)


def check_static_server(directory, identity):
    """DATA is a plain document root: an unrelated static server can serve the dashboard."""
    port = free_port()
    server = subprocess.Popen(['python3', '-m', 'http.server', str(port), '--bind', '127.0.0.1',
                               '--directory', str(directory)],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait(lambda: server.poll() is None and port_open(port), 'static server did not start')
        for path in ('/', '/app.js', '/style.css', '/hosts.txt', f'/h/{identity}/live.v1.bin',
                     f'/h/{identity}/disks.v1.bin', *(f'/h/{identity}/{ring}' for ring in RINGS)):
            assert get(port, path)[0] == 200, path
    finally:
        server.terminate()
        server.wait(timeout=5)


def port_open(port):
    try:
        socket.create_connection(('127.0.0.1', port)).close()
        return True
    except OSError:
        return False


def check_protocol(hub, ring_headers, live):
    """A hand-written probe: RESUME semantics, record storage, replacement, rejection."""
    def session():
        probe = Probe(hub.probes, b'python-probe')
        assert probe.handshake()
        for tier, header in enumerate(ring_headers):
            probe.send(2, bytes([tier]) + header)
        reply = probe.receive()
        assert reply[:4] == bytes([3, 20, 0, 0]), reply
        return probe, struct.unpack_from('<5I', reply, 4)

    first, held = session()
    assert held == (0,) * 5, held
    points, width, record_bytes, header_bytes = ring_geometry(ring_headers[0])
    start = (int(time.time()) // width - 3) * width
    record = bytearray(record_bytes)
    record[:4] = struct.pack('<I', start)
    record[-4:] = struct.pack('<I', fnv(record[:-4]))
    first.send(4, b'\x00' + bytes(record))
    replica = hub.directory / 'h' / 'python-probe' / RINGS[0]
    stored = wait(lambda: records(replica.read_bytes()).get(start), 'record not stored')
    assert stored == (header_bytes + start // width % (points - 1) * record_bytes, bytes(record))
    second, held = session()  # replaces the first stream of the same host
    assert held[0] == start and closed(first.sock)
    damaged = bytearray(record)
    damaged[5] ^= 1
    second.send(4, b'\x00' + bytes(damaged))
    assert closed(second.sock), 'damaged record accepted'
    second, _ = session()  # an unknown frame type is skipped, not refused
    second.send(9, b'future')
    second.send(4, b'\x00' + bytes(record))
    assert not closed_soon(second.sock), 'unknown frame type closed the stream'
    third, _ = session()  # a valid record whose frame header lies about its length
    lying = bytearray(frame(4, b'\x00' + bytes(record)))
    lying[1] += 1
    third.sock.sendall(message(third.noise.encrypt(bytes(lying))))
    assert closed(third.sock), 'frame length mismatch accepted'
    # Refused before or during the handshake.
    for identity, name, version in ((b'../escape', b'python', 3), (b'.hidden', b'python', 3),
                                    (b'bad\tname', b'x', 3), (b'ok', b'tab\there', 3),
                                    (b'old-version', b'python', 2), (b'new-version', b'python', 4)):
        probe = Probe(hub.probes, identity, name=name, version=version)
        assert not probe.handshake() and closed(probe.sock), identity
    for raw in (message(b'QTOP\x03'), message(opening(b'short')) + message(bytes(47))):
        with socket.create_connection(('127.0.0.1', hub.probes)) as sock:
            sock.sendall(raw)
            assert closed(sock), raw
    # Refused after it: forged, mislabelled or malformed frames.
    def refused(attack):
        probe = Probe(hub.probes, b'python-probe')
        assert probe.handshake()
        attack(probe)
        assert closed(probe.sock)
    refused(lambda a: a.send(4, b'\x00'))
    refused(lambda a: a.send(3, bytes(20)))  # RESUME runs hub to probe only
    refused(lambda a: a.sock.sendall(message(bytes(40))))  # not sealed
    refused(lambda a: a.sock.sendall(message(a.noise.encrypt(b'\x02\x09\x00\x00' + bytes(8)))))  # length lies
    def reordered(probe):
        early, late = (probe.noise.encrypt(frame(2, bytes([tier]) + ring_headers[tier])) for tier in (0, 1))
        probe.sock.sendall(message(late) + message(early))
    refused(reordered)
    def tampered(probe):
        sealed = bytearray(probe.noise.encrypt(frame(2, bytes([0]) + ring_headers[0])))
        sealed[7] ^= 1
        probe.sock.sendall(message(bytes(sealed)))
    refused(tampered)
    newer = bytearray(ring_headers[0])
    newer[8:12] = struct.pack('<I', 2)
    refused(lambda a: a.send(2, b'\x00' + bytes(newer)))
    incompatible = bytearray(ring_headers[0])
    struct.pack_into('<I', incompatible, 28, record_bytes + 4)
    struct.pack_into('<I', incompatible, 60, fnv(incompatible[64:], fnv(incompatible[:60])))
    refused(lambda a: a.send(2, b'\x00' + bytes(incompatible)))
    live_probe, _ = session()
    live_probe.send(5, b'QUECTOPL' + struct.pack('<I', 2) + bytes(140))
    assert closed(live_probe.sock), 'newer live format accepted'
    live_probe, _ = session()
    incompatible = bytearray(live)
    struct.pack_into('<I', incompatible, 12, points + 1)
    struct.pack_into('<I', incompatible, len(incompatible) - 4, fnv(incompatible[:-4]))
    live_probe.send(5, incompatible)
    assert closed(live_probe.sock), 'incompatible live layout accepted'
    backup = hub.directory.parent / 'hardlinked-ring-backup'
    os.link(replica, backup)
    saved = backup.read_bytes()
    fresh, held = session()
    assert held[0] == 0 and not replica.samefile(backup), 'shared ring inode was reused'
    fresh.send(4, b'\x00' + bytes(record))
    wait(lambda: records(replica.read_bytes()).get(start), 'replacement ring did not accept records')
    assert backup.read_bytes() == saved
    fresh.sock.close()
    assert not (hub.directory / '..' / 'escape').exists()
    assert [row[0] for row in hub.hosts()].count('python-probe') == 1


def check_impostor(probe_command, root):
    """The probe sends nothing past message 1 to a hub without its key, and
    interoperates with a noiseprotocol hub that has it."""
    key = bytes(range(32))
    key_file = root / 'impostor.key'
    key_file.write_text(f'phone {key.hex()}\n')
    key_file.chmod(0o600)
    listener = socket.create_server(('127.0.0.1', 0))
    probe = subprocess.Popen(probe_command + [
        '--data-dir', str(root / 'impostor-probe'), '--interval-ms', '100', '--publish-ms', '200',
        '--interface', 'lo', '--hub', f'127.0.0.1:{listener.getsockname()[1]}', '--key-file', str(key_file)],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        listener.settimeout(15)
        sock, _ = listener.accept()
        with sock:
            sock.settimeout(5)
            opened, first = read_message(sock), read_message(sock)
            assert opened == opening(b'phone') and len(first) >= 48, (opened, first)
            sock.sendall(message(bytes(range(48))))  # a hub that cannot prove the key
            assert read_exact(sock, 1) == b'', 'probe sent data to an impostor'
        sock, _ = listener.accept()  # the probe retries; this hub holds the key
        with sock:
            sock.settimeout(5)
            hub = NoiseConnection.from_name(NOISE)
            hub.set_as_responder()
            hub.set_psks(psk=key)
            opened = read_message(sock)
            hub.set_prologue(opened)
            hub.start_handshake()
            payload = hub.read_message(read_message(sock))
            assert payload[5:5 + payload[4]] == socket.gethostname().encode(), payload
            sock.sendall(message(hub.write_message(b'')))
            for tier in range(len(TIERS)):
                ring = hub.decrypt(read_message(sock))
                assert ring[0] == 2 and ring[4] == tier and ring[5:13] == b'QUECTOPH', ring[:13]
    finally:
        probe.terminate()
        probe.wait(timeout=5)
        listener.close()
    assert not probe.stderr.read()


def check_replica_io(command, root, ring_headers, live):
    hub = Hub(command, root / 'replica-io')
    hub.start()
    probes = []

    def stream(identity):
        probe = Probe(hub.probes, identity.encode())
        probes.append(probe)
        probe.sock.settimeout(5)
        assert probe.handshake()
        for tier, header in enumerate(ring_headers):
            probe.send(2, bytes([tier]) + header)
        assert probe.receive()[:4] == bytes([3, 20, 0, 0])
        return probe, hub.directory / 'h' / identity / 'live.v1.bin'

    def status(identity):
        return next((row[1] for row in hub.hosts() if row[0] == identity), None)

    try:
        failing, old = stream('failing')
        healthy, current = stream('healthy')
        failing.send(5, live)
        healthy.send(5, live)
        wait(lambda: old.exists() and current.exists(), 'initial snapshots did not arrive')

        blocked_snapshot = old.with_name('.live.v1.bin.tmp')
        blocked_snapshot.mkdir()
        change = bytearray(live[:-4])
        change[24] ^= 1  # snapshot generation time
        updated = bytes(change) + struct.pack('<I', fnv(change))
        failing.send(5, updated)
        assert closed(failing.sock), 'probe with a failed snapshot stayed connected'
        wait(lambda: status('failing') == '0', 'failed replica was still listed online')
        assert old.read_bytes() == live
        healthy.send(5, updated)
        path = '/h/healthy/live.v1.bin'
        wait(lambda: get(hub.http, path)[2] == updated, 'healthy probe stopped streaming')
        blocked_snapshot.rmdir()

        blocked_index = hub.directory / '.hosts.txt.tmp'
        blocked_index.mkdir()
        newcomer, snapshot = stream('newcomer')
        newcomer.send(5, live)
        wait(snapshot.exists, 'index failure prevented a healthy probe from streaming')
        assert status('newcomer') is None and status('healthy') == '1'
        blocked_index.rmdir()
        wait(lambda: status('newcomer') == '1', 'host index did not retry')

        recovered, _ = stream('failing')
        recovered.send(5, updated)
        wait(lambda: old.read_bytes() == updated, 'replica did not recover')
    finally:
        for probe in probes:
            probe.sock.close()
        hub.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', default=str(ROOT / 'quectop'), help='probe command')
    parser.add_argument('--hub', default=str(ROOT / 'quectop-hub'), help='hub command')
    args = parser.parse_args()
    probe_command, hub_command = shlex.split(args.probe), shlex.split(args.hub)
    with tempfile.TemporaryDirectory(prefix='quectop-hub-') as temporary:
        root = Path(temporary)
        hub = Hub(hub_command, root / 'hub')
        hub.start()
        conflict = subprocess.run(hub.command + ['--data-dir', str(hub.directory), '--probes',
                                                 f'127.0.0.1:{free_port()}', '--http', f'127.0.0.1:{free_port()}'],
                                  capture_output=True, timeout=5)
        assert conflict.returncode == 1 and b'quectop-hub: another quectop-hub owns' in conflict.stderr
        bad = subprocess.run(probe_command + ['--hub', 'bad name:7070'], capture_output=True, timeout=5)
        assert bad.returncode == 1 and b'--hub needs HOST:PORT' in bad.stderr
        remote = subprocess.run(probe_command + ['--hub', '192.0.2.1:7070'], capture_output=True, timeout=5)
        assert remote.returncode == 1 and b'outside loopback' in remote.stderr
        probe_data = root / 'probe'
        probe = subprocess.Popen(probe_command + [
            '--data-dir', str(probe_data), '--interval-ms', '100', '--publish-ms', '200',
            '--interface', 'lo', '--hub', f'127.0.0.1:{hub.probes}'],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            rows = wait(lambda: hub.alive() and [r for r in hub.hosts() if r[1] == '1'], 'probe not connected')
            identity, _, name = rows[0]
            assert name == socket.gethostname()
            replica = hub.directory / 'h' / identity
            wait(lambda: (replica / 'live.v1.bin').exists() and (replica / 'disks.v1.bin').exists(),
                 'snapshots not replicated')
            live = replica / 'live.v1.bin'
            data = live.read_bytes()
            assert fnv(data[:-4]) == struct.unpack_from('<I', data, len(data) - 4)[0]
            generated = struct.unpack_from('<Q', data, 24)[0]
            wait(lambda: struct.unpack_from('<Q', live.read_bytes(), 24)[0] > generated, 'live not refreshed')
            check_http(hub, identity)

            # Resume: buckets completed while the hub is down are backfilled.
            wait(lambda: records((replica / RINGS[0]).read_bytes()), 'no bucket replicated', 30)
            hub.stop()
            ring = (replica / RINGS[0]).read_bytes()
            width, newest_before = ring_geometry(ring)[1], max(records(ring))
            latest = lambda: struct.unpack_from('<I', (probe_data / 'live.v1.bin').read_bytes(), 48)[0]
            wait(lambda: latest() >= newest_before + 3 * width, 'probe stopped sampling', 30)
            hub.start()
            wait(lambda: [r for r in hub.hosts() if r[0] == identity and r[1] == '1'], 'probe did not reconnect')
            wait(lambda: max(records((replica / RINGS[0]).read_bytes())) >= newest_before + 4 * width,
                 'stream did not continue after reconnecting')
            # The probe samples continuously, so every bucket exists: no gap while the hub was down.
            held = records((replica / RINGS[0]).read_bytes())
            assert all(start in held for start in range(newest_before, max(held) + 1, width)), 'backfill gap'

        finally:
            probe.terminate()
            probe.wait(timeout=5)
            error = probe.stderr.read()
            assert not error, error
        wait(lambda: [r for r in hub.hosts() if r[0] == identity and r[1] == '0'], 'disconnect not listed')
        local_live = (probe_data / 'live.v1.bin').read_bytes()
        pending_count = struct.unpack_from('<I', local_live, 104)[0]
        for index, ring in enumerate(RINGS):
            local, remote = (probe_data / ring).read_bytes(), (replica / ring).read_bytes()
            points, width, record_bytes, header_bytes = ring_geometry(local)
            assert remote[:header_bytes] == local[:header_bytes] and len(remote) == len(local), ring
            mine, theirs = records(local), records(remote)
            assert all(theirs.get(start) == record for start, record in mine.items()), ring
            unflushed = pending_count if index == 0 else 0
            assert len(set(theirs) - set(mine)) <= unflushed + 1, (ring, len(set(theirs) - set(mine)))
            for start in theirs:  # replicas never invent buckets
                assert start % width == 0
        ring_headers = [(probe_data / ring).read_bytes()[:ring_geometry((probe_data / ring).read_bytes())[3]]
                        for ring in RINGS]
        check_protocol(hub, ring_headers, local_live)
        hub.alive()
        hub.stop()
        log = hub.process.stderr.read().decode()
        for line in ('old-version: protocol version 2; this hub accepts 3; upgrade the device',
                     'new-version: protocol version 4; this hub accepts 3; upgrade the hub',
                     'python-probe: history format version 2; this hub accepts 1; upgrade the hub',
                     'python-probe: live format version 2; this hub accepts 1; upgrade the hub'):
            assert f'quectop-hub: rejected device {line}\n' in log, (line, log)
        # Without --http the hub only maintains files; hosts.txt survives restarts.
        restarted = Hub(hub_command, hub.directory, http=False)
        restarted.start()
        assert {row[0]: row[1:] for row in restarted.hosts()}[identity] == ['0', name], 'stored hosts listed offline'
        check_static_server(hub.directory, identity)
        restarted.stop()
        check_cors(hub_command, hub.directory, identity)
        check_replica_io(hub_command, root, ring_headers, local_live)
        check_keys(hub_command, probe_command, root, ring_headers)
        check_impostor(probe_command, root)
    print('hub: replication, snapshots, resume after hub restart, built-in static serving and path '
          'rejection, --cors, device keys and Noise handshake (replay, impostor hub, noiseprotocol interop), '
          'DATA served by another static server, stream replacement and protocol rejection passed')


if __name__ == '__main__':
    main()
