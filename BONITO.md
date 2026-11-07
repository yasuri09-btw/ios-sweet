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

## Next
- eMMC root driver (replace sweet_ufs: 0028/0030/0034/0069) for sdhc_1, rd=emmcroot0 or similar.
- Persistent log: sweet's 0047 writes to the end of UFS userdata; needs the eMMC driver.
- Touch: RMI4 over GENI I2C9 (sweet: Goodix on its own SE; 0060/0062/0063/0071).
- Then the iOS root image (mkiosroot.sh) into userdata, D22AP /product (d22ap.lzfse needs lzfse).
