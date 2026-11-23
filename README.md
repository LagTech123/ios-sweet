# ios-sweet-kagura - Made by AI

**Apple's iOS 16 XNU on a Sony Xperia XZ F8331.**

This project ports Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) to the Sony
Xperia XZ F8331 (`kagura`, Qualcomm MSM8996 / Snapdragon 820). iOS 16.7.16 (20H392) is the
userland target.

> Experimental bring-up, not a usable phone OS. Many things are missing or stubbed, and Apple's
> security model is mostly switched off. The root image is written into the `userdata`
> partition, so **it erases Android's user data**.

## Status (2026-11-23) — iOS userland reached

The shim boots the XNU kernel on the Xperia XZ, the HFSX root mounts from eMMC, and Apple's
**iOS 16.7.16 `launchd` runs its boot tasks**. The latest boot log
(`D:\\xz\\shimlogs\\xnu-0125-fulltrace-20261123.txt`, 18,932 B) shows:
`Darwin Bootstrapper Version 7.0.0`, root remounted read-write
(`/dev/kaguraemmc0 on / (hfs, local, journaled, noatime)`), `lo0` up, libignition attempting
preboot, `mount-phase-1` and `data-protection` boot tasks running, and `exec seputil` — with no
panic. `SWEET FB: AppleCLCD 1080x1920 ... presented as 1125x2436` confirms the built-in
IOMobileFramebuffer is presenting into the panel framebuffer.

| Area | State |
| --- | --- |
| Boot chain | Sony ABL → shim (`SWEETXNU` payload loader) → XNU at EL1, MMU off |
| Early console | ramoops at `0xa7fbe000` + live LCD glyphs; all kernel/daemon console bytes tee'd into pstore |
| XNU early boot | Full `bsd_init()` completes; global `ml_set_max_cpus(1)` avoids the single-CPU wait wedge |
| eMMC / storage | `kagura_emmc.c` (0085) reads GPT + CMD17/CMD24; `/dev/kaguraemmc0` is the root device |
| Root filesystem | HFSX image at p54 LBA 0, SWEETXNU payload at LBA 46,563,000; XNU remounts `/` RW |
| Userland | **`launchd` running boot tasks** (mount-phase-1, data-protection, seputil); next: backboardd/SpringBoard |
| Display | Shim MDP5 + live FB glyphs; `sweet_early_display_commit()` (CTL0_FLUSH/START) fires on every `AppleCLCD::present()` |

Boot args required: `rd=kaguraemmc0 emmcroot=userdata rootrw=1 emmcrw=1
unentitled_ios_sim_launch=1 cs_enforcement_disable=1 amfi_get_out_of_my_way=1` — the last three
are needed because the kernel is built `XNU_TARGET_OS_OSX` and otherwise kills iOS binaries with
`EXEC_EXIT_REASON_WRONG_PLATFORM` (namespace 9 code 0xe).

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
