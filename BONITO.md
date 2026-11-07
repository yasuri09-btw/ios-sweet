# iOS on bonito (Pixel 3a XL, SDM670) - working notes

Port of ios-sweet to the Google Pixel 3a XL (`bonito`, SDC panel `samsung,sofef00-bonito`).
Hardware reference: postmarketOS / gitlab.com/sdm670-mainline/linux (branch `on-stable`).
Device dumps (live FDT, pmOS boot.img) live outside the repo: they contain IMEIs and serials.

## Hardware facts (verified on device unless marked)
- ABL `b4s4-0.4-8048689`, unlocked, slot a. `fastboot boot` works; ABL fastboot has no `fetch`.
- Boot image: header **v0**, gzip'd kernel with the DTB **appended** (Image.gz-dtb), like pmOS.
  The arm64 Image header needs **text_offset 0x80000**; 0 gives
  `Error verifying the received boot.img: Invalid Parameter`.
- DTB: pmOS's bonito-sdc DTB (no qcom,msm-id/board-id; ABL accepts it).
- Framebuffer 0x9c000000, 1080x2160 a8r8g8b8 (cont_splash_region size 0x2400000). ABL sets the
  panel up in **command mode** (`dsi_sofef00_sdc_1080p_cmd_display`) but CPU writes still show up.
- RAM: `<0x80000000 2G>, <0x1_00000000 0x7e2c0000>`; XNU gets the upper bank (`--highmem`).
- GICD 0x17a00000, GICR 0x17a60000 (same as sweet). Timer 19.2 MHz, vtimer INTID 19.
- CPUs: MPIDR 0x000..0x700 (6x A55 + 2x A75), ID_AA64MMFR0 PARange=2, **TGran16 = 1** (16K OK).
- PSCI 1.1. PSCI SYSTEM_RESET works but ignores the IMEM restart_reason (0x146bf65c): it boots
  the normal slot, not fastboot. PSHOLD 0x0c264000 (downstream DT).
- Storage: **eMMC** (sdhc_1 @ 0x7c4000, Micron 64 GB), not UFS. Touch: Synaptics RMI4, I2C9 @0x20.

## Milestones
1. 2026-11-07 shim boots: red screen + diagnostics. MMU ok, VTIMER irqs=0x1E, SGI1 irqs=0x0A.
2. 2026-11-07 **XNU reaches root mount.** CI run 42395123187 (macos-15, Xcode 16.0, sweet patches
   0001-0081 unchanged), ADT `mkadt.py --highmem --no-ufs`, cmdline
   `debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress wdt=-1 sweetkeys=0 sweettouch=0 sweetusb=0 rd=ufsroot0`
   -> panic `IOFindBSDRoot: SWEET UFS root ufsroot0 unavailable` (expected: no UFS node).
   Prints `SWEET CPU: 1/1 cpus initialized, SGI1 verified`. Without `rd=` it waits silently
   (>4 min seen) after `Security policy loaded: SWEET built-in Mobile File Integrity`.
   `-enable_kprintf_spam` makes the fb console very slow.

3. 2026-11-07 **eMMC read from our own code** (shim probe, commit 2c40dd8 + this note).
   ABL hands sdhc_1 over at SDHCI power-on defaults (hostctl 0, ctl2 0, clk 0x3 = card clock off,
   bus power off); the card itself stays powered. Working sequence (mirrors mainline sdhci-msm):
   VENDOR_SPEC (+0x20c) := 0xa9c, MCLK_SEL default, HC_SELECT_IN off, VENDOR_SPEC3 PWRSAVE_DLL off;
   RESET_ALL; bus power 3.0 V + 1.8 V signalling, each acked via PWRCTL (+0x240 status/+0x248
   clear/+0x24c ctl: BUS_ON->BUS_SUCCESS, IO_LOW->IO_SUCCESS); IO_PAD_PWR_SWITCH; clock
   div 0x3ff; CMD0, CMD1 (ocr 0xc0ff8080), CMD2, CMD3 rca 1, CMD7; clock div 8 (<=24 MHz), 1-bit.
   **PIO does not work on this controller**: CMD8 is accepted (R1 0x900), the card returns to
   tran, but Buffer Read Ready never latches, the data port reads 0 and no data timeout fires.
   **ADMA2 (32-bit) works** with the SMMU (stream 0x140) as ABL left it. pmOS/mainline also uses
   ADMA 64-bit (dmesg: "using ADMA 64-bit", CQE, HS400, card RCA 0001, 58.2 GiB user area).
   GCC clocks apps/ahb/ice/bus all on at handoff; ICE v3.1 (0x0301004b) needs no setup.

