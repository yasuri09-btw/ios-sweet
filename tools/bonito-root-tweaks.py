#!/usr/bin/env python3
"""bonito (Pixel 3a XL) changes on top of tools/sweet-root-tweaks.py.

  doas bonito-root-tweaks.py MOUNTED_ROOT [--remove-jobs | --restore-jobs ORIG_LAUNCHD_PLIST]

Idempotent; run as root on the HFS+ root image mounted read-write.

  auearlyboot   MobileAccessoryUpdater's early-boot task never exits on bonito
                (boot 2026-11-07: last console line `exec auearlyboot`, after
                init_featureflags). In launchd's built-in Boot list it has no
                RequireSuccess/RequireRun, i.e. it is optional, so the binary is
                renamed: launchd cannot spawn it and goes on with the next task.
  launchd.plist bluetoothd (KeepAlive SuccessfulExit=false) and fairplayd.H2
                (KeepAlive=true) relaunch forever without their hardware (also on
                sweet), flooding the console and taking CPU from the first boot:
                their LaunchDaemons entries can be removed (--remove-jobs).
                NOT the default: with them removed, two boots (2026-11-07) stopped
                early (after the xpcroleaccountd boot task, then after usermanagerd's
                `Adding 99 entry to persona Table`), while the image that kept them
                reached the Apple logo. --restore-jobs copies them back from the
                IPSW's original /System/Library/xpc/launchd.plist.
"""
import os, plistlib, sys

DISABLE = [
    'System/Library/PrivateFrameworks/MobileAccessoryUpdater.framework/Support/auearlyboot',
]
REMOVE_JOBS = ('com.apple.bluetoothd', 'com.apple.fairplayd.H2')


def launchd_jobs(root, mode, orig=None):
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    with open(path, 'rb') as f:
        plist = plistlib.load(f)
    jobs = plist['LaunchDaemons']
    if mode == 'remove':
        changed = [k for k, j in jobs.items() if j.get('Label') in REMOVE_JOBS]
        for k in changed:
            del jobs[k]
    else:
        with open(orig, 'rb') as f:
            src = plistlib.load(f)['LaunchDaemons']
        changed = [k for k, j in src.items() if j.get('Label') in REMOVE_JOBS and k not in jobs]
        for k in changed:
            jobs[k] = src[k]
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    print(f'launchd.plist: {mode}d', [k.split('/')[-1] for k in changed] or 'nothing')


def main():
    args = sys.argv[1:]
    if not args or not os.path.isdir(os.path.join(args[0], 'System')) or not (
            len(args) == 1 or args[1:] == ['--remove-jobs'] or
            (len(args) == 3 and args[1] == '--restore-jobs')):
        sys.exit(__doc__)
    root = args[0]
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
    if args[1:2] == ['--remove-jobs']:
        launchd_jobs(root, 'remove')
    elif args[1:2] == ['--restore-jobs']:
        launchd_jobs(root, 'restore', args[2])


if __name__ == '__main__':
    main()
