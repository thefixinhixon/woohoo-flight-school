"""Linux build kit: one AppImage with everything a player needs to build the game from their own dump, nothing to install.

    python3 tools/make_linux_kit.py [--out DIR] [--name NAME]

Runs on Linux with Python 3.8+, `mksquashfs` (squashfs-tools) and `dpkg-deb` or `ar` (binutils); GitHub Actions builds it for
every release (.github/workflows/build-kits.yml). It contains no game code or data. Nothing it downloads is run while the kit
is assembled. Inside the AppImage (usr/):

  python/   standalone CPython with Tk (python-build-standalone): the launcher and the pipeline
  zig/      Zig, used as the C compiler (`zig cc`, Clang/LLVM); C libraries for other platforms removed
  cmake/    CMake (Kitware's build)       bin/ninja   Ninja
  bin/      cc, ar, ranlib (zig wrappers), glslc (LunarG Vulkan SDK)
  sysroot/  SDL2 headers + library (Ubuntu 22.04) and Vulkan headers + loader + libshaderc (LunarG Vulkan SDK) to build
            against; the game uses the system's SDL2 and Vulkan loader when it runs, and the kit's libshaderc next to itself
  source/   this repository's tracked files

Every download is checked against the SHA-256 recorded here (each one matched its project's published checksum when it was
pinned). The kit needs glibc 2.29 or newer on the player's system.
"""
import argparse, glob, hashlib, io, os, shutil, struct, subprocess, sys, tarfile, urllib.request, zipfile, zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GH = 'https://github.com/'
DL = {   # name: (url, sha256)
    'zig': ('https://ziglang.org/download/0.15.2/zig-x86_64-linux-0.15.2.tar.xz',
            '02aa270f183da276e5b5920b1dac44a63f1a49e55050ebde3aecc9eb82f93239'),
    'python': (GH + 'astral-sh/python-build-standalone/releases/download/20261009/'
               'cpython-3.12.15%2B20261009-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz',
               'ffa8f85f1b56e88b687f08d743d817015524b86a9365fd921243132b80f1a36d'),
    'cmake': (GH + 'Kitware/CMake/releases/download/v3.31.12/cmake-3.31.12-linux-x86_64.tar.gz',
              '0dc2e9a6860f06bf10bd8fadc03e35d9eeb4df46e33763a7e480e987758f385c'),
    'ninja': (GH + 'ninja-build/ninja/releases/download/v1.13.1/ninja-linux.zip',
              '0830252db77884957a1a4b87b05a1e2d9b5f658b8367f82999a941884cbe0238'),
    'vulkansdk': ('https://sdk.lunarg.com/sdk/download/1.3.296.0/linux/vulkansdk-linux-x86_64-1.3.296.0.tar.xz',
                  '79b0a1593dadc46180526250836f3e53688a9a5fb42a0e5859eb72316dc4d53e'),
    'sdl2': ('http://archive.ubuntu.com/ubuntu/pool/main/libs/libsdl2/libsdl2-2.0-0_2.0.20+dfsg-2ubuntu1.22.04.1_amd64.deb',
             'ac3cea9ea66df71445541b2cfd5e07f554ba0f83e3ce4a9dacc311c358100c47'),
    'sdl2-dev': ('http://archive.ubuntu.com/ubuntu/pool/universe/libs/libsdl2/libsdl2-dev_2.0.20+dfsg-2ubuntu1.22.04.1_amd64.deb',
                 '9bc7b72116bda5a668c127aa48ebbf3e350bfabeb1b245680da3d7157be9d531'),
    'runtime': (GH + 'AppImage/type2-runtime/releases/download/20251108/runtime-x86_64',
                '2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d'),
}
SDK = '1.3.296.0/x86_64/'
# Zig's C library sources and headers for other systems (the kit only targets x86_64 glibc Linux), C++ and sanitizer runtimes
ZIG_TRIM = ['doc', 'lib/libcxx', 'lib/libcxxabi', 'lib/libtsan', 'lib/libc/mingw', 'lib/libc/musl', 'lib/libc/wasi', 'lib/libc/darwin',
            'lib/libc/freebsd', 'lib/libc/netbsd', 'lib/libc/openbsd']
