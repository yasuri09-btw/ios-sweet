# Metal on bonito (userland GPU stack)

Built on GitHub's macOS runners (`.github/workflows/metal.yml`), since they carry the iOS SDK.

- `mesa-patches/` — UniXNU's Mesa patches at UniXNU commit
  `d07f3c3fc44fe3932be47af3cd5f3c169a531ee4` (BSD-2-Clause-Patent, Copyright (c) 2026 UniXNU
  contributors), in the order of its `scripts/data/adreno-compiler-inputs.json`, for Mesa
  `85fda3b36405bcaea8bffc64aa4f2651dc90770d`. `05-turnip-darwin-kmd.patch` adds Turnip's Darwin
  kernel backend (`freedreno-kmds=darwin`).
- `turnip/NativeAdrenoABI.h` — the user-client wire ABI v1 the backend speaks (MIT; copy of
  ios-flame's `phone/turnip/NativeAdrenoABI.h`). Our kernel must implement it (flame patch 0115
  adapted to the A615: 32-bit GPU VA, chip id 0x06010500, GMEM 512K).

The recipe (iOS 16.0 minimum, release, NDEBUG, shader cache off) follows ios-flame's
`tools/build-turnip-ios.sh`. Output artifact `turnip-ios`: `libvulkan_freedreno.dylib`.

## Phone test programs (`phone/`, `.github/workflows/phone.yml` → artifact `phone-tests`)

- `sweetrun.c` — launchd job `org.sweet.run`: runs `/var/local/run.txt` once per boot, everything
  logged to `/var/local/run.log` (read it from a userdata dump: TWRP `dd` of the first 16 GiB,
  `mount -t hfsplus -o loop,ro,force`).
- `vktest.c`, `v2test.cpp`, `shaders/` — ios-flame's Turnip smoke test and raw v2 user-client test
  (MIT, `phone/turnip/`), stdout line-buffered for the console.

They reach the phone in the boot image: `tools/mkpayload.py --files LIST` (see
`tools/bonito-files.sh`), written into the root by the kernel before launchd starts
(`bsd/dev/sweet_bootfiles.c`, patch 0095). Boot-arg `sweetmtl=1` publishes the user client without
the Metal plugin properties, so Core Animation stays on the CPU while the tests run.
