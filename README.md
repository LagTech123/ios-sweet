# ios-sweet-kagura - Made by AI

**Apple's iOS 16 XNU on a Sony Xperia XZ F8331.**

This project ports Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) to the Sony
Xperia XZ F8331 (`kagura`, Qualcomm MSM8996 / Snapdragon 820). iOS 16.7.16 (20H392) is the
userland target.

> Experimental bring-up, not a usable phone OS. Many things are missing or stubbed, and Apple's
> security model is mostly switched off. The root image is written into the `userdata`
> partition, so **it erases Android's user data**.

## Status (2026-11-23) — iOS userland reached; `seputil` stall isolated

The shim boots XNU on the Xperia XZ, the HFSX root mounts from eMMC, and Apple's **iOS 16.7.16
`launchd` runs its boot tasks**: root remounted RW, `lo0` up, `mount-phase-1` done, and
`data-protection` → `exec seputil`. The current payload is **0126** (`CI 45232214211`), which adds
`SWEET EXEC:` / `SWEET EXIT:` telemetry written straight into pstore. Log
`D:\\xz\\shimlogs\\xnu-0126-fulltrace-20261123.txt` (19,070 B) shows `launchd` exec,
`mount_hfs` exec + exit `0x0`, `mount` exit `0x0`, then `exec seputil` with no seputil exec/exit.
`SWEET FB: AppleCLCD 1080x1920 ... presented as 1125x2436` confirms IOMobileFramebuffer is
presenting. A RAM-only long-watchdog boot (180 s CPU3 / 205 s HW) produced a byte-identical log at
the stall, so this is a **genuine hang, not a watchdog cutoff**.

Disassembly of `/usr/libexec/seputil` (the `init_data_protection` symlink launched by launchd with
argc=1) shows it calls `IORegistryEntryFromPath("IODeviceTree:/arm-io/sep")` and, when that node is
absent, prints `No SEP present on this device` and exits 0. Our ADT has no `/arm-io/sep`; the hang
is at the registry `RLOCK` (`lck_rw_lock_shared(&gIORegistryLock)`) in `IORegistryEntry::fromPath`.
No `AppleSEPManager` stub is needed — seputil never calls `IOServiceMatching` in this path.
**0127** (`3eadd6c`, CI 45247763407) adds a no-SEP fast-path in `is_io_registry_entry_from_path`
plus `SWEET SEP:` before/after-RLOCK IOLogs (`strstr` is unavailable in the kernel; `strncmp` is
used). Awaiting verification and a p54 payload write.

**0127 and 0128 were deployed and disproved the earlier theory.** The registry `RLOCK` is fine (all 18 before/after pairs complete) and seputil never asks for `/arm-io/sep`. 0128 added fine-grained exec markers and the sailfish IOKit-quiesce fix; the log now ends at `exec seputil` followed by `SWEET EXECP: 0 in process_signature` — the stall is **inside `process_signature()`** in `bsd/kern/kern_exec.c`, after the `exec` printf and before it can return. 0129 (pushed, CI 45259406943) adds six markers inside that function to name the exact call. The eMMC driver was audited and cleared (CMD17 sector loop, 64-bit LBA, per-sector status clears).

**0129/0130:** seputil passes every marker inside `process_signature()` (through `0g before return`) but never reaches the `1 after process_signature` marker in `__mac_execve()`. An atomic pstore tee (0130) produced an identical pattern, proving this is a real control-flow difference: `process_signature()` has two call sites — `posix_spawn()` at `kern_exec.c:4461` and `__mac_execve()` at 5053 — and seputil takes the posix_spawn path. 0131 (pushed, CI 45269294790) instruments both call sites and also fixes the live console overlay to draw into the presented surface.

**0135 sampler changed the diagnosis:** seputil is runnable in a shared-cache page fault (`esr 0x92000007`) while the single-sector CMD17 eMMC driver delivers only ~1.8-3.6 MB/s, so dyld simply cannot finish inside the 40 s watchdog — seputil is slow, not wedged. 0136 (pushed, CI 45344986056) switches to CMD23+CMD18 multi-block reads with auto-CMD12 for ~10-20x throughput, with `emmcmulti=0` / `emmccmd23=0` boot-arg rollback. The `ignite() returned 25` preboot warning is normal and also present on the working sailfish port.