ZIG_HEADERS_KEEP = {'any-linux-any', 'generic-glibc', 'x86-linux-any', 'x86-linux-gnu'}
PY_TRIM = ['lib/python3.*/test', 'lib/python3.*/idlelib', 'lib/python3.*/turtledemo', 'lib/python3.*/ensurepip',
           'lib/python3.*/lib2to3', 'lib/python3.*/tkinter/test', 'lib/tk*/demos', 'share/man']

APPRUN = r'''#!/bin/sh
# Woohoo! Flight School build kit: the launcher with the kit's compiler and tools first on PATH (nothing is installed).
# "AppImage --cli <pipeline arguments>" runs tools/pipeline.py instead, e.g. --cli check, --cli build my-dump.cia
HERE="$(dirname "$(readlink -f "$0")")"
export PATH="$HERE/usr/bin:$HERE/usr/cmake/bin:$PATH"
export PWR_KIT="$HERE/usr"
export ZIG_GLOBAL_CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/woohoo-flight-school/zig"
if [ "$1" = "--cli" ]; then shift; exec "$HERE/usr/python/bin/python3" "$HERE/usr/source/tools/pipeline.py" "$@"; fi
exec "$HERE/usr/python/bin/python3" "$HERE/usr/source/launcher/pwr_launcher.py" "$@"
'''
WRAP = '#!/bin/sh\n# {what}\nexec "$(dirname "$(readlink -f "$0")")/../zig/zig" {cmd} "$@"\n'
DESKTOP = '''[Desktop Entry]
Type=Application
Name=Woohoo! Flight School
Comment=Build and play your own copy of Pilotwings Resort (3DS) as a native program
Exec=AppRun
Icon=woohoo-flight-school
Categories=Game;
Terminal=false
'''
THIRD_PARTY = '''Woohoo! Flight School Linux build kit: third-party software
=============================================================

The project itself (usr/source) is GPL-3.0-or-later (usr/source/LICENSE). The bundled tools keep their own licences:

- Zig 0.15.2 (usr/zig): MIT (usr/zig/LICENSE). Includes LLVM/Clang (Apache-2.0 WITH LLVM-exception) and C library headers
  and sources under their own licences (glibc: LGPL-2.1-or-later, Linux headers: GPL-2.0 WITH Linux-syscall-note).
  Source: https://ziglang.org/download/0.15.2/zig-0.15.2.tar.xz
- CPython 3.12.15, python-build-standalone 20261009 (usr/python): PSF-2.0, with Tcl/Tk (TCL licence), OpenSSL (Apache-2.0)
  and others listed in usr/python/share or https://github.com/astral-sh/python-build-standalone
- CMake 3.31.12 (usr/cmake): BSD-3-Clause (usr/cmake/share/cmake-3.31/Copyright.txt) https://cmake.org
- Ninja 1.13.1 (usr/bin/ninja): Apache-2.0 https://github.com/ninja-build/ninja
- From the LunarG Vulkan SDK 1.3.296.0: glslc and libshaderc_shared (shaderc, Apache-2.0), the Vulkan loader (Apache-2.0)
  and Vulkan headers (Apache-2.0 OR MIT) https://vulkan.lunarg.com
- SDL 2.0.20 headers and library, Ubuntu 22.04 package 2.0.20+dfsg-2ubuntu1.22.04.1: zlib licence https://libsdl.org
- AppImage type2-runtime 20251108: MIT https://github.com/AppImage/type2-runtime
'''


def fetch(name, cache):
    url, sha = DL[name]; path = os.path.join(cache, url.rsplit('/', 1)[1].replace('%2B', '+'))
    if not (os.path.exists(path) and hashlib.sha256(open(path, 'rb').read()).hexdigest() == sha):
        print('  get', url, flush=True)
        with urllib.request.urlopen(url, timeout=300) as r: data = r.read()
        if hashlib.sha256(data).hexdigest() != sha: sys.exit(f'{url}: SHA-256 mismatch')
        open(path, 'wb').write(data)
    return path


