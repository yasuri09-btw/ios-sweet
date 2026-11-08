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

6. 2026-11-07 ~18:22 CST **iOS LOCK SCREEN ON THE PIXEL 3A XL.** SpringBoard + PosterBoard
   presented the iOS 16 lock screen. Boot `shim/out/bonito-ios9-kstack.img` (kernel 0086, ADT
   `shim/out/bonito-ios2.adt` with /chosen MAC addresses, cpus=1, ufslogsecs=2, sweetsample=*
   sweetsamplesecs=60) booted 18:09:43 on the root image with the ByHost migration pref (flashed
   17:09). What it took after the logo: ByHost `com.apple.migration` LastSystemVersion (no data
   migration), /chosen mac-address-wifi0/bluetooth0/ethernet0 (MobileGestalt's 5 s wait per app
   in lsd, found by ios-flame), cpus=1 (fewer/shorter boot-task stalls), and one long first boot
   so containermanagerd/lsd could create the app containers and the LaunchServices DB (they
   persist on the rw root).

7. 2026-11-07 ~19:20 CST **TOUCH UNLOCK ON THE PIXEL 3A XL.** Swipe-up on the iOS 16 lock screen
   works (user: laggy, software rendering, but it works). Kernel 0089 (Synaptics RMI4 over GENI
   I2C, ts_1p8 via PM660 GPIO12, bonito buttons), image `shim/out/bonito-ios13-touch.img`
   (cpus=1, sweetfb_noconsole=1, SpringBoard/backboardd sampler), booted 19:15:19 (third try;
   the first two hit the random early boot-task stall).

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
- **ios-flame** (NWylynko, Pixel 4) and **ios-sailfish** (joranv1, Pixel 1): reference notes, e.g.
  the SMMU setup for USB DMA after ABL.

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
  Booted 15:54:27 with dtbo_a = factory overlays: ABL boots the shim fine (usual kernel text), so
  dtbo_a stays factory (TWRP + iOS both boot). Then: TWRP + tools/sweet-log-pull.sh.
