# Kagura port — detailed status (2026-11-24, session 3t)

This file holds the long-form bring-up history and the exact device-state rules that used to
live in `README.md`. Keep `README.md` short; update this file and
`D:\xz\XPERIA-PORT-HANDOFF.md` when a milestone is reached.

## Current device state (last verified 2026-11-24)

- Phone in TWRP (`adb devices` → `recovery`).
- **p30 = genuine root-layout shim** `xnu-flash-rootlayout-20261123.img`, MD5
  `024ddd143649be14ef57803e39839cda`. Verify before interpreting any log:
  `adb shell dd if=/dev/block/mmcblk0p30 bs=512 count=33416 2>/dev/null | md5sum`.
- **p54 payload = 0136 + `emmccmd23=0` (CMD18 enabled)**, MD5 `0115fb3d44c1862772ba375027b8a927`,
  at LBA 46,563,000, 32,507 sectors. Log: `D:\xz\shimlogs\xnu-0136-nocmd23-rootlayout-20261124.txt`.
- p54 LBA 0 = HFSX root image; never overwrite. Only p54 and (with approval) p30 are written.
- Kernel CI artifact 45344986056, head `0e00a2d`; README head after rewrite to follow.

## DO / DON'T (hard-won)

1. **Only the stock root-layout shim reaches seputil.** Any rebuilt shim
   (`xnu-longwdt-0126-20261123.img`, `xnu-flash-wdt180-20261124.img`) stops at
   `ignite() returned 25` with no sampler, regardless of payload. Do not use rebuilt shims for
   seputil/watchdog experiments. These shims must be **flashed to p30** to boot, not just
   `fastboot boot`ed (user guidance).
2. `D:\xz\dump\p30-backup-20261124.img` is **NOT** a good-shim backup: its first 17,108,992
   bytes are MD5 `f4bf6d71870d05dea2ad6ae980261e73` (the longwdt image). Restoring it restored
   the wrong shim and invalidated two tests.
3. Any shim rebuild must compile with `-DPAYLOAD_LBA_OFFSET=46563000ULL`. The payload is at
   p54 LBA 46,563,000 = absolute LBA 48,930,488 (verified by reading the `SWEETXNU` magic at
   both addresses). Without the define the shim reads LBA 0 (HFSX root) and reports no payload.
   `build_root_layout.sh`'s `46563648` is stale/wrong. Require `ABS64: 0`.
4. **`ignite() returned 25` is normal** (the working sailfish port has it too). But if it is
   the last log line with no sampler, that means a wrong shim, not a seputil problem.
5. **CMD23 is unusable:** 0136 with CMD23 gives `hfs_mountfs: buf_meta_bread failed with 5`.
   Keep `emmccmd23=0`. Boot args `emmcmulti=0` / `emmccmd23=0` are the rollback gates and need
   no kernel rebuild.