4. 2026-11-07 **XNU roots on the eMMC.** Patch 0082 (`bsd/dev/sweet_emmc.c`), CI run 42399429115,
   ADT `mkadt.py --highmem --no-ufs --emmc`, cmdline as milestone 2 but `rd=emmcroot0`.
   On screen: `SWEET EMMC: card ready`, `SWEET EMMC: "userdata" ...`, `BSD root: emmcroot0`,
   then `Could not mount root, errno = 19` (ENODEV: userdata still holds pmOS's ext4). Read-only.

5. 2026-11-07 ~13:15 CST **APPLE LOGO on bonito.** iOS 16.7.16 (20H392, iPhone X userland) root
   flashed to userdata (`tools/img2simg.py` sparse image, ~6 min), kernel 0082, ADT
   `--highmem --no-ufs --emmc` with D22AP /product, cmdline `debug=0xc serial=3 cpus=6 keepsyms=1
   -noprogress rd=emmcroot0 emmcrw=1 unentitled_ios_sim_launch=1 ignition_level=1
   wdt_test=no_user_checkin rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_hz=30 sweetkeys=0
   sweettouch=0 sweetusb=0` (image `shim/out/bonito-ios1.img`). 6 A55 cores (SMP works).
   First boot hung after `exec auearlyboot` (MobileAccessoryUpdater FUD early boot waits on a
   semaphore for AppleFirmwareUpdate callbacks that never come); it is an optional boot task
   (no RequireSuccess/RequireRun in launchd's embedded Boot list), so `tools/bonito-root-tweaks.py`
   renames it. Then: MSUEarlyBootTask, FIPSPOST_USER (4 passed, 8 ms), daemon wave,
   backboardd, Apple logo. Clock starts at 1970 (no RTC driver).
   Gentoo's fsck.hfsplus (diskdev_cmds 332.14) segfaults even on a fresh volume: image checked by
   read-only mount instead. Flashing a raw image makes ABL write FILL chunks: use the .simg.

## Decisions
- 2026-11-07: the user is fine with losing postmarketOS: the iOS root goes into `userdata`
  (no GPT changes). Before the first write: back up persist, modemst1/2, fsg, fsc, devinfo,
  boot/dtbo/vbmeta (both slots) from pmOS (ABL fastboot has no `fetch`), and confirm the
  write with the user.

## Next
- eMMC: done read-only (0082). Still to verify: 8-bit HS switch result, writes (emmcrw=1), speed.
  Later: HS200/HS400 + 64-bit ADMA from xnu RAM instead of the bounce buffer.
- Persistent log: sweet's 0047 writes to the end of UFS userdata; needs the eMMC driver.
- Touch: RMI4 over GENI I2C9 (sweet: Goodix on its own SE; 0060/0062/0063/0071).
- Then the iOS root image (mkiosroot.sh) into userdata, D22AP /product (d22ap.lzfse needs lzfse).

## Credits
- **Nura (formerly postmarketOS)**: the running install that gave us bonito's live device tree,
  memory map, boot image format and the eMMC driver's real-world configuration.
- **The sdm670-mainline team** (gitlab.com/sdm670-mainline), including Richard Acayan's bonito
  device trees: the hardware map for this phone (addresses, regulators, panel, touch, eMMC).
- **Mainline Linux** (`sdhci-msm`, `sdhci`, `gcc-sdm845`): the hardware reference for the eMMC
  bring-up; the driver here is written from the SDHCI/JEDEC specs with it as the guide.
- **ios-sweet** (dipa-studio) and the hackiosx.dev ports: everything XNU, which this port builds on.
- **blacktop/darwin-xnu-build** and Apple's open-source XNU releases.

## Handoff (2026-11-07 ~13:30 CST, session paused for usage limit)
- Phone: booted iOS once with logo; console then flooded by bluetoothd + fairplayd.H2 relaunch
  loops; `spindump` appeared (something hung; unknown what). userdata holds the image WITH
  auearlyboot disabled but still WITH bluetoothd/fairplayd jobs.
- Ready, NOT flashed: `research/ipsw-x/ios16-root.simg` = image + bonito-root-tweaks (auearlyboot
  renamed, bluetoothd + fairplayd.H2 jobs removed). Flash: `doas fastboot flash userdata ...simg`
  (~6 min; trust only the unbuffered log, never kill it early).
- Building: kernel 0083 (USB live log, SDM670 USB clocks), CI run 42406204342 (commit e7d98b5).
  When done: download to xnu/out-0083, mkpayload with the milestone-5 cmdline but sweetusb=1
  (or omit), boot it, run `tools/sweet-usbnet.sh logs/boot.txt` on the PC (needs sudo->doas)
  to receive the console on udp/5140.
- Next after that: read the spindump cause from the live log; then sweetfb_noconsole=1;
  RTC (clock starts 1970); RMI4 touch driver.
- UPDATE 13:55: the cleaned image (bluetoothd + fairplayd.H2 jobs removed) is ON userdata now.
  Boots with it stop at the `xpcroleaccountd` boot task (last line `<Notice>: launchd`, cut off),
  with BOTH kernel 0083 (sweetusb=2) and 0082 -> not the USB code. The earlier image (jobs kept)
  went past it to the Apple logo. Next: boot once more (first-boot state may differ); if it stops
  again, revert the job removal in tools/bonito-root-tweaks.py, rebuild the .simg, reflash.
  USB on 0083: no USB device enumerated on the PC; the `SWEET USB:` lines (just after
  `SWEET EMMC: "userdata"`) are still unread. Suspect ABL powers USB down at exit (GDSC/clocks).
- UPDATE 15:35: boot 15:31:20 (0082, `shim/out/bonito-ios1.img`) on the cleaned image stopped
  with last line `Adding 99 entry to persona Table for user: FFFFFFFF-FFFF-...` (usermanagerd
  UMD_INIT, persona 99 System). So two boots of the cleaned image stopped early at different
  places. Job removal is now opt-in (`--remove-jobs`). `--restore-jobs ORIG_PLIST` puts the two
  jobs back from the IPSW's original launchd.plist (apfs-fuse OS.dmg, root/System/Library/xpc/).
  Done locally: `research/ipsw-x/ios16-root.img` now has both jobs again (530 LaunchDaemons),
  `.simg` rebuilt 15:34. NOT flashed yet (waiting for user OK). After flashing, boot
  `shim/out/bonito-ios1.img`; expect the Apple logo again (earlier result with the jobs kept).
- UPDATE 15:35: NOT a hang. The 15:31:20 boot resumed by itself after the persona-99 line
  (boot started 15:31:20; resumed before 15:35:30 per the user, so a stall of ~2-3 min) and is now exec'ing daemons. So the cleaned
  image is probably fine and the earlier xpcroleaccountd "stop" was likely the same stall,
  judged too early. Restored-jobs .simg was NOT flashed (userdata still = cleaned image). Open:
  what waits there (a timeout of a few minutes; time it, find it from the live log).
- UPDATE 15:40: same boot reached backboardd (its usual log spam on the fb console), then the
  APPLE LOGO (second time on bonito), i.e. as far
  as the first logo boot. Cleaned image (no bluetoothd/fairplayd.H2) is good; no reflash needed.
  Next: USB live log (read the `SWEET USB:` lines: photo of the early console on a 0083 boot),
  then sweetfb_noconsole=1, RTC, RMI4 touch.
- UPDATE 15:50: after the logo the 15:31 boot went to spindump; NO SpringBoard (the console is
  drawn over the GUI, so SpringBoard would be visible; noconsole is not the issue). Last lines
  seen: AKS calls from siriinferenced, fileproviderd, searchd, aggregated, coreidvd; last exec
  `xpcproxy` -> `nesessionmanager`; the user flags `symptomsd-diag` (after suggestd, assetsd) as
  the one that looks wrong (exact message unread). Next: 0083 boot with sweetusb=2 +
  `tools/bonito-usblog.py logs/boot-0083-usb.txt` to get the log off the phone.
- 15:45: TWRP for log reads: `recovery/twrp/twrp-3.7.1_12-1-bonito.img` (official dl.twrp.me,
  SHA256 e0781b59...78d0 matches the published .sha256; not in git). Use only via temporary
  `doas fastboot boot` (never flash). Plan: move the 0047 console ring writer to the eMMC
  (last 1025 blocks of userdata), pull it from TWRP with adb (tools/sweet-log-pull.sh).
  Untested: whether TWRP's adb comes up and sees /dev/block/by-name/userdata on this ABL.
- 15:55: patch **0084** (`bsd/dev/sweet_emmc.c`): eMMC writer for the 0047 console ring, same
  layout (last 1025 4-KiB blocks of userdata, header + ring), every ufslogsecs s (default 10),
  then EXT_CSD FLUSH_CACHE (stops trying if refused). Needs emmcrw=1. CI run 42417348540
  (commit 3685435). Not compiled locally (no Apple clang here). Read: TWRP via
  `doas fastboot boot recovery/twrp/...img`, then `tools/sweet-log-pull.sh logs/NAME.txt`.
  Open puzzle: on 0083 (sweetusb=2) the user sees NO `SWEET USB:` lines at all, although the
  probe prints GDSC unconditionally right after `SWEET EMMC: "userdata"`; the log will show.
  Also seen on the 15:40 boot: a `driverkitd` CRASH line (text unread; user's phone camera dead).
- 15:50: **TWRP works.** 3.7.1_12-1 first crashed (Google logo, black, reboot to fastboot):
  dtbo_a/dtbo_b were erased (all zeros in bonito-backup/parts) for pmOS. With the user's OK,
  `fastboot flash dtbo_a` with dtbo.img from the factory image bonito-sp2a.220505.008
  (zip SHA256 db19d2aa...0002 = Google's; dtbo.img SHA256 ba8b2b6e...a80a; in
  recovery/factory/, not in git), 15:49:44. TWRP then booted: adb root, userdata =
  /dev/block/mmcblk0p72, 53648801280 bytes. dtbo_b still zeros. XNU's device tree is
  our ADT in the payload, so overlays cannot reach iOS; only open point: does ABL still accept the
  shim's appended DTB with dtbo_a present (next boot shows). Undo = `fastboot erase dtbo_a`.
- 15:54: 0084 CI success (`xnu/out-0084`, SHA256SUMS OK). Image `shim/out/bonito-ios3-emmclog.img` =
  0084 + `shim/out/bonito-ios.adt` + milestone-5 cmdline with `sweetusb=2 ufslogsecs=2`
  (build: `MKBOOTIMG="python3 /home/ada/codestuff/tools/mkbootimg/mkbootimg.py" shim/build.sh ABS_PAYLOAD`).
  Booted 15:54:27 with dtbo_a = factory overlays. Then: TWRP + tools/sweet-log-pull.sh.
