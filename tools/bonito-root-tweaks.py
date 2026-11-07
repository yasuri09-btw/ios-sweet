#!/usr/bin/env python3
"""bonito (Pixel 3a XL) changes on top of tools/sweet-root-tweaks.py.

  doas bonito-root-tweaks.py MOUNTED_ROOT

Idempotent; run as root on the HFS+ root image mounted read-write.

  auearlyboot   MobileAccessoryUpdater's early-boot task never exits on bonito
                (boot 2026-11-07: last console line `exec auearlyboot`, after
                init_featureflags). In launchd's built-in Boot list it has no
                RequireSuccess/RequireRun, i.e. it is optional, so the binary is
                renamed: launchd cannot spawn it and goes on with the next task.
  launchd.plist bluetoothd (KeepAlive SuccessfulExit=false) and fairplayd.H2
                (KeepAlive=true) relaunch forever without their hardware (also on
                sweet), flooding the console and taking CPU from the first boot:
                their LaunchDaemons entries are removed, like sweet's CommCenter.
"""
import os, plistlib, sys

DISABLE = [
    'System/Library/PrivateFrameworks/MobileAccessoryUpdater.framework/Support/auearlyboot',
]
REMOVE_JOBS = ('com.apple.bluetoothd', 'com.apple.fairplayd.H2')


def launchd_jobs(root):
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    with open(path, 'rb') as f:
        plist = plistlib.load(f)
    jobs = plist['LaunchDaemons']
    gone = [k for k, j in jobs.items() if j.get('Label') in REMOVE_JOBS]
    for k in gone:
        del jobs[k]
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    print('launchd.plist: removed', [k.split('/')[-1] for k in gone] or 'nothing (already removed)')


def main():
    if len(sys.argv) != 2 or not os.path.isdir(os.path.join(sys.argv[1], 'System')):
        sys.exit(__doc__)
    root = sys.argv[1]
    for rel in DISABLE:
        path = os.path.join(root, rel)
        off = path + '.bonito-disabled'
        if os.path.exists(path):
            os.rename(path, off)
            print('disabled', rel)
        elif os.path.exists(off):
            print('already disabled', rel)
        else:
            print('missing', rel)
    launchd_jobs(root)


if __name__ == '__main__':
    main()
