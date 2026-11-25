CC = cc
BUN = bun
PYTHON = python3
CFLAGS = -Os -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fstack-protector-strong -mstack-protector-guard=global -fno-unwind-tables -fno-asynchronous-unwind-tables -fpie -ffunction-sections -fdata-sections
LDFLAGS = -nostdlib -static-pie -Wl,-z,text,-z,relro,--gc-sections,--build-id=none,-z,noexecstack,-s
COMPILER_ARCH := $(firstword $(subst -, ,$(shell $(CC) -dumpmachine)))
TARGET ?= linux-$(if $(filter arm armv% thumb%,$(COMPILER_ARCH)),armv6,$(patsubst x86_64,amd64,$(patsubst aarch64,arm64,$(COMPILER_ARCH))))
ifeq ($(TARGET),linux-armv6)
ARCHFLAGS = -march=armv6 -marm -mfloat-abi=soft
endif

CRYPTO_SOURCES = vendor/monocypher/monocypher.c vendor/monocypher/monocypher-ed25519.c
HUB_SOURCES = hub/main.c hub/assets.S boot.c arm.c start.S $(CRYPTO_SOURCES)
# Shared runtime helpers the hub does not call are dropped by --gc-sections.
HUB_CFLAGS = $(CFLAGS) -Wno-unused-function

.PHONY: all clean release
all: quectop quectop-hub

.build/assets.h: Makefile scripts/assets.py web/index.html web/style.css web/app.js web/codec.js
	mkdir -p .build
	$(BUN) build web/app.js --target browser --minify --banner '"use strict";' --outfile .build/app.js
	$(PYTHON) scripts/assets.py prepare .build

quectop: main.c $(wildcard *.h) boot.c arm.c start.S $(CRYPTO_SOURCES) $(wildcard vendor/monocypher/*.h)
	$(CC) $(CFLAGS) $(ARCHFLAGS) main.c boot.c arm.c start.S $(CRYPTO_SOURCES) $(LDFLAGS) -o $@

quectop-hub: $(HUB_SOURCES) $(wildcard hub/*.h) $(wildcard *.h) $(wildcard vendor/monocypher/*.h) .build/assets.h
	$(CC) $(HUB_CFLAGS) $(ARCHFLAGS) $(HUB_SOURCES) $(LDFLAGS) -o $@

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
