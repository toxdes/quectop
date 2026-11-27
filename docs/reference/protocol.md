# Hub protocol reference (v3)

The probe streams to the hub over one TCP connection. All integers are little-endian. `protocol.h` defines the constants, `noise.h` the handshake, `uplink.h` the probe side, and `hub/intake.h` the hub side.

The hub accepts a probe whose protocol version is between its oldest supported version and its own version. This hub supports only version 3. The hub rejects other versions and logs which program to upgrade. The probe then keeps its history locally, retries, and sends the missing data once both versions match.

The data formats work the same way, but each hub currently supports only format version 1.

## Messages

Every message is a `u16` length, then that many bytes. The largest message is 65535 bytes.

## Connection sequence

1. **OPEN**, probe to hub, unencrypted:

	| Bytes | Field |
	| --- | --- |
	| 4 | `"QTOP"` |
	| 1 | Protocol version, `3` |
	| 1 | ID length, 1–64 |
	| n | Host ID, `[A-Za-z0-9_.-]`, no leading dot |

	The OPEN message is also the Noise prologue. A change to any byte makes the handshake fail.

2. **Noise message 1** (`psk, e`), probe to hub. Its encrypted payload is:

	| Bytes | Field |
	| --- | --- |
	| 4 | Effective publication period in milliseconds, rounded up to a whole number of sample intervals |
	| 1 | Host name length, 0–64 |
	| n | Host name |

3. **Noise message 2** (`e, ee`), hub to probe, with an empty payload. It is 48 bytes. If the probe cannot decrypt the message, it disconnects and sends nothing else.

4. **Frames.** Every later message is one frame, sealed with the transport keys from the handshake. Each sealed frame ends with a 16-byte tag.

## Handshake

The handshake is `Noise_NNpsk0_25519_ChaChaPoly_SHA512` from the [Noise Protocol Framework](https://noiseprotocol.org/noise.html), revision 34.

| Part | Choice |
| --- | --- |
| Pre-shared key | The device key from the keys file, 256 bits. All zeros without a keys file. |
| Key exchange | X25519. Both sides use a new 256-bit ephemeral key for each connection. |
| Cipher | ChaCha20-Poly1305 (IETF), 256-bit key, 128-bit tag |
| Nonce | 64-bit counter for each direction |
| Hash | SHA-512 |

### Authentication

The hub looks up the pre-shared key by the host ID in OPEN. Only a side that holds the key can complete the handshake. Each new connection uses new ephemeral keys, so a key that leaks later cannot decrypt recorded traffic.

### Replay

An attacker can replay message 1. The hub treats a device as connected only after the device's first sealed frame decrypts. Until then, the connection may stay open for 10 s.

### Visible data

The host ID in OPEN is not encrypted. The host name is encrypted.

## Frames

A frame is a `u8` type, a `u24` payload length, and the payload. A frame is at most 65515 bytes, so one sealed frame fills at most one message.

| Type | Name | Direction | Payload |
| ---: | --- | --- | --- |
| 2 | RING | probe to hub | `u8` tier (0–4), then the ring file's 64-byte header and device metadata. Sent once for each tier. |
| 3 | RESUME | hub to probe | Five `u32` values: for each tier, the start of the newest valid bucket the hub holds, or `0` for none. |
| 4 | RECORD | probe to hub | `u8` tier, then one bucket, byte for byte as the ring file stores it. |
| 5 | LIVE | probe to hub | A complete `live.v1.bin`. |
| 6 | DISKS | probe to hub | A complete `disks.v1.bin`. |

Tiers 0–4 are `1h`, `1d`, `7d`, `30d`, and `90d`.

The hub ignores a frame of any other type, so a new optional frame type needs no new protocol version. A change to an existing frame needs one. The probe closes the connection on any frame after RESUME, so the hub must not send a new frame type to a probe of an older version.

## Stream order

After the handshake, the two sides do these steps:

1. The probe sends five RING frames.
2. The hub checks each ring's geometry against the device layout. It scans matching rings with one link in chunks between socket polls and recreates the others.
3. The hub sends RESUME.
4. For each tier, the probe sends the buckets after the hub's newest, oldest first. If the hub's newest bucket is at or beyond the probe's active window, the probe resends the retained window. Buckets not yet flushed come from memory.
5. The probe sends the latest LIVE and DISKS snapshots.
6. The probe sends each new bucket and snapshot when it writes them.

While a tier is still sending old buckets, the probe does not send that tier's new buckets directly. The catch-up reaches them later, so the hub's newest bucket never runs ahead of a gap.

## Validation

The hub validates every frame before it writes to a replica. It checks the type, length, magic number, version, and checksum. LIVE must match the geometry announced in RING. The hub writes each bucket to the slot selected by its start time and replaces snapshots atomically.

The hub closes the connection when it receives any of these messages:

- A frame that does not decrypt.
- A frame whose length does not match its header.
- A RESUME frame.
- A bad header or checksum in the payload.

## Conformance

`test/crypto.py` checks the cryptography against suites written outside this project:

- [Wycheproof](https://github.com/C2SP/wycheproof) vectors for ChaCha20-Poly1305, X25519, HMAC-SHA512, and HKDF-SHA512.
- The [cacophony](https://github.com/haskell-cryptography/cacophony) test vector for `Noise_NNpsk0_25519_ChaChaPoly_SHA512`.
- Random sessions against the [`noiseprotocol`](https://pypi.org/project/noiseprotocol/) Python package, including tampered messages and wrong keys.

`test/hub.py` runs a real probe against a Python hub, and a Python probe against the real hub.