6. `adb wait-for-device` hangs here — poll `adb devices`. `.sh` files must stay LF (CRLF breaks
   `set -euo pipefail`; run through `tr -d '\015'`). No raw MDSS/MDP/DSI reads. Pull every pstore
   to a NEW filename under `D:\xz\shimlogs\`.
7. Every `dd` to p54 and every `fastboot flash` to p30 needs its exact command printed and
   explicitly approved each time. `adb reboot` and `fastboot boot` (RAM only) are standing
   exceptions. `fastboot reboot` and `adb reboot bootloader` still need approval.

## Chronological findings

### 0104–0126 — early boot to userland
`stage > 63` logging bug fixed (0106); `ml_wait_max_cpus()` bypassed (0109/0121/0122);
`ml_set_max_cpus(1)` at bsd_init (0124); live FB glyphs + CTL0 flush/start (0110–0112); console
pstore tee (0122); `AppleCLCD::present()` frame commit (0125); EXEC/EXIT telemetry (0126).
Log `xnu-0126-fulltrace-20261123.txt` (19,070 B) reaches `exec seputil` with no seputil exit.

### 0127–0131 — disproved theories
The `IORegistry` `RLOCK` theory was wrong (all 18 before/after pairs complete); seputil never
asks for `/arm-io/sep`. No `AppleSEPManager` stub is published by sailfish or kagura, and
`ignite() returned 25` is normal. 0129/0130 showed seputil passes every marker inside
`process_signature()` and takes the `posix_spawn()` call site; an atomic pstore tee produced an
identical pattern, so it is a real control-flow difference, not a log race.

### 0135 — the diagnosis changed
Sampler (`xnu-0135-firstboot-20261124.txt`, 28,330 B): seputil is runnable, not blocked —
`SMP seputil t0 st 0x4 ... far 0x1c8e3e004 esr 0x92000007`, a translation fault in the shared
cache. eMMC read counters climb at ~7,300 sectors/2 s (~1.8–3.6 MB/s) via polled one-sector
CMD17, and no `waiting lba=` heartbeat fires. seputil is **slow**, and the 40 s hardware
watchdog cuts it off. Rootfs/shared-cache layout matches sailfish (`/System/Cryptexes/OS/...`).

### 0136 — CMD18/CMD23 result (2026-11-24)
CI 45344986056 GREEN, SHA256SUMS OK, `LSE_REAL_COUNT 0`; SDK strings present;
`emmcmulti` / `emmccmd23` boot-arg gates present.
- CMD23 enabled: `hfs_mountfs: buf_meta_bread failed with 5` (EIO) — root mount fails.
- `emmccmd23=0`: root mounts cleanly (`hfs: mounted iOSRoot on device b(1, 0)`), no CMD23 error.
- With the correct p30 shim, the decisive log `xnu-0136-nocmd23-rootlayout-20261124.txt`
  (16,425 B) ends with `KAGURA eMMC: CMD18 timeout s=23/32 lba=19123832 int=1` and never reaches
  `SWEET EXEC:` / `SWEET EXECP: P9`. Earlier "stall at launchd load" results with this payload
  used the wrong p30 and are invalid.
- CMD17 baseline on the same kernel and shim,
  `xnu-0136-nomulti-rootlayout-20261124.txt` (28,235 B), reaches `SWEET EXECP: P9`,
  `SWEET EXIT: mount_hfs`, `(data-protection) Doing boot task`, five sampler samples — same
  picture as 0135. **CMD18 is the regression; CMD17 works.**

### Shim/watchdog experiments (do not repeat)
`xnu-longwdt-0126-20261123.img` cancels the hardware watchdog in DSI bring-up and adds emmc/DSI
work before the loader. Flashing it (approved) and booting any 0135/0136 payload always stops at
ignition with no sampler. Built `xnu-flash-wdt180-20261124.img` (180/205 s, cancel call removed):
first build without `PAYLOAD_LBA_OFFSET` reported no payload (user saw it); rebuilt with
`46563000ULL` (MD5 `fabc27b87ef498e417a651f907789dfc`, `ABS64: 0`) found the payload but still
stopped at ignition. Conclusion: rebuilt shims change boot behavior; extending the watchdog
does not help. Only the genuine rootlayout shim is valid.

## Next concrete action
1. Decide: keep CMD17 (`emmcmulti=0`, working baseline) and attack seputil throughput another
   way, or fix CMD18 properly in a new CI patch — e.g. manual CMD12 after the block transfer
   instead of SDHCI auto-CMD12, plus logging of the DATA_END status that currently yields EIO.
   Keep `emmcmulti=0` / `emmccmd23=0` rollback gates.
2. Whatever is chosen, verify p30 MD5 first, write payload only with exact-command approval, and
   pull a NEW pstore log. Compare against `xnu-0136-nomulti-rootlayout-20261124.txt`.
3. Live on-screen console still shows ~5 stale lines (0134 double renderer); ask the user what
   they say, then suppress the old renderer. Native 1080×1920 stays deferred until
   seputil/backboardd progress.

## Rollbacks
- p54 CMD17 payload (0135 kernel): `xnu-payload-0135-20261123.pad512.bin`, MD5
  `bc155eeabeaefd459d3a17d9b98e2975`.
- p54 CMD17 payload (0136 kernel): `xnu-payload-0136-nomulti-20261124.bin`, MD5
  `e20f845a6fc36f2ba4e9ab2b0931ac87`.
- p30 good shim: `xnu-flash-rootlayout-20261123.img`, MD5 `024ddd143649be14ef57803e39839cda`.
