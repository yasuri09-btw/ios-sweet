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