def untar(path, dst, members=None, strip=0):
    """extract (optionally only names under the prefixes in members), dropping `strip` leading path parts"""
    with tarfile.open(path) as t:
        for m in t.getmembers():
            if members and not any(m.name.startswith(p) for p in members): continue
            parts = m.name.split('/')[strip:]
            if not parts or not parts[0]: continue
            m.name = '/'.join(parts)
            if m.islnk(): m.linkname = '/'.join(m.linkname.split('/')[strip:])
            t.extract(m, dst, **({'filter': 'tar'} if sys.version_info >= (3, 12) else {}))


def deb_data(path, dst):
    """the data.tar.* of a .deb (an ar archive) into dst"""
    raw = open(path, 'rb').read(); pos = 8
    while pos < len(raw):
        name = raw[pos:pos + 16].decode().strip().rstrip('/'); size = int(raw[pos + 48:pos + 58])
        body = raw[pos + 60:pos + 60 + size]; pos += 60 + size + (size & 1)
        if name.startswith('data.tar'):
            if name.endswith('.zst'):
                body = subprocess.run(['zstd', '-dc'], input=body, capture_output=True, check=True).stdout
            with tarfile.open(fileobj=io.BytesIO(body)) as t:
                t.extractall(dst, **({'filter': 'tar'} if sys.version_info >= (3, 12) else {}))
            return
    sys.exit(f'{path}: no data.tar')


def icon_png(size=256):
    """the kit's icon: a sun over sea and sky, drawn here (no image tools needed)"""
    rows = []
    for y in range(size):
        row = bytearray([0])
        for x in range(size):
            dx, dy = x - size * 0.68, y - size * 0.34
            if dx * dx + dy * dy < (size * 0.17) ** 2: c = (255, 210, 63)
            elif y > size * 0.62: c = (30, 120 + y * 60 // size, 200)
            else: c = (90 + y * 80 // size, 170 + y * 50 // size, 255)
            if (x - size / 2) ** 2 + (y - size / 2) ** 2 > (size / 2 - 2) ** 2: row += bytes((0, 0, 0, 0))
            else: row += bytes(c + (255,))
        rows.append(bytes(row))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b''))


