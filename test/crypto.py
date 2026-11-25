#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["noiseprotocol==0.3.1"]
# ///
"""Checks the shipped crypto against suites we did not write: Wycheproof for the
Monocypher primitives, cacophony's Noise vector, and live sessions against the
noiseprotocol package. Vectors are pinned by commit and SHA-256."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import shlex
import subprocess
import tempfile
import urllib.request

import warnings

from noise.connection import Keypair, NoiseConnection

warnings.filterwarnings('ignore', message='One of ephemeral keypairs')  # fixed test keys

ROOT = Path(__file__).resolve().parent.parent
PROTOCOL = b'Noise_NNpsk0_25519_ChaChaPoly_SHA512'
CACOPHONY = 'https://raw.githubusercontent.com/haskell-cryptography/cacophony/8ee9d41e34a1a596cfa3ab12aa4069ff87dc1247/vectors/'
WYCHEPROOF = 'https://raw.githubusercontent.com/C2SP/wycheproof/65ca494727a52944c204441c860b039e6786b99b/testvectors_v1/'
VECTORS = {
    'cacophony.txt': (CACOPHONY, '3bde7c09a6f349ee11c825c50fcc02649f8f02a47c857a459206b357f9386cae'),
    'chacha20_poly1305_test.json': (WYCHEPROOF, 'fe61d25f90e1bde4461d00eafe61049e5f29bd999f36b766df9cda90906ad53d'),
    'x25519_test.json': (WYCHEPROOF, '35c3f5231cf25cc640b524d403461deee9e49441d5d915a3a25b2c8ff5adbe7d'),
    'hmac_sha512_test.json': (WYCHEPROOF, 'b6c90477bdb4a6fc8ee3d1f7b2c0b69a8dfffab34718abaa6cabd71cc2ba1207'),
    'hkdf_sha512_test.json': (WYCHEPROOF, 'bb9a21f4e86041caf5d7792b030349f8ff289087f195b2fbc0fc0afc39deca6f'),
}
TARGETS = {
    'amd64': (['cc'], []),
    'arm64': (['clang', '--target=aarch64-linux-gnu'], ['qemu-aarch64']),
    'armv6': (['clang', '--target=arm-linux-gnueabi', '-march=armv6', '-marm', '-mfloat-abi=soft'],
              ['qemu-arm', '-cpu', 'arm1176']),
}


def vector(name):
    """A pinned vector file, downloaded once into .build/vectors."""
    base, digest = VECTORS[name]
    path = ROOT / '.build' / 'vectors' / name
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(base + name, timeout=60) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != digest:
            raise SystemExit(f'{name}: checksum mismatch')
        path.write_bytes(data)
    data = path.read_bytes()
    assert hashlib.sha256(data).hexdigest() == digest, name
    return json.loads(data)


def hexes(*fields):
    return ' '.join(f.hex() or '-' for f in fields)


def wycheproof(add):
    for group in vector('chacha20_poly1305_test.json')['testGroups']:
        for t in group['tests']:
            if group['ivSize'] != 96:  # Noise nonces are 96 bits
                continue
            key, iv, aad, msg, ct, tag = (bytes.fromhex(t[k]) for k in ('key', 'iv', 'aad', 'msg', 'ct', 'tag'))
            if t['result'] == 'valid':
                add(f's {hexes(key, iv, aad, msg)}', (ct + tag).hex())
                add(f'o {hexes(key, iv, aad, ct, tag)}', msg.hex())
            else:
                add(f'o {hexes(key, iv, aad, ct, tag)}', 'fail')
    for group in vector('x25519_test.json')['testGroups']:
        for t in group['tests']:
            add(f"x {t['private']} {t['public']}", t['shared'])
    for group in vector('hmac_sha512_test.json')['testGroups']:
        for t in group['tests']:
            line = f"h {t['key'] or '-'} {t['msg'] or '-'}"
            if t['result'] == 'valid':
                add(line, lambda out, tag=t['tag']: out.startswith(tag))
            else:
                add(line, lambda out, tag=t['tag']: not out.startswith(tag))
    for group in vector('hkdf_sha512_test.json')['testGroups']:
        for t in group['tests']:
            if t['result'] == 'valid':  # the rest ask for more than 255 blocks, which Noise never does
                add(f"k {t['ikm'] or '-'} {t['salt'] or '-'} {t['info'] or '-'} {t['size']:04x}", t['okm'])


def session_line(psk, prologue, initiator_e, responder_e, payloads):
    return 'n ' + hexes(psk, prologue, initiator_e, responder_e, *payloads)


def cacophony(add):
    found = [v for v in vector('cacophony.txt')['vectors'] if v['protocol_name'] == PROTOCOL.decode()]
    assert len(found) == 1, 'cacophony vector missing'
    for v in found:
        assert v['init_psks'] == v['resp_psks'] and v['init_prologue'] == v['resp_prologue']
        payloads = [bytes.fromhex(m['payload']) for m in v['messages']]
        expected = ' '.join(m['ciphertext'] for m in v['messages']) + ' ' + v['handshake_hash']
        add(session_line(bytes.fromhex(v['init_psks'][0]), bytes.fromhex(v['init_prologue']),
                         bytes.fromhex(v['init_ephemeral']), bytes.fromhex(v['resp_ephemeral']), payloads), expected)


def reference_session(psk, prologue, initiator_e, responder_e, payloads):
    """The same session through the independent noiseprotocol implementation."""
    sides = []
    for initiator, ephemeral in ((True, initiator_e), (False, responder_e)):
        side = NoiseConnection.from_name(PROTOCOL)
        side.set_as_initiator() if initiator else side.set_as_responder()
        side.set_psks(psk=psk)
        side.set_prologue(prologue)
        side.set_keypair_from_private_bytes(Keypair.EPHEMERAL, ephemeral)
        side.start_handshake()
        sides.append(side)
    messages = []
    for index, payload in enumerate(payloads):
        writer, reader = (sides[0], sides[1]) if index % 2 == 0 else (sides[1], sides[0])
        if index < 2:
            message = writer.write_message(payload)
            assert reader.read_message(message) == payload
        else:
            message = writer.encrypt(payload)
            assert reader.decrypt(message) == payload
        messages.append(message.hex())
    return ' '.join(messages) + ' ' + sides[0].get_handshake_hash().hex()


def live(add, rng, sessions):
    for _ in range(sessions):
        size = lambda: rng.choice((0, 1, 15, 16, 17, 63, 64, 65, rng.randrange(0, 2000), 65535 - 48))
        psk, prologue = rng.randbytes(32), rng.randbytes(rng.randrange(0, 80))
        initiator_e, responder_e = rng.randbytes(32), rng.randbytes(32)
        payloads = [rng.randbytes(size()) for _ in range(rng.randrange(2, 9))]
        add(session_line(psk, prologue, initiator_e, responder_e, payloads),
            reference_session(psk, prologue, initiator_e, responder_e, payloads))
        # Our responder against their initiator; any flipped bit must be refused.
        sender = NoiseConnection.from_name(PROTOCOL)
        sender.set_as_initiator()
        sender.set_psks(psk=psk)
        sender.set_prologue(prologue)
        sender.start_handshake()
        payload = rng.randbytes(rng.randrange(0, 200))
        message = bytearray(sender.write_message(payload))
        wrong_psk = rng.random() < 0.2
        if not wrong_psk and rng.random() < 0.5:
            bit = rng.randrange(len(message) * 8)
            message[bit // 8] ^= 1 << bit % 8
            add(f'r {hexes(psk, prologue, bytes(message))}', 'fail')
        elif wrong_psk:
            add(f'r {hexes(rng.randbytes(32), prologue, bytes(message))}', 'fail')
        else:
            add(f'r {hexes(psk, prologue, bytes(message))}', payload.hex())


def build(target, linker, directory):
    compiler, runner = TARGETS[target]
    if target != 'amd64' and linker:
        compiler = compiler + [f'--ld-path={linker}']
    flags = next(l for l in (ROOT / 'Makefile').read_text().splitlines() if l.startswith('CFLAGS ='))
    links = next(l for l in (ROOT / 'Makefile').read_text().splitlines() if l.startswith('LDFLAGS ='))
    binary = directory / f'crypto-{target}'
    subprocess.run(compiler + shlex.split(flags.split('=', 1)[1]) + ['-Wno-unused-function'] + [
        str(ROOT / name) for name in ('test/crypto.c', 'vendor/monocypher/monocypher.c',
                                      'vendor/monocypher/monocypher-ed25519.c', 'boot.c', 'arm.c', 'start.S')
    ] + shlex.split(links.split('=', 1)[1]) + ['-o', str(binary)], check=True)
    return runner + [str(binary)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=TARGETS, action='append', help='default: amd64')
    parser.add_argument('--linker', help='ld.lld for ARM targets')
    parser.add_argument('--sessions', type=int, default=1000)
    args = parser.parse_args()
    cases = []
    add = lambda line, check: cases.append((line, check))
    wycheproof(add)
    primitives = len(cases)
    cacophony(add)
    live(add, random.Random(0x5eed), args.sessions)
    with tempfile.TemporaryDirectory(prefix='quectop-crypto-') as temporary:
        for target in args.target or ['amd64']:
            command = build(target, args.linker, Path(temporary))
            out = []
            for at in range(0, len(cases), 50):  # batches fit the harness's 16 MiB input
                batch = cases[at:at + 50]
                stdin = ''.join(line + '\n' for line, _ in batch).encode()
                got = subprocess.run(command, input=stdin, capture_output=True, check=True).stdout.decode()
                out += got.split('\n')[:-1]
            failed = [line[:70] for (line, check), got in zip(cases, out)
                      if not (check(got) if callable(check) else got == check)]
            assert len(out) == len(cases) and not failed, (target, len(failed), failed[:3])
            print(f'crypto {target}: {primitives} Wycheproof vectors, the cacophony Noise vector and '
                  f'{len(cases) - primitives - 1} noiseprotocol cases passed')


if __name__ == '__main__':
    main()
