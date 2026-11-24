# ios-sweet-kagura — Made by AI

**Apple's iOS 16 XNU on a Sony Xperia XZ F8331 (`kagura`, MSM8996).** Userland target:
iOS 16.7.16 (20H392). Experimental bring-up, not a usable phone OS: Apple's security model is
mostly disabled and the root image is written into `userdata`, erasing Android's data.

## Status (2026-11-24)

The shim boots XNU, the HFSX root mounts from eMMC, and iOS `launchd` runs its boot tasks
(`mount_hfs`/`mount` exit 0, `data-protection` boot task). `seputil` is spawned but is **slow,
not wedged**: it faults in the dyld shared cache (`esr 0x92000007`) while the CMD17 eMMC driver
delivers only ~1.8–3.6 MB/s, so the shim's 40 s hardware watchdog cuts it off before dyld finishes.

The attempted CMD18/CMD23 speed-up failed: **CMD23 breaks the HFS+ mount** (EIO), and CMD18
without CMD23 times out in the transfer loop (`KAGURA eMMC: CMD18 timeout s=23/32`). The working
configuration remains CMD17 (`emmcmulti=0`). Backboardd and SpringBoard have not been reached.

| Area | State |
| --- | --- |
| Boot chain | Sony ABL → shim (`SWEETXNU` payload loader) → XNU at EL1; **p30 must be the stock root-layout shim** |
| Console | ramoops at `0xa7fbe000` + live LCD glyphs, tee'd to pstore; on-screen log still double-renders |
| Early boot | `bsd_init()` completes; `ml_set_max_cpus(1)` avoids the single-CPU wait wedge |
| Storage | `kagura_emmc.c` (0085) reads GPT; CMD17/CMD24 work, CMD18/CMD23 do not |
| Root FS | HFSX at p54 LBA 0; payload at p54 LBA 46,563,000; `/` remounted RW |
| Userland | `launchd` boot tasks run; `seputil` slow at ~1.8–3.6 MB/s |
| Display | MDP5 + live FB glyphs, `AppleCLCD` presenting; native 1080×1920 deferred |

Boot args: `rd=kaguraemmc0 emmcroot=userdata rootrw=1 emmcrw=1
unentitled_ios_sim_launch=1 cs_enforcement_disable=1 amfi_get_out_of_my_way=1` (the last three
avoid `EXEC_EXIT_REASON_WRONG_PLATFORM` because the kernel is built `XNU_TARGET_OS_OSX`).

## Build and test

- Kernel: GitHub Actions `.github/workflows/xnu.yml`, macOS 13.5 / xnu-8796 / DEVELOPMENT /
  SWEET, patches in `xnu/patches` applied in order. Every run must pass the LSE check
  (`LSE_REAL_COUNT 0`); Kryo is ARMv8.0-A and faults on leftover ARMv8.1+ atomics.
- Shim: `shim/build_kagura.sh` + `shim/pack_kagura.py` (stock Sony 608-byte header, `ABS64: 0`).
  If rebuilding, **`CFLAGS` must include `-DPAYLOAD_LBA_OFFSET=46563000ULL`**, or the shim reads
  the HFSX root instead of the payload. `build_root_layout.sh` has a stale `46563648` value.
- `tools/mkadt.py` generates the ADT; `tools/mkpayload.py` resolves chained fixups.
- Payload is staged on `mmcblk0p54` with base64-safe `dd` (approval required); p30 is flashed
  only with approval and must be `xnu-flash-rootlayout-20261123.img` (MD5
  `024ddd143649be14ef57803e39839cda`). Rebuilt shims stop at ignition and are unusable.
- Read logs from TWRP `/sys/fs/pstore/console-ramoops` to a NEW filename; never overwrite.
  Rebuilt-shim traps and the bad p30 backup are documented in `status.md`.

## Related ports

- [joranv1/ios-sailfish](https://github.com/joranv1/ios-sailfish) — sibling fork reaching the
  lock screen with CoreAnimation/Wi-Fi/touch; provided IOKit-quiesce and `IOAcceleratorES`
  findings. Its storage is UFS, not portable to kagura's eMMC.
- [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz) — source of the IOKit findings.

## Known problems

- seputil throughput (CMD18 broken); live console double-render; `mkadt.py` still advertises
  the reference device's memory map; platform expert still matches `xiaomi,sweet`.
- Patch 0096 keeps identity pointers for framebuffer/ramoops; a proper mapped-alias fix is needed.

## Warnings

- Only `userdata` is written experimentally. Never touch bootloader, `TA`, `modemst1`/`modemst2`,
  `fsg`, `persist` or partition tables. No raw MDSS/MDP/DSI register reads (bus hang).
- `NOTES.md` describes the unrelated Redmi `sweet` port; its hardware facts do not apply here.

## Made by

- Claude (Free tier with MCP Connector)
- OpenCode (DeepSeek V4.1 Flash, MiMo-V2.6-Flash, Big Pickle, Hy4 Flash)
