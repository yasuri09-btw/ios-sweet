#!/usr/bin/env python3
"""bonito (Pixel 3a XL) changes on top of tools/sweet-root-tweaks.py.

  doas bonito-root-tweaks.py MOUNTED_ROOT

Idempotent; run as root on the HFS+ root image mounted read-write.

  auearlyboot   MobileAccessoryUpdater's early-boot task never exits on bonito
                (boot 2026-11-07: last console line `exec auearlyboot`, after
                init_featureflags). In launchd's built-in Boot list it has no
                RequireSuccess/RequireRun, i.e. it is optional, so the binary is
                renamed: launchd cannot spawn it and goes on with the next task.
"""
import os, sys

DISABLE = [
    'System/Library/PrivateFrameworks/MobileAccessoryUpdater.framework/Support/auearlyboot',
]


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


if __name__ == '__main__':
    main()
