# Build from source

You need a C compiler (GCC or Clang), GNU Make, Python 3, and [Bun](https://bun.sh). The binaries that you build need none of these tools at run time.

## Build for this machine

```sh
make
```

The build writes `./quectop` and `./quectop-hub`. Both are static and have no library dependencies.

## Build a release archive

```sh
make release
```

The archive is `dist/quectop-TARGET.tar.gz`. It contains both binaries. The target name comes from the compiler, such as `linux-amd64`. To name it yourself, add `TARGET=linux-amd64`.

## Cross-compile for ARM

Clang and LLD can build for ARM without a target sysroot. Add `-B` when you change the compiler or the architecture, so that `make` rebuilds everything.

For 64-bit ARM (AArch64):

```sh
make -B CC='clang --target=aarch64-linux-gnu --ld-path=/path/to/ld.lld' release TARGET=linux-arm64
```

For 32-bit ARM:

```sh
make -B CC='clang --target=arm-linux-gnueabi --ld-path=/path/to/ld.lld' release TARGET=linux-armv6
```

The ARMv6 build runs on little-endian ARMv6, ARMv7, and ARMv8 systems with 32-bit support. It needs Linux 5.1 or later. It does not run on ARMv5, Thumb-only microcontrollers, big-endian ARM, or the old ARM OABI.

## Copy the binaries to a device

```sh
scp dist/quectop-linux-arm64/quectop user@device.example.com:
```

The device needs only the `quectop` binary. Copy `quectop-hub` only to the machine that runs the hub.

## Next steps

- To check the build, see [Run the tests](run-the-tests.md).
- To start monitoring, see [Monitor another machine](monitor-another-machine.md).
