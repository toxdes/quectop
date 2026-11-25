# Run the tests

The tests build their own binaries, run them, and compare the results with fixtures and external suites. Run them from the repository root.

## Requirements

| Test | Needs |
| --- | --- |
| All | Python 3, [uv](https://docs.astral.sh/uv/), Bun, and a C compiler |
| `test/abi.py` | Clang, and LLD on your `PATH` or passed with `--linker` |
| ARM targets | Clang, LLD, `qemu-aarch64`, and `qemu-arm` |
| `test/crypto.py` | Network access the first time, to download the test vectors into `.build/vectors` |
| `test/resolve.py` | Unprivileged user namespaces (`unshare -rnm`) |

## Test this machine

```sh
make
python3 test/abi.py --target native
python3 test/verify.py
python3 test/services.py
uv run test/crypto.py
uv run test/hub.py
uv run test/resolve.py
```

| Script | Checks |
| --- | --- |
| `test/abi.py` | System calls, file structures, clocks, and polling |
| `test/verify.py` | Packed formats, CPU layouts, dashboard decoding, and the probe's run behavior |
| `test/services.py` | Service discovery and memory figures, from fixtures and a real process |
| `test/crypto.py` | The cryptography, against Wycheproof, cacophony, and `noiseprotocol` |
| `test/hub.py` | A real probe against a real hub, plus rejected keys and bad frames |
| `test/resolve.py` | Name lookup through `/etc/hosts` and DNS |

## Test ARM builds

1. Build both ARM releases. See [Build from source](build-from-source.md#cross-compile-for-arm).
2. Run the tests under QEMU:

	```sh
	LLD=/path/to/ld.lld
	python3 test/abi.py --target arm64 --linker $LLD
	python3 test/abi.py --target armv6 --linker $LLD
	python3 test/verify.py --arm-binary dist/quectop-linux-arm64/quectop --arm32-binary dist/quectop-linux-armv6/quectop
	python3 test/verify.py --fixture-target arm64 --fixtures-only --linker $LLD
	python3 test/verify.py --fixture-target armv6 --fixtures-only --linker $LLD
	uv run test/crypto.py --target amd64 --target arm64 --target armv6 --linker $LLD
	uv run test/hub.py --probe 'qemu-aarch64 dist/quectop-linux-arm64/quectop' --hub 'qemu-aarch64 dist/quectop-linux-arm64/quectop-hub'
	```

QEMU uses the host's kernel. The tests check ARM code, but they do not measure speed on real ARM hardware.
