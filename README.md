# ios-sweet-kagura - Made by AI

**Apple's iOS 16 XNU on a Sony Xperia XZ F8331.**

This project ports Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) to the Sony
Xperia XZ F8331 (`kagura`, Qualcomm MSM8996 / Snapdragon 820). iOS 16.7.16 (20H392) is the
userland target.

> Experimental bring-up, not a usable phone OS. Many things are missing or stubbed, and Apple's
> security model is mostly switched off. The root image is written into the `userdata`
> partition, so **it erases Android's user data**.

## Status (2026-11-22)

The shim boots and hands off to XNU, which runs through its early VM setup. Kernel consoles are
captured from the ramoops/pstore region at `0xa7fbe000`, so every boot can leave a log even when
the real console does not exist yet. `arm_vm_init()` entry, memory-size checks, `early_random()` +
DRBG, `arm_vm_prot_init()` and `arm_set_user_tbi()` all complete.

**Important correction (2026-11-22):** logs that stopped after instrumentation marker 9 did *not*
prove a kernel hang. With `__ARM_16K_PG__` the L2 translation block covers **32 MB**, but the early
console aliases used physical bases `0x83400000` (framebuffer) and `0xa7f00000` (ramoops), neither
of which is 32 MB-aligned. The block descriptors therefore map `0x82000000` and `0xa6000000` —
ordinary RAM — so console writes after the VA switch never reached the real framebuffer or
pstore. Earlier theories that the kernel stopped at the TTBR0 switch, an ASID issue or a pstore
pointer bug were based on those missing log lines and are retracted; the kernel may have kept
running well past marker 9.

Patch `0096` (currently the head of `kagura`) keeps the early console on its identity VA=PA
pointers and defers the TTBR0 switch, so later boot stages should become observable. It has been
built in CI but has not yet been boot-tested on the device at the time of writing.

| Area | State |
| --- | --- |
| Boot chain | Sony ABL → shim (`SWEETXNU` payload loader) → XNU at EL1, MMU off |
| Early console | ramoops at `0xa7fbe000`, readable from TWRP; 0096 keeps it on identity VA=PA |
| XNU early boot | `arm_vm_init()` entry, memory checks, `early_random()` + DRBG, `arm_vm_prot_init()`, `arm_set_user_tbi()` complete |
| Past `arm_vm_init()` | not confirmed yet — logs after marker 9 were blind before 0096 |
| Display | MDP5 scanout works from the shim; the XNU early framebuffer alias is being corrected |
| eMMC / storage | driver from patch `0085` present (`bsd/dev/kagura_emmc.c`, `/kagura-emmc` ADT node, boot args `rd=kaguraemmc0 emmcroot=userdata`), compile-tested only, never runtime-tested |
| Userland | not reached |

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
- OpenCode (DeepSeek V4.1 Flash, MiMo-V2.6-Flash, Big Pickle)
