#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["noiseprotocol==0.3.1"]
# ///
"""Probe hub-name lookup, hermetic: runs in user, network and mount namespaces
with a fake /etc/hosts, /etc/resolv.conf and DNS server."""
import argparse
from pathlib import Path
import select
import shlex
import socket
import struct
import subprocess
import sys
import tempfile
import threading

from hub import Hub, ROOT, free_port, wait

# Line 2 overflows the 4 KiB reader; its tail must not be read as an entry.
HOSTS = ('# comment\n' + '1.1.1.1 ' + 'x' * 4088 + ' 10.9.9.9 from-hosts\n'
         '127.0.0.1\tlocalhost  from-hosts # trailing comment\n'
         '127.0.0.1 last-line-no-newline')
# The first nameserver refuses (nothing listens), so lookups move to the second.
RESOLV = 'search example.test\nnameserver ::1\nnameserver 127.0.0.3\nnameserver 127.0.0.1\n'


class Dns:
    """A/IN answers via a CNAME and compression pointers; silent for slow.test."""

    def __init__(self, names):
        self.names, self.queries = names, []
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', 53))
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            query, peer = self.sock.recvfrom(512)
            end = 12
            while query[end]:
                end += query[end] + 1
            labels, at = [], 12
            while query[at]:
                labels.append(query[at + 1:at + 1 + query[at]].decode())
                at += query[at] + 1
            name = '.'.join(labels).lower()
            self.queries.append(name)
            if name == 'slow.test':
                continue
            question = query[12:end + 5]
            address = self.names.get(name)
            header = query[:2] + struct.pack('>HHHHH', 0x8180 if address else 0x8183, 1,
                                             2 if address else 0, 0, 0)
            answers = b''
            if address:
                alias = b'\x04real\xc0\x0c'
                answers = (struct.pack('>HHHIH', 0xc00c, 5, 1, 60, len(alias)) + alias +
                           struct.pack('>HHHIH', 0xc00c + 12 + len(question), 1, 1, 60, 4) +
                           socket.inet_aton(address))
            self.sock.sendto(header + question + answers, peer)


def bind_file(contents, target, directory):
    path = directory / Path(target).name
    path.write_text(contents)
    subprocess.run(['mount', '--bind', str(path), target], check=True)


def run_probe(probe_command, directory, hub, target, connects=True):
    data = directory / ('probe-' + target.replace(':', '-'))
    probe = subprocess.Popen(probe_command + [
        '--data-dir', str(data), '--interval-ms', '100', '--publish-ms', '200',
        '--interface', 'lo', '--hub', target], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        if connects:
            wait(lambda: hub.alive() and [r for r in hub.hosts() if r[1] == '1'], f'{target} not connected')
        return probe, data
    except BaseException:
        probe.kill()
        raise


def stop(probe, hub):
    probe.terminate()
    probe.wait(timeout=5)
    error = probe.stderr.read()
    assert not error, error
    wait(lambda: not [r for r in hub.hosts() if r[1] == '1'], 'disconnect not listed')


def inside(probe_command, hub_command):
    subprocess.run(['ip', 'link', 'set', 'lo', 'up'], check=True)
    with tempfile.TemporaryDirectory(prefix='quectop-resolve-') as temporary:
        root = Path(temporary)
        bind_file(HOSTS, '/etc/hosts', root)
        bind_file(RESOLV, '/etc/resolv.conf', root)
        dns = Dns({'hub.example.test': '127.0.0.1'})
        hub = Hub(hub_command, root / 'hub')
        hub.start()
        port = hub.probes

        for target in (f'From-Hosts:{port}', f'last-line-no-newline:{port}'):
            stop(run_probe(probe_command, root, hub, target)[0], hub)
        assert not dns.queries, f'/etc/hosts names reached DNS: {dns.queries}'

        probe, _ = run_probe(probe_command, root, hub, f'HUB.Example.Test.:{port}')
        assert dns.queries == ['hub.example.test'], dns.queries
        dns.names['hub.example.test'] = '192.0.2.1'
        hub.stop()
        hub.start()
        wait(lambda: select.select([probe.stderr], [], [], 0)[0], 'probe did not reject a non-loopback resolution')
        assert b'outside loopback needs a nonzero device key' in probe.stderr.readline()
        assert not [r for r in hub.hosts() if r[1] == '1']
        dns.names['hub.example.test'] = '127.0.0.1'
        wait(lambda: [r for r in hub.hosts() if r[1] == '1'], 'did not reconnect')
        assert dns.queries.count('hub.example.test') >= 3, 'reconnect did not look the name up again'
        stop(probe, hub)

        # Failed and unanswered lookups retry with backoff and never stall sampling.
        for name in ('missing.test', 'slow.test'):
            probe, data = run_probe(probe_command, root, hub, f'{name}:{port}', connects=False)
            try:
                wait(lambda: dns.queries.count(name) >= 2, f'{name} not retried', 20)
                live = data / 'live.v1.bin'
                wait(live.exists, 'no snapshot')
                before = struct.unpack_from('<Q', live.read_bytes(), 24)[0]
                wait(lambda: struct.unpack_from('<Q', live.read_bytes(), 24)[0] > before, 'sampling stalled')
                assert not [r for r in hub.hosts() if r[1] == '1']
            finally:
                stop(probe, hub)
        hub.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', default=str(ROOT / 'quectop'), help='probe command')
    parser.add_argument('--hub', default=str(ROOT / 'quectop-hub'), help='hub command')
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.inside:
        inside(shlex.split(args.probe), shlex.split(args.hub))
        return
    for bad in ('bad name:7070', 'a..b:7070', 'host:0', 'host', ':7070', 'x' * 64 + '.test:1'):
        result = subprocess.run(shlex.split(args.probe) + ['--hub', bad], capture_output=True, timeout=5)
        assert result.returncode == 1 and b'--hub needs HOST:PORT' in result.stderr, bad
    subprocess.run(['unshare', '-rnm', sys.executable, __file__, '--inside',
                    '--probe', args.probe, '--hub', args.hub], check=True)
    print('resolve: /etc/hosts (comments, overlong and unterminated lines), DNS via CNAME and '
          'compression, case and trailing dot, nameserver failover, lookup on every reconnect, '
          'failed and unanswered lookups without stalled sampling, and invalid names passed')


if __name__ == '__main__':
    main()
