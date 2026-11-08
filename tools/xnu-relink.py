#!/usr/bin/env python3
"""Relink a SWEET kernel locally from the CI's kernel link kit, with some objects replaced.

  xnu-relink.py LINKKIT LINKLIBS OUT [OBJ ...]

LINKKIT   kernel.development.sweet.link/ from a CI xnu-kernel artifact
          (kernel.development.sweet.a = every kernel object in link order, plus
          .linkarguments / .linkarchives = the link flags)
LINKLIBS  directory with libfirehose_kernel.a and libcc_kext.a (xnu-kernel
          artifact out/linklibs/, xnu.yml)
OUT       the kernel Mach-O to write
OBJ       replacement objects (same file name as the archive member, e.g.
          sweet_gpu.o from tools/xnu-cc.py with XNU_CC_OBJDIR); a name that is not
          in the archive is added at the end

Links with LLVM's ld64.lld instead of Apple's ld64. The Apple link flags are
translated: -Wl,x,y -> x y, -mmacosx-version-min -> -platform_version, compiler
flags dropped. Verify a relink of an unchanged kit against the CI kernel before
trusting it (segments and symbol addresses).
"""
import os, shlex, shutil, subprocess, sys, tempfile

LD = os.environ.get('LD64', 'ld64.lld')


def main():
    kit, libs, out = sys.argv[1:4]
    repl = {os.path.basename(p): os.path.abspath(p) for p in sys.argv[4:]}
    name = [f for f in os.listdir(kit) if f.endswith('.a')][0][:-2]
    args = shlex.split(open(os.path.join(kit, name + '.linkarguments')).read())
    args += shlex.split(open(os.path.join(kit, name + '.linkarchives')).read())

    work = tempfile.mkdtemp(prefix='xnu-relink-')
    try:
        members = subprocess.run(['llvm-ar', 't', os.path.join(kit, name + '.a')], check=True,
                                 capture_output=True, text=True).stdout.split()
        subprocess.run(['llvm-ar', 'x', os.path.abspath(os.path.join(kit, name + '.a'))], check=True, cwd=work)
        files = []
        for m in members:
            files.append(repl.pop(m, os.path.join(work, m)))
        files += list(repl.values())
        with open(os.path.join(work, 'filelist'), 'w') as f:
            f.write('\n'.join(files) + '\n')

        ld = [LD, '-o', os.path.abspath(out), '-filelist', os.path.join(work, 'filelist')]
        i = 0
        while i < len(args):
            a = args[i]
            if a.startswith('-Wl,'):
                ld += a[4:].split(',')
            elif a == '-arch':
                ld += ['-arch', args[i + 1]]
                i += 1
            elif a.startswith('-mmacosx-version-min='):
                v = a.split('=', 1)[1]
                ld += ['-platform_version', 'macos', v, v]
            elif a.startswith('-L'):
                pass                              # CI paths; LINKLIBS instead
            elif a.startswith('-l'):
                if a not in ld:
                    ld.append(a)
            elif a in ('-nostdlib', '-fapple-kext') or a.startswith('-D'):
                pass
            else:
                ld.append(a)
            i += 1
        ld += ['-L', os.path.abspath(libs)]
        # Apple ld64 options lld does not know (layout-neutral): drop them.
        drop = {'-new_linker', '-version_load_command', '-function_starts'}
        ld = [x for x in ld if x not in drop]
        print(' '.join(shlex.quote(x) for x in ld[:6]), '...', file=sys.stderr)
        r = subprocess.run(ld)
        sys.exit(r.returncode)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
