#!/bin/bash
# Build the kagura shim and pack it into a stock Xperia boot.img v0.
set -euo pipefail
cd "$(dirname "$0")"
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-tree-loop-distribute-patterns -DKAGURA=1 -Wall -Wextra"
mkdir -p out-kagura
for f in start.S chain.S shim.c console.c mmu.c fdt.c loader.c plog.c fbsnap.c cc.c dsi.c; do
	$CC $CFLAGS -c "$f" -o "out-kagura/${f%.*}.o"
done
abs=$(aarch64-linux-gnu-readelf -r out-kagura/shim.o out-kagura/console.o out-kagura/mmu.o out-kagura/fdt.o out-kagura/loader.o out-kagura/plog.o out-kagura/fbsnap.o out-kagura/cc.o out-kagura/dsi.o out-kagura/chain.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
echo "ABS64: $abs"
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,-N out-kagura/start.o out-kagura/chain.o out-kagura/shim.o out-kagura/console.o out-kagura/mmu.o out-kagura/fdt.o out-kagura/loader.o out-kagura/plog.o out-kagura/fbsnap.o out-kagura/cc.o out-kagura/dsi.o -o out-kagura/shim.elf
$OBJCOPY -O binary out-kagura/shim.elf out-kagura/Image
size=$(aarch64-linux-gnu-nm out-kagura/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out-kagura/Image
python3 pack_kagura.py out-kagura/Image out-kagura/shim-boot.img
ls -l out-kagura/Image out-kagura/shim-boot.img
