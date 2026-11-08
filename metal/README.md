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
