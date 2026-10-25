CC = cc
BUN = bun
PYTHON = python3
CFLAGS = -Os -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-pie -ffunction-sections -fdata-sections
LDFLAGS = -nostdlib -static -Wl,-no-pie,--gc-sections,--build-id=none,-z,noexecstack,-s
COMPILER_ARCH := $(firstword $(subst -, ,$(shell $(CC) -dumpmachine)))
TARGET ?= linux-$(if $(filter arm armv% thumb%,$(COMPILER_ARCH)),armv6,$(patsubst x86_64,amd64,$(patsubst aarch64,arm64,$(COMPILER_ARCH))))
ifeq ($(TARGET),linux-armv6)
ARCHFLAGS = -march=armv6 -marm -mfloat-abi=soft
endif

.PHONY: all clean release
all: quectop

.build/assets.h: scripts/assets.py web/index.html web/style.css web/app.js
	mkdir -p .build
	$(BUN) build web/app.js --target browser --minify --outfile .build/app.js
	$(PYTHON) scripts/assets.py prepare .build

quectop: main.c linux.h services.h arm.c start.S .build/assets.h scripts/assets.py
	$(CC) $(CFLAGS) $(ARCHFLAGS) main.c arm.c start.S $(LDFLAGS) -o .build/quectop.elf
	$(PYTHON) scripts/assets.py pack .build/quectop.elf $@ .build

clean:
	rm -f quectop
	rm -rf .build

release: quectop
	mkdir -p dist/quectop-$(TARGET)
	cp quectop dist/quectop-$(TARGET)/.quectop.new
	mv -f dist/quectop-$(TARGET)/.quectop.new dist/quectop-$(TARGET)/quectop
	tar -czf dist/quectop-$(TARGET).tar.gz -C dist quectop-$(TARGET)/quectop
