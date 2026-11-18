#!/bin/bash
# Build a kagura RAM-boot image with the XNU payload in the ramdisk.
# The ADT hides physical CPU3 (0x101) from XNU so the shim's 40-second
# watchdog CPU stays alive and can reset a hung first-boot attempt.
set -euo pipefail
ROOT=/root/ios-xz
KERNEL=${1:?usage: prepare_xnu_boot.sh KERNEL [OUT.img]}
OUT=${2:-/mnt/d/xz/shim_kagura/xnu-boot.img}
WORK=${WORK:-/tmp/kagura-xnu-final}
CMDLINE=${CMDLINE:-'debug=0xc serial=3 cpus=1 keepsyms=1'}

mkdir -p "$WORK"
if [ "$(readlink -f "$KERNEL")" != "$(readlink -f "$WORK/kernel")" ]; then
	cp "$KERNEL" "$WORK/kernel"
fi
cd "$ROOT"
python3 tools/mkadt.py "$WORK/kagura-full.adt"
python3 - "$WORK/kagura-full.adt" "$WORK/kagura.adt" <<'PY'
import sys
sys.path.insert(0, '/root/ios-xz/tools')
import adt
root, _ = adt.parse(open(sys.argv[1], 'rb').read())
cpus = next(c for c in root.children if c.name == 'cpus')
cpus.children = [c for c in cpus.children if c.name != 'cpu3']
open(sys.argv[2], 'wb').write(adt.serialize(root))
print('ADT CPUs:', len(cpus.children))
PY
python3 tools/mkpayload.py "$WORK/kernel" "$WORK/kagura.adt" "$WORK/payload.bin" --cmdline "$CMDLINE"
(cd shim && bash build_kagura.sh)
(cd shim && python3 pack_kagura.py out-kagura/Image "$OUT" "$WORK/payload.bin")
sha256sum "$OUT"
