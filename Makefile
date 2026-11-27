CC = cc
BUN = bun
PYTHON = python3
VERSION := $(shell cat VERSION)
VERSION_CPPFLAGS = -DQUECTOP_VERSION='"$(VERSION)"'
CFLAGS = -Os -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fstack-protector-strong -mstack-protector-guard=global -fno-unwind-tables -fno-asynchronous-unwind-tables -fpie -ffunction-sections -fdata-sections
LDFLAGS = -nostdlib -static-pie -Wl,-z,text,-z,relro,--gc-sections,--build-id=none,-z,noexecstack,-s
COMPILER_ARCH := $(firstword $(subst -, ,$(shell $(CC) -dumpmachine)))
TARGET ?= linux-$(if $(filter arm armv% thumb%,$(COMPILER_ARCH)),armv6,$(patsubst x86_64,amd64,$(patsubst aarch64,arm64,$(COMPILER_ARCH))))
ifeq ($(TARGET),linux-armv6)
ARCHFLAGS = -march=armv6 -marm -mfloat-abi=soft
endif
ifeq ($(COMPILER_ARCH),aarch64)
# Keep RELRO and writable data on separate pages even on 64 KiB kernels.
ARCHLDFLAGS = -Wl,-z,max-page-size=65536,-z,common-page-size=65536
endif

CRYPTO_SOURCES = vendor/monocypher/monocypher.c vendor/monocypher/monocypher-ed25519.c
HUB_SOURCES = hub/main.c hub/assets.S boot.c arm.c start.S $(CRYPTO_SOURCES)
# Shared runtime helpers the hub does not call are dropped by --gc-sections.
HUB_CFLAGS = $(CFLAGS) -Wno-unused-function

.PHONY: all clean release release-packages
all: quectop quectop-hub

.build/assets.h: Makefile scripts/assets.py $(wildcard web/*) $(wildcard web/icons/*)
	mkdir -p .build
	$(BUN) build web/app.js --target browser --minify --banner '"use strict";' --outfile .build/app.js
	$(BUN) build web/style.css --minify --outfile .build/style.css
	$(PYTHON) scripts/assets.py prepare .build

quectop: Makefile VERSION main.c $(wildcard *.h) boot.c arm.c start.S $(CRYPTO_SOURCES) $(wildcard vendor/monocypher/*.h)
	$(CC) $(VERSION_CPPFLAGS) $(CFLAGS) $(ARCHFLAGS) main.c boot.c arm.c start.S $(CRYPTO_SOURCES) $(LDFLAGS) $(ARCHLDFLAGS) -o $@

quectop-hub: VERSION $(HUB_SOURCES) $(wildcard hub/*.h) $(wildcard *.h) $(wildcard vendor/monocypher/*.h) .build/assets.h
	$(CC) $(VERSION_CPPFLAGS) $(HUB_CFLAGS) $(ARCHFLAGS) $(HUB_SOURCES) $(LDFLAGS) $(ARCHLDFLAGS) -o $@

clean:
	rm -f quectop quectop-hub
	rm -rf .build

release: quectop quectop-hub
	mkdir -p dist/quectop-$(TARGET)
	for binary in quectop quectop-hub; do \
	  cp $$binary dist/quectop-$(TARGET)/.$$binary.new && \
	  mv -f dist/quectop-$(TARGET)/.$$binary.new dist/quectop-$(TARGET)/$$binary || exit 1; \
	done
	tar -czf dist/quectop-$(TARGET).tar.gz -C dist quectop-$(TARGET)/quectop quectop-$(TARGET)/quectop-hub

release-packages:
	./yesb/build_all.py
	mkdir -p dist/packages/hub/dist
	cp VERSION packaging/hub/release.toml packaging/hub/PKGBUILD* dist/packages/hub/
	cp dist/packages/quectop-hub[-_]* dist/packages/hub/dist/
