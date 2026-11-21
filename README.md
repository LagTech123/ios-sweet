# ios-sweet-kagura - Made by AI

**Apple's iOS 16 XNU on a Sony Xperia XZ F8331.**

This project ports Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) to the Sony
Xperia XZ F8331 (`kagura`, Qualcomm MSM8996 / Snapdragon 820). iOS 16.7.16 (20H392) is the
userland target.

> Experimental bring-up, not a usable phone OS. Many things are missing or stubbed, and Apple's
> security model is mostly switched off. The root image is written into the `userdata`
> partition, so **it erases Android's user data**.

## Status (2026-11-21)

The shim boots and hands off to XNU, which now runs through its early VM setup. Kernel consoles
are captured from the ramoops/pstore region, so every boot leaves a log even when the kernel dies
before the real console exists.

The current kernel stops between two of its own instrumentation markers inside `arm_vm_init()`.
That window is `sweet_early_map_final()` plus `arm_vm_physmap_init()`; the running audit points at
an unsigned-size underflow in the boot-data `arm_vm_page_granular_RWNX()` call (the virtual
address of `topOfKernelData` lands below `end_kern` on this SoC, so the length wraps and the
function spins). Patch `0090` instruments that window and guards the underflow.

| Area | State |
| --- | --- |
| Boot chain | Sony ABL → shim (`SWEETXNU` payload loader) → XNU at EL1, MMU off |
| Early console | ramoops console at `0xa7fbe000`, mirrored to pstore, readable from TWRP |
| XNU early boot | `arm_vm_init()` entry, memory-size checks, `early_random()` + DRBG, `arm_vm_prot_init()`, `arm_set_user_tbi()` all complete |
| Past `arm_vm_init()` | not yet reached |
| Display | implemented by the shim (MDP5 scanout works outside XNU) |
| eMMC / storage | **missing** — XNU has no eMMC/SDHCI driver for this SoC, so there is no root filesystem |
| Userland | not reached |

## Build and test

- The kernel is built by GitHub Actions (`.github/workflows/xnu.yml`, `blacktop/darwin-xnu-build`):
  macOS **13.5** (xnu-8796), `DEVELOPMENT`, `SWEET`, with `xnu/patches/*` applied in order.
  Every run must pass the LSE check — MSM8996/Kryo is ARMv8.0-A, so any leftover `ldadd`/`cas`
  class instruction or ARMv8.3 `rcpc` load is a hard fault on this device.
- The shim is built with `shim/build_kagura.sh` and packed with `shim/pack_kagura.py` (stock Sony
  608-byte header, `ABS64: 0`). `tools/mkadt.py` generates the device tree XNU receives.
- Bring-up uses a kernel payload staged in the `userdata` partition (`mmcblk0p54`); the shim finds
  it, copies the Mach-O segments and jumps.
- After a boot, read the log from TWRP as `/sys/fs/pstore/console-ramoops`

## Known problems

- `arm_vm_physmap_init()` underflow (above) — the current blocker.
- `mkadt.py` still advertises the memory map of the reference device; the payload is loaded into
  a region that is not reserved for it.
- The platform expert still matches the reference device string.
- No eMMC driver, so even a completed `arm_vm_init()` has nowhere to mount a root filesystem.

## Warnings

- Only the `userdata` partition is ever written during experiments. Never touch the bootloader,
  `TA`, `modemst1`/`modemst2`, `fsg`, `persist` or the partition tables.
- Do not read MDSS/MDP/DSI registers directly — it hangs the bus and forces a power cycle.

`NOTES.md` describes the unrelated Redmi `sweet` reference port; its hardware details do not
apply to `kagura`.

## Made by
- Claude (Free tier with MCP Connector)
- OpenCode (DeepSeek V4.1 Flash, MiMo-V2.6-Flash, Big Pickle)
