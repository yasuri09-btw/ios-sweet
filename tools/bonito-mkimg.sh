#!/bin/bash
# Build a bonito iOS boot image with the Adreno firmware and the boot files.
#   tools/bonito-mkimg.sh KERNEL NAME [EXTRA BOOT-ARGS] -> shim/out/bonito-NAME.img
# Example: tools/bonito-mkimg.sh xnu/out-0097/kernel.development.sweet ios32-mtl2 sweetmtl=2
# Boot files: research/gpu/files/list (tools/bonito-files.sh); no list, no files.
set -euo pipefail
cd "$(dirname "$0")/.."
K=$1 NAME=$2 EXTRA=${3:-}
A=shim/out/bonito-ios2.adt
B="debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress rd=emmcroot0 emmcrw=1 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_hz=30 sweetfb_noconsole=1 sweetusb=0 ufslogsecs=2 io_throttle_window_tier1=0 io_throttle_window_tier2=0 io_throttle_window_tier3=0 io_throttle_period_tier1=1 io_throttle_period_tier2=1 io_throttle_period_tier3=1 sweetgpu=1"
C="$B $EXTRA"
P=$PWD/shim/out/payload-bonito-$NAME.bin
F=()
[ -f research/gpu/files/list ] && F=(--files research/gpu/files/list)
python3 -I tools/mkpayload.py "$K" "$A" "$P" --gpufw research/gpu/fw/bonito "${F[@]}" --cmdline "$C" | grep -E "^(files|size):"
MKBOOTIMG="python3 /home/ada/codestuff/tools/mkbootimg/mkbootimg.py" bash shim/build.sh "$P" > /dev/null 2>&1
cp shim/out/shim-boot.img "shim/out/bonito-$NAME.img"
ls -l "shim/out/bonito-$NAME.img"