- 16:05: **first eMMC log pulled** (`logs/boot-0084.txt`, 29865 bytes, flush #5; TWRP + doas adb,
  userdata = mmcblk0p72; .bin kept out of git). Findings:
  - Boot stops after `exec init_featureflags` (boot tasks: mount-phase-1/2, data-protection,
    MSUEarlyBootTask, fips, keybag, usermanagerd, dirs_cleaner, xpcroleaccountd, init_featureflags);
    nothing printed afterwards (writer flushes only on new text). No eMMC errors (every eMMC
    command times out <= 6 s and prints).
  - dirs_cleaner `dc_reset: /tmp|/var/run: Invalid argument` = its renamex_np(RENAME_SWAP)
    (HFS has no swap; same as usermanagerd's .kb saves). Non-fatal; the next task runs.
  - Timestamps: launchd at ~205 s because the arch counter includes ABL fastboot time.
  - USB (lines were in the log, never seen on screen): GDSC + all 7 clocks ON, DWC3 GSNPSID
    0x5533300a, soft reset ok, `ep0 cmd 0x1 timeout` -> `endpoint 0 setup failed`; QUSB2 reads
    0x64646464/0x23232323/0x67676767/0x90909090 (wrong address for SDM670, or PHY off).
- Patch **0085**: thread sampler (0054) started from the eMMC init too; `sweetsample=*` = all
  processes; `sweetsamplesecs=N` (5-600) first delay and interval.
- 16:09: 0085 CI 42418579986 success -> `xnu/out-0085`; image `shim/out/bonito-ios4-sample.img`
  (0084 cmdline + `sweetsample=* sweetsamplesecs=45`), booted 16:08:58 (TWRP -> adb reboot
  bootloader -> fastboot boot, no buttons). Pull after a few minutes stuck.
- 16:12: **"Nothing boots" = slot a unbootable.** After the 0085 attempt, TWRP and the shim both
  bounced to the bootloader. Battery fine (`getvar battery-voltage` 4320, soc-ok yes). Slot a:
  `slot-unbootable:a yes, retry-count 0, successful no` (this morning: successful yes, retry 2).
  Fixed with `doas fastboot set_active a` (user OK) -> unbootable no, retry 3. TWRP boots again.
  The eMMC log was still 0084's (total 29865, flush #5): **0085 never ran**; retry it.
  Watch the retry count: nothing marks the slot successful now, so it may run out again.
- 16:20: **0085 sampler result** (`logs/boot-0085.txt`, 3 samples, slide 0x14554000): stall at
  MSUEarlyBootTask[8]; its only thread sits at the same pc in all samples, inside **dyld**
  (unslid pc 0x1a4643e0c = `str xzr, [x23, #8]!`, a plain store near a function start) -> blocked
  in a kernel page fault, before main(). launchd idle (libsystem_kernel/libdispatch waits).
  Not memory (126258 free 16K pages at boot) and not eMMC (the log writer kept flushing the
  samples during the stall). Open: what the fault waits on (page-in of dyld/cache __DATA?).
  `tools/dsc-a2s.py` resolves cache addresses to images (symbol lookup still prints `?`).
  Slot a retry count: 1 left after this TWRP boot. User: no more kernel patches for now.
- 16:24: cpus=1 test (`shim/out/bonito-ios5-cpu1.img`, 0085 + cpus=1, booted 16:21:15) abandoned
  before a result. User rolled back to the logo kernel: **0082 `shim/out/bonito-ios1.img`**
  booted 16:23:54 (slot a retry 2). Kernels 0084/0085 stay in the repo; the user does not want
  more kernel patches for now.
- 16:33: **userdata reflashed** with a fresh image (user OK): `research/ipsw-x/ios16-root.simg`
  rebuilt 16:26 = mkiosroot + sweet tweaks + bonito-root-tweaks `--remove-jobs` (same content as
  the 15:31 logo image). `fastboot flash userdata` 16:26:31-16:32:31, 47/47 OKAY, 360 s
  (`research/ipsw-x/flash-userdata5.log`). Then 0082 `shim/out/bonito-ios1.img` booted 16:32:42
  (slot a retry 1 before the boot). Note: the eMMC log area survives (DONT_CARE chunks).
- References (user-supplied, read 16:30): github.com/NWylynko/ios-flame (Pixel 4, SM8150) and
  github.com/joranv1/ios-sailfish (Pixel 1, MSM8996), both ios-sweet descendants on UFS.
  Flame's USB fix: the apps SMMU blocks DWC3 DMA after ABL; bypass refused by the hypervisor;
  what works = a free SMR for the USB stream -> own context bank with CBAR 0x1f000, CBA2R 1,
  SCTLR 0xe0 (translation off) — matches our `ep0 cmd 0x1 timeout` (needs a kernel patch; on
  hold). Flame also reads XBL/ABL reset forensics from the `klog` partition (TWRP, `strings`).
  Flame avoids `fastboot flash userdata` (Pixel 4 ABL hung on long zero FILL chunks); our
  img2simg uses DONT_CARE, five bonito flashes finished fine.
- 16:36: the 16:32:42 boot (0082 on the fresh image) stalled at MSUEarlyBootTask, then RESUMED
  by itself (user: now at finish-demo-restore). Same kernel + same image content as the 15:31
  logo boot. => The stall is intermittent (minutes, sometimes permanent?) and was present
  before 0083-0085; shim passes nothing from ABL's FDT except the initrd range, so dtbo_a is
  not involved. 0085's sample: the stalled thread sits in a kernel page fault (dyld store).
- **Boot time rule (user, 16:37):** sweet's port also needs ~4 min to boot; the multi-minute
  pauses in the boot tasks are known on sweet too. **Wait >= 10 min before calling a boot
  stuck.** The 13:55 (xpcroleaccountd), 15:54 (init_featureflags) and 16:13 (MSUEarlyBootTask)
  "hangs" were most likely cut off too early. (0085 sample: the pause is a kernel page fault in
  a dyld store; worth a look once kernel work resumes, but not a blocker.)
- 16:40: the 16:32:42 boot (0082, fresh image) passed finish-demo-restore after a ~3 min pause
  and reached **backboardd** (~8 min after fastboot boot), then the **APPLE LOGO** (~16:42,
  third time on bonito). Waiting works.
- 16:55: the 16:32 boot (0082) reached logo + spindump + daemon wave (triald, mmaintenanced,
  medialibraryd, mount_hfs, lots of SWEET AKS lines) but NO `exec SpringBoard` after 22 min.
  Next run: `shim/out/bonito-ios6-log.img` = 0084 + exactly the 0082 cmdline (sweetusb=0) +
  ufslogsecs=2, booted 16:55:21 (set_active a first: retry was 0). Plan: run ~25 min, pull the
  log via TWRP, look for SpringBoard/launchd job lines.
- 17:08: **0084 log of the 16:55 run** (`logs/boot-0084b.txt`, 129760 bytes, flush #38): SpringBoard
  WAS exec'd early (line 472) and is alive (AKS calls at the end); backboardd presented only 4
  frames (logo). `com.apple.datamigrator` exec'd 3 times -> **data migration**, which SpringBoard
  waits on. Cause (sweet NOTES 2026-10-17): datamigrator reads LastSystemVersion as a
  CurrentHost pref = `Preferences/ByHost/com.apple.migration.<IOPlatformUUID>.plist`; sweet's v11
  added it by hand, it never reached the tools, bonito's image had no ByHost dir.
  Fix: `tools/sweet-root-tweaks.py` byhost_pref() writes it (UUID A9683613-3704-53F2-95D0-98A8FB574E78,
  computed like IOPlatformExpertDevice::generatePlatformUUID from the ADT's synthetic
  unique-chip-id/chip-id; upper + lower case file names, 501:501 0600, dir 0755).
  Also seen: driverkitd CRASH sig 9 (line 606); securityd AKS sel 10 (wrap_key) -> unsupported x44.
  Image rebuilt + reflash (user: "do it for me") 17:09, then boot `shim/out/bonito-ios6-log.img`.
- 17:15: reflash with the ByHost fix done (`research/ipsw-x/flash-userdata6.log`, 47/47), slot a
  reset (retry was 0), `shim/out/bonito-ios6-log.img` booted 17:14:52. Expect: no datamigrator,
  SpringBoard lock screen. Next after that: touch (Synaptics RMI4, GENI I2C9 @0x20) = kernel work.
- 17:22: log of the 17:14 run (`logs/boot-0084c.txt`, ~6 min only): SpringBoard exec'd (line 691),
  datamigrator still exec'd twice (may only be SpringBoard's "migration needed?" query); no
  `migrationpluginwrapper` in this or the previous log, so "migration blocks SpringBoard" is
  NOT proven yet. Run ended in the daemon wave (pulled too early).
- 17:23: `shim/out/bonito-ios7-sbsample.img` = 0085 + 0082 cmdline + `ufslogsecs=2
  sweetsamplesecs=120` (sampler default names SpringBoard,backboardd), booted 17:23:05.
  Plan: run ~25 min untouched, then pull -> SMP samples show what SpringBoard waits on.
- 17:29: log of the 17:23 sampler run (`logs/boot-0085b.txt`): real stop after `exec tzinit` at
  ~55 s (screen's "fips" = launchd's [System Event] replay of the boot-task list, misleading).
  tzinit passed instantly on all earlier boots -> same random freeze. Next: `cpus=1` run
  (`shim/out/bonito-ios5-cpu1.img`, 0085, sampler on all processes every 45 s), no new patch.
- 17:34: **cpus=1 run is much faster**: booted 17:29:12, short wait at `exec mount`, `exec
  SpringBoard` ~17:33, **Apple logo ~17:34** (6-core runs: 5-10+ min with multi-minute freezes).
  Supports: the freezes are an SMP race in our kernel (page-fault path, 0085 sample).
- 17:38: **cpus=1 log** (`logs/boot-cpu1.txt`, 411797 bytes, 8 samples): SpringBoard main thread
  blocked in a synchronous NSXPC call from CoreServices (LaunchServices) to **lsd** (samples 5-7,
  same frames). lsd's busy thread: InstalledContentLibrary -> **libMobileGestalt**, waiting =
  ios-flame's finding: MobileGestalt WifiAddress reads /chosen mac-address-wifi0, else waits 5 s
  for an IONetworkController, per app bundle (~190 apps ~= 16 min). backboardd idle in runloops.
  Fix (ADT only): `tools/mkadt.py` adds /chosen mac-address-wifi0/bluetooth0/ethernet0
  (02:53:57:45:00:01..03). `shim/out/bonito-ios2.adt` (= `--highmem --no-ufs --emmc`, regenerated
  ADT matches the old one except random-seed). Image `shim/out/bonito-ios8-mac.img` = 0085 +
  bonito-ios2.adt + cpus=1 + ufslogsecs=2 + sweetsamplesecs=120.
  Also from flame, not done yet: lsd may wait for "seeding" forever if the LaunchServices
  migrator never ran (flame ran 00LaunchServicesMigrator by hand); IORTC removed a 30 s wait.
- 17:53: log of the MAC run (`logs/boot-mac.txt`, cpus=1): boot-task durations from launchd's
  timestamps: all < 1 s except xpcroleaccountd 8 s, **tzinit 228 s**, **launchd_cache_loader > 6 min**
  (still running at the pull). Boot tasks are serial, so one stalled task stalls everything; the
  stall moves between tasks per boot and also happens on one core.
- Patch **0086** (user OK for kernel work: "best kernel optimizations" -> first find the stall):
  sampler prints per thread `SMP name tN K state hint ev klr kbt ...` (scheduler state, block
  hint, wait event, saved kernel lr + fp backtrace inside its kernel stack, unslid) or `cont`;
  `sweetsample=*` skips kernel_task (name it explicitly to get it).
- 18:02: 0086 CI 42427313004 success -> `xnu/out-0086`. Image `shim/out/bonito-ios9-kstack.img` =
  0086 + bonito-ios2.adt (MACs) + cpus=1 + ufslogsecs=2 + `sweetsample=* sweetsamplesecs=60`,
  booted 18:02:24. Pull once a boot task visibly stalls for ~2 min.
- 18:09: **0086 log** (`logs/boot-0086.txt`, 4 samples, cpus=1): fastest boot yet (SpringBoard
  exec ~2 min, logo ~3 min). SpringBoard t0 still in the sync LaunchServices call to lsd (kernel:
  hint 2 = port receive). lsd no longer in MobileGestalt (MAC fix works); lsd t2 waits on a sync
  XPC from InstalledContentLibrary -> libsystem_containermanager -> **containermanagerd**, t1/t4
  wait on a user lock (hint 10). containermanagerd t3/t4 are RUNNING (state 0x4, changing
  backtraces): it is busy creating app containers (first boot work; persists on the rw root).
  Next: one long run (30+ min, same image) so lsd/containermanagerd finish.
- 18:30: Patch **0087** (touch + buttons, written from the live DT + mainline Linux sources):
  - Touch: Synaptics RMI4 @0x20 on GENI I2C 0xa84000 (same SE address as sweet's touch;
    GCC RCG 0x18148 / CBCR 0x18144 / vote bit 23 identical to gcc-sdm845.c). Pins: I2C GPIO6/7
    mux 1 (qup9), reset GPIO99, attention GPIO125 (TLMM NORTH 0x3900000, as sweet), ts-switch
    GPIO135 low (WEST 0x3500000). Rail ts_1p8 = PM660 (SID 0) GPIO13, PPID 0x0cc: SPMI v5
    EXT_WRITEL on the apps-owned channel (chnls 0xc600000 + 0x10000*apid), only after
    type 0x10 check; LV/MV vs old GPIO register layout per pinctrl-spmi-gpio.c.
    RMI4 ported from drivers/input/rmi4: PDT scan pages 0-3, F01 ctrl0 nosleep+configured,
    F12 register descriptors (query+1/+4/+7), control8 sensor max, data1 objects polled 10 ms.
  - SweetHIDTouch maps by the real panel size (PE_current_console) instead of 1080x2400.
  - Keys: bonito volume-up = PM660L (SID 2) GPIO8, PPID 0x2c7, RT 0xc710 (PON 0x0810 unchanged).
  - sweet_emmc_init starts keys + touch (as sweet_ufs_init). Bonito detection: ADT /sweet-emmc.
- 18:39: 0087 CI 42429984416 success -> `xnu/out-0087`. Image `shim/out/bonito-ios11-touch.img` =
  0087 + bonito-ios2.adt + cpus=1, sweetfb_noconsole=1, keys+touch on (defaults; touch starts
  45 s after eMMC init), ufslogsecs=2, sampler off. Booted 18:38:59.
- 18:45: **0087 result** (`logs/boot-0087.txt`): keys work at kernel level (power -> HID 0x30
  down/up with 1 client, vol-down 0xea; iOS answered with `SWEET FB: display off/on`); screen
  stayed black only because PosterBoard had just started (17 frames). Volume-up read as held
  (key=1 down=1, gpio=0) and the first RMI4 I2C write timed out (err 60, only TX watermark,
  no NACK). Cause of both: **DT PMIC GPIO numbers are 1-based** (pinctrl-spmi-gpio.c
  `args[0] - PMIC_GPIO_PHYSICAL_OFFSET`). ts_1p8 is PM660 GPIO12 (PPID 0x0cb), vol-up PM660L
  GPIO7 (PPID 0x2c6, RT 0xc610). 0087 wrongly set **PM660 GPIO13** (0xcc) to output-high
  (was mode 0x00 out 0x00 en 0x80).
- Patch **0088**: correct PPIDs; restores GPIO13 to mode 0x00/out 0x00/en 0x80 if it still
  shows 0087's values; logs SDA/SCL/IRQ levels before and after the RMI probe.

## Handoff (2026-11-07 18:52 CST, usage limit near)
- **State:** iOS 16.7 lock screen reached on bonito (milestone 6, 18:22). Phone is in **TWRP**
  (adb root), plugged in. dtbo_a = factory overlays (keep). userdata = image flashed 17:09
  (ByHost migration pref) + rw boots; app containers + LaunchServices DB are built (persist).
- **Kernel 0088** (PMIC GPIO numbering fix, GPIO13 restore, I2C level logging) CI run
  42431176953: in_progress . When done: `gh run download 42431176953 -R yasuri09-btw/ios-sweet -n xnu-kernel -D xnu/out-0088`,
  then image = 0088 + `shim/out/bonito-ios2.adt` + cmdline of `shim/out/bonito-ios11-touch.img`
  (`debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress rd=emmcroot0 emmcrw=1 unentitled_ios_sim_launch=1
  ignition_level=1 wdt_test=no_user_checkin rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_hz=30
  sweetfb_noconsole=1 sweetusb=0 ufslogsecs=2 sweetsample=-`), build with
  `python3 -I tools/mkpayload.py K ADT OUT --cmdline C` and
  `MKBOOTIMG="python3 /home/ada/codestuff/tools/mkbootimg/mkbootimg.py" bash shim/build.sh ABS_PAYLOAD`.
- **Boot loop recipe:** TWRP -> `doas .../adb reboot bootloader` -> check `getvar slot-retry-count:a`,
  if 0 run `doas fastboot set_active a` (standing OK) -> `doas fastboot boot IMG`. Pull log: boot
  `recovery/twrp/twrp-3.7.1_12-1-bonito.img`, then dd the last 1025 4K blocks of /dev/block/mmcblk0p72
  via `doas adb exec-out` and `tools/sweet-log.py` (sweet-log-pull.sh needs plain adb; use doas adb).
- **Test for 0088:** wait for the lock screen to be fully drawn (minutes after the logo), then
  power / volume / swipe up from the bottom edge. Check `SWEET TOUCH` lines: ts_1p8 GPIO12 on,
  "before probe sda6/scl7" (1/1 = idle bus), RMI F01/F12, "Synaptics RMI4 running". If the bus
  is stuck low: look at ts-switch GPIO135 polarity and touch power-up order.
- **Open issues:** multi-core boot-task stalls (cpus=1 workaround; 0085/0086 samplers show a
  thread stuck in a kernel page fault); USB (SMMU, see ios-flame's fix); driverkitd CRASH sig 9
  every boot; securityd AKS sel 10 unsupported; no RTC (1970). Rules: wait >= 10 min before
  calling a boot stuck; verify DT conventions in the driver before hardware writes.
- 18:54: 0088 CI 42431176953 success -> `xnu/out-0088`; `shim/out/bonito-ios12-touch.img`
  (0088 + bonito-ios2.adt + the bonito-ios11 cmdline) booted 18:54:02.
- 18:57: **TOUCH WORKS at driver level** (`logs/boot-0088.txt`): ts_1p8 = PM660 GPIO12 on, bus idle
  (sda/scl 1/1), RMI4 PDT: F34 (fw), F01 (query 0x2c ctrl 0x0d data 0x06), F12 (query 0x4b
  ctrl 0x13 data 0x08), F54 (page 1). F12 max 1079x2159, data1 0x08, 10 fingers. HID events
  `touch 0 down at 611,1405` etc. arrived. Vol-up fixed (reads released). GPIO13 found back at
  mode 0/out 0 (PMIC reset since 0087). That boot stalled at mount_hfs (random stall, before
  backboardd: HID 0 clients). Re-booted 0088.
- 19:03: second 0088 boot (`logs/boot-0088b.txt`): lock screen shown; ALL keys reach backboardd
  (power 0x30, vol-up 0xe9 now correct, vol-down 0xea; 1 client) but SpringBoard did not react
  (no display off/on after PosterBoard; only ~20 frames) -> sample SpringBoard next run.
  Touch failed: GPIO12 subtype read 0 (was 0x10) -> old-layout write, ts_1p8 stayed off, I2C
  lines low (sda/scl 0/0), PDT read timeout. Patch **0089**: PMIC reads must agree twice
  (8 tries), only the LV/MV layout (subtype 0x10-0x13) is written, success requires readback
  mode 0x01 / out 0x80 / en 0x80.
- 19:11: 0089 CI 42432525454 success -> `xnu/out-0089`; `shim/out/bonito-ios13-touch.img` = 0089 +
  bonito-ios2.adt + bonito-ios11 cmdline but `sweetsample=SpringBoard,backboardd sweetsamplesecs=60`.
  Booted 19:11:26. Test: lock screen -> power / swipe up; then pull for SMP + SWEET TOUCH lines.
- 19:22: unlock boot log saved (`logs/boot-0089-unlock.txt`, 13 touch events). Next test:
  `shim/out/bonito-ios14-cpu8.img` (0089, **cpus=8**, ADT has 8 CPU nodes), booted 19:22:04.
- 19:25: cpus=8 (`bonito-ios14-cpu8.img`) stalled at `exec mount` (multi-core stall). User chose
  to go back to the 1-core touch image (`bonito-ios13-touch.img`). Diagnosis plan for later:
  cpus=8 + `sweetsample=* sweetsamplesecs=30` (0086 kernel stacks), let it sit stuck ~2 min, pull.
