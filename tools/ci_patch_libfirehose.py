#!/usr/bin/env python3
"""Patch dxb/build.sh so libfirehose_kernel (libdispatch) is built with
ARMv8.0-safe atomics for arm64/arm64e slices.

libdispatch is built by its own xcodebuild target, so it does not inherit
xnu's MACHINE_FLAGS_ARM64_SWEET.  Without these flags its arm64 slice emits
LSE (ldadd/cas) and RCpc (ldapr) instructions, which fault on MSM8996/Kryo.
"""
from pathlib import Path
import sys

p = Path('dxb/build.sh')
s = p.read_text()

anchor = (
    "        sed -i '' 's|$(SDKROOT)/usr/local/include|$(FAKEROOT_DIR)/usr/local/include|g' "
    "\"${SRCROOT}/xcodeconfig/libfirehose_kernel.xcconfig\"\n"
    '        cd "${SRCROOT}"'
)

flags = '-mkernel -nostdinc -Wno-packed -Xclang -target-feature -Xclang -lse -Xclang -target-feature -Xclang -rcpc'
inject = (
    "        sed -i '' 's|$(SDKROOT)/usr/local/include|$(FAKEROOT_DIR)/usr/local/include|g' "
    "\"${SRCROOT}/xcodeconfig/libfirehose_kernel.xcconfig\"\n"
    f"        printf 'OTHER_CFLAGS[arch=arm64] = {flags}\\n"
    f"OTHER_CFLAGS[arch=arm64e] = {flags}\\n' >> \"${{SRCROOT}}/xcodeconfig/libfirehose_kernel.xcconfig\"\n"
    '        cd "${SRCROOT}"'
)

if anchor not in s:
    sys.exit('ci_patch_libfirehose: anchor not found in dxb/build.sh')
p.write_text(s.replace(anchor, inject, 1))
print('ci_patch_libfirehose: patched dxb/build.sh')
