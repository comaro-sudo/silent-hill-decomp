#!/usr/bin/env bash
# Builds ogc_port/tools/apploader.bin with devkitPPC (WSL). Raw image, linked at 0x81200000.
set -e
D="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH=/opt/devkitpro/devkitPPC/bin:$PATH
powerpc-eabi-gcc -Os -mcpu=750 -msoft-float -nostdlib -ffreestanding -fno-zero-initialized-in-bss \
    -fno-asynchronous-unwind-tables -Wl,-T,"$D/apploader.ld" -Wl,--build-id=none \
    "$D/apploader_stub.c" -o "$D/apploader.elf"
powerpc-eabi-objcopy -O binary "$D/apploader.elf" "$D/apploader.bin"
powerpc-eabi-nm "$D/apploader.elf" | grep -w entry
ls -l "$D/apploader.bin"