| Area | State |
| --- | --- |
| Boot chain | Sony ABL → shim (`SWEETXNU` payload loader) → XNU at EL1, MMU off |
| Early console | ramoops at `0xa7fbe000` + live LCD glyphs; all kernel/daemon console bytes tee'd into pstore |
| XNU early boot | Full `bsd_init()` completes; global `ml_set_max_cpus(1)` avoids the single-CPU wait wedge |
| eMMC / storage | `kagura_emmc.c` (0085) reads GPT + CMD17/CMD24; `/dev/kaguraemmc0` is the root device |
| Root filesystem | HFSX image at p54 LBA 0, SWEETXNU payload at LBA 46,563,000; XNU remounts `/` RW |
| Userland | **`launchd` running boot tasks**; blocked in `data-protection` at `seputil` → registry RLOCK |
| Display | Shim MDP5 + live FB glyphs; `sweet_early_display_commit()` (CTL0_FLUSH/START) fires on every `AppleCLCD::present()` |

Boot args required: `rd=kaguraemmc0 emmcroot=userdata rootrw=1 emmcrw=1
unentitled_ios_sim_launch=1 cs_enforcement_disable=1 amfi_get_out_of_my_way=1` — the last three
are needed because the kernel is built `XNU_TARGET_OS_OSX` and otherwise kills iOS binaries with
`EXEC_EXIT_REASON_WRONG_PLATFORM` (namespace 9 code 0xe).

## Related ports

- [joranv1/ios-sailfish](https://github.com/joranv1/ios-sailfish) — sibling fork of the same
  `dipa-studio/ios-sweet` upstream, running iOS 16.7.16 on a Google Pixel 1 (Snapdragon 821 /
  Adreno 530, the same GPU family as kagura). It reaches the lock screen with GPU-accelerated
  CoreAnimation, Wi-Fi, touch and battery. Its patch `0166` identifies an iOS-specific IOKit bug
  relevant to our current `seputil` stall: with no `kernelmanagerd`,
  `OSKext::iokitDaemonAvailable()` leaves a registry busy-count hold that is never released, so
  IOKit never quiesces. Our tree carries the same unguarded code; a 0128 port is planned. The same
  patch adds the `IOAcceleratorES` service stub needed later for backboardd. Its storage driver is
  UFS, so that part is not portable to kagura's eMMC.
- [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz) — source of the IOKit-quiesce and
  `IOAcceleratorES` findings used by ios-sailfish.

## Build and test

- The kernel is built by GitHub Actions (`.github/workflows/xnu.yml`, `blacktop/darwin-xnu-build`):
  macOS **13.5** (xnu-8796), `DEVELOPMENT`, `SWEET`, with `xnu/patches/*` applied in order.
  Every run must pass the LSE check — MSM8996/Kryo is ARMv8.0-A, so any leftover `ldadd`/`cas`
  class instruction or ARMv8.3 `rcpc` load is a hard fault on this device. Check the build log for
  `Skipping patch ...-kagura` before trusting a successful run, and verify marker strings in the
  built kernel rather than relying on the patch list alone.
- The shim is built with `shim/build_kagura.sh` and packed with `shim/pack_kagura.py` (stock Sony
  608-byte header, `ABS64: 0`). `tools/mkadt.py` generates the device tree XNU receives.
- Bring-up uses a kernel payload staged in the `userdata` partition (`mmcblk0p54`); the shim finds
  it, copies the Mach-O segments and jumps. The boot partition is never flashed; bring-up boots
  are RAM-only (`fastboot boot`).
- After a boot, read the log from TWRP as `/sys/fs/pstore/console-ramoops`. Do not overwrite an
  existing log — save each run under a new name.

## Known problems

- The 0084 framebuffer/ramoops aliases use 32 MB block descriptors with unaligned physical bases,
  so they point at the wrong RAM. Patch 0096 avoids them by keeping identity pointers; a proper
  mapped-alias fix is still needed for the later console handoff.
- `mkadt.py` still advertises the memory map of the reference device; the payload is loaded into
  a region that is not reserved for it.
- The platform expert still matches the reference device string.
- The eMMC driver is present but has never been runtime-tested, so no root filesystem has been
  mounted yet.
- Display and userland work are downstream of reaching a stable early boot.

## Warnings

- Only the `userdata` partition is ever written during experiments. Never touch the bootloader,
  `TA`, `modemst1`/`modemst2`, `fsg`, `persist` or the partition tables.
- Do not read MDSS/MDP/DSI registers directly — it hangs the bus and forces a power cycle.

`NOTES.md` describes the unrelated Redmi `sweet` reference port; its hardware details do not
apply to `kagura`.

## Made by
- Claude (Free tier with MCP Connector)
- OpenCode (DeepSeek V4.1 Flash, MiMo-V2.6-Flash, Big Pickle, Hy4 Flash)
