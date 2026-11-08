#!/bin/bash
# Stage the boot-image file set for Metal-on-Turnip tests and print the list path:
#   tools/bonito-files.sh [RUN_TXT]    -> research/gpu/files/list   (for mkpayload.py --files)
#
# Installs into the phone's root (bsd/dev/sweet_bootfiles.c, before launchd):
#   /var/local/lib/libvulkan_freedreno.dylib   research/gpu/turnip-ios/   (metal.yml artifact)
#   /var/local/bin/{sweetrun,vktest,v2test}    research/gpu/phone-tests/  (phone.yml artifact)
#   /var/local/run.txt                         RUN_TXT (default metal/phone/run.txt)
#   /System/Library/xpc/launchd.plist          research/gpu/launchd.plist.orig (the cache from
#                                              research/ipsw-x/ios16-root.img = what is flashed)
#                                              + job org.sweet.run (sweetrun; log /var/local/run.log)
# With research/gpu/metal2-ios/ (metal2.yml artifact) also:
#   /System/Library/Extensions/SweetMetal2.bundle/  (+ Turnip inside it; sweetmtl=2 loads it)
#   /var/local/bin/m2vtest, /var/local/mtl2.log (empty, 666: SweetMetal2's log)
# Everything under research/ stays out of git (Apple data, build artifacts).
set -euo pipefail
cd "$(dirname "$0")/.."
run=${1:-metal/phone/run.txt}
S=research/gpu/files
rm -rf "$S"; mkdir -p "$S"
cp research/gpu/turnip-ios/libvulkan_freedreno.dylib "$S/"
for b in sweetrun vktest v2test; do cp research/gpu/phone-tests/$b "$S/"; done
cp "$run" "$S/run.txt"
python3 -I - research/gpu/launchd.plist.orig "$S/launchd.plist" <<'PY'
import plistlib, sys
p = plistlib.load(open(sys.argv[1], 'rb'))
p['LaunchDaemons']['/System/Library/LaunchDaemons/org.sweet.run.plist'] = {
    'Label': 'org.sweet.run',
    'ProgramArguments': ['/var/local/bin/sweetrun'],
    'RunAtLoad': True, 'UserName': 'root', 'LaunchOnlyOnce': True,
    'EnvironmentVariables': {'PATH': '/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin'},
    'StandardOutPath': '/var/local/sweetrun.out', 'StandardErrorPath': '/var/local/sweetrun.out',
}
plistlib.dump(p, open(sys.argv[2], 'wb'), fmt=plistlib.FMT_BINARY)
PY
cat > "$S/list" <<'L'
dir /var/local 755
file libvulkan_freedreno.dylib /var/local/lib/libvulkan_freedreno.dylib 755
file sweetrun /var/local/bin/sweetrun 755
file vktest /var/local/bin/vktest 755
file v2test /var/local/bin/v2test 755
file run.txt /var/local/run.txt 644
file launchd.plist /System/Library/xpc/launchd.plist 644
L
M=research/gpu/metal2-ios
if [ -d "$M" ]; then
	cp "$M/m2vtest" "$S/"
	cp "$M/SweetMetal2.bundle/SweetMetal2" "$S/SweetMetal2"
	cp "$M/SweetMetal2.bundle/Info.plist" "$S/Info.plist"
	: > "$S/mtl2.log"
	X=/System/Library/Extensions/SweetMetal2.bundle
	cat >> "$S/list" <<L
file m2vtest /var/local/bin/m2vtest 755
file SweetMetal2 $X/SweetMetal2 755
file Info.plist $X/Info.plist 644
file libvulkan_freedreno.dylib $X/libvulkan_freedreno.dylib 755
file mtl2.log /var/local/mtl2.log 666
dir /var/local/m2v-cache 777
dir /var/local/m2v-cache/backboardd 777
L
fi
echo "$S/list"