def rm(base, pats):
    for pat in pats:
        for p in glob.glob(os.path.join(base, pat)): shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out', default=os.path.join(REPO, 'build-linuxkit'), help='work and output folder (default build-linuxkit/)')
    ap.add_argument('--name', default='WoohooFlightSchool-Linux-kit-x86_64', help='AppImage name (without .AppImage)')
    a = ap.parse_args()
    cache = os.path.join(a.out, 'cache'); app = os.path.join(a.out, 'AppDir'); usr = os.path.join(app, 'usr')
    os.makedirs(cache, exist_ok=True)
    if os.path.exists(app): shutil.rmtree(app)
    for d in ('bin', 'sysroot/include', 'sysroot/lib'): os.makedirs(os.path.join(usr, d))

    print('tools:', flush=True)
    untar(fetch('python', cache), os.path.join(usr, 'python'), strip=1); rm(os.path.join(usr, 'python'), PY_TRIM)
    untar(fetch('zig', cache), os.path.join(usr, 'zig'), strip=1); rm(os.path.join(usr, 'zig'), ZIG_TRIM)
    for d in os.listdir(os.path.join(usr, 'zig/lib/libc/include')):
        if d not in ZIG_HEADERS_KEEP: shutil.rmtree(os.path.join(usr, 'zig/lib/libc/include', d))
    untar(fetch('cmake', cache), os.path.join(usr, 'cmake'), strip=1); rm(os.path.join(usr, 'cmake'), ['doc', 'man', 'share/cmake-*/Help'])
    with zipfile.ZipFile(fetch('ninja', cache)) as z: open(os.path.join(usr, 'bin/ninja'), 'wb').write(z.read('ninja'))
    sdk = os.path.join(a.out, 'sdk')
    if os.path.exists(sdk): shutil.rmtree(sdk)
    untar(fetch('vulkansdk', cache), sdk, members=[SDK + 'bin/glslc', SDK + 'lib/libshaderc_shared.so.1', SDK + 'lib/libvulkan.so.1.',
                                                   SDK + 'include/vulkan/', SDK + 'include/vk_video/'], strip=2)
    shutil.copy2(os.path.join(sdk, 'bin/glslc'), os.path.join(usr, 'bin/glslc'))
    shutil.copy2(os.path.join(sdk, 'lib/libshaderc_shared.so.1'), os.path.join(usr, 'sysroot/lib/'))
    shutil.copy2(glob.glob(os.path.join(sdk, 'lib/libvulkan.so.1.*'))[0], os.path.join(usr, 'sysroot/lib/libvulkan.so'))
    for d in ('vulkan', 'vk_video'): shutil.copytree(os.path.join(sdk, 'include', d), os.path.join(usr, 'sysroot/include', d))
    deb = os.path.join(a.out, 'deb')
    if os.path.exists(deb): shutil.rmtree(deb)
    deb_data(fetch('sdl2', cache), deb); deb_data(fetch('sdl2-dev', cache), deb)
    shutil.copy2(glob.glob(os.path.join(deb, 'usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0.*'))[0], os.path.join(usr, 'sysroot/lib/libSDL2-2.0.so.0'))
    shutil.copytree(os.path.join(deb, 'usr/include/SDL2'), os.path.join(usr, 'sysroot/include/SDL2'))
    shutil.copy2(os.path.join(deb, 'usr/include/x86_64-linux-gnu/SDL2/_real_SDL_config.h'), os.path.join(usr, 'sysroot/include/SDL2/'))
    for name, what, cmd in (('cc', 'C compiler: Zig\'s Clang (UBSan traps off, as with GCC)', 'cc -fno-sanitize=undefined'),
                            ('ar', 'archiver', 'ar'), ('ranlib', 'archive index', 'ranlib')):
        open(os.path.join(usr, 'bin', name), 'w').write(WRAP.format(what=what, cmd=cmd))
    for f in os.listdir(os.path.join(usr, 'bin')): os.chmod(os.path.join(usr, 'bin', f), 0o755)

    print('source:', flush=True)
    files = subprocess.run(['git', '-C', REPO, 'ls-files', '-z'], capture_output=True, check=True).stdout.decode().split('\0')
    for f in filter(None, files):
        dst = os.path.join(usr, 'source', f); os.makedirs(os.path.dirname(dst), exist_ok=True); shutil.copy2(os.path.join(REPO, f), dst)

    open(os.path.join(app, 'AppRun'), 'w').write(APPRUN); os.chmod(os.path.join(app, 'AppRun'), 0o755)
    open(os.path.join(app, 'woohoo-flight-school.desktop'), 'w').write(DESKTOP)
    open(os.path.join(app, 'woohoo-flight-school.png'), 'wb').write(icon_png())
    os.symlink('woohoo-flight-school.png', os.path.join(app, '.DirIcon'))
    open(os.path.join(app, 'THIRD-PARTY.txt'), 'w').write(THIRD_PARTY)
    for p in glob.glob(os.path.join(usr, '**', '__pycache__'), recursive=True): shutil.rmtree(p, ignore_errors=True)

    print('squashfs:', flush=True)
    sq = os.path.join(a.out, a.name + '.squashfs'); out = os.path.join(a.out, a.name + '.AppImage')
    for p in (sq, out):
        if os.path.exists(p): os.remove(p)
    subprocess.run(['mksquashfs', app, sq, '-root-owned', '-noappend', '-comp', 'zstd', '-Xcompression-level', '19', '-quiet',
                    '-no-progress'], check=True)
    with open(out, 'wb') as f:
        f.write(open(fetch('runtime', cache), 'rb').read()); f.write(open(sq, 'rb').read())
    os.chmod(out, 0o755); os.remove(sq)
    mb = lambda p: sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(p) for f in fs) / 1e6
    print(f'  {out}: {os.path.getsize(out) / 1e6:.0f} MB ({mb(app):.0f} MB unpacked)')


if __name__ == '__main__':
    main()
