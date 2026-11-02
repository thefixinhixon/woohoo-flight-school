"""Windows build kit: everything a player needs to build the game from their own dump on Windows, without MSYS2.

    python3 tools/make_windows_kit.py [--out DIR] [--name NAME]

Runs on Linux (GitHub Actions: .github/workflows/build-kits.yml) or anywhere with Python 3.8+ and `zstd` (or Python 3.14+).
It contains no game code or data. Layout of the zip:

  <name>/Woohoo! Flight School.bat   starts the launcher with the kit's tools first on PATH
  <name>/source/                     this repository's tracked files (launcher, pipeline, recompiler, runtime)
  <name>/mingw64/                    MSYS2 MINGW64 packages: GCC, CMake, Ninja, SDL2, Vulkan headers + loader, shaderc,
                                     Python with Tk; documentation and test suites removed
  <name>/THIRD-PARTY.txt             every package with its version, licence and where its source code is
  <name>/README.txt

Packages come from repo.msys2.org (the newest versions at kit build time, checked against the repository's SHA-256 sums),
with all their dependencies resolved from the repository database.
"""
import argparse, hashlib, io, os, re, shutil, subprocess, sys, tarfile, urllib.request, zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MIRROR = 'https://repo.msys2.org/mingw/mingw64/'
SOURCES = 'https://repo.msys2.org/mingw/sources/'
ROOTS = ['gcc', 'cmake', 'ninja', 'SDL2', 'vulkan-headers', 'vulkan-loader', 'shaderc', 'python', 'tk']
PFX = 'mingw-w64-x86_64-'
# removed from mingw64/: not needed to build or run (documentation, translations, test suites, IDLE, Tcl/Tk demos)
TRIM = ['share/doc', 'share/man', 'share/info', 'share/locale', 'share/gtk-doc', 'share/gettext', 'share/aclocal',
        'lib/python*/test', 'lib/python*/idlelib', 'lib/python*/turtledemo', 'lib/python*/ensurepip', 'lib/python*/lib2to3',
        'lib/python*/tkinter/test', 'lib/tk*/demos', 'share/cmake-*/Help', 'lib/cmake/llvm',
        # C only: no C++ compiler, no LTO; static libraries of the bundled tools themselves (they run from their DLLs)
        'lib/gcc/x86_64-w64-mingw32/*/cc1plus.exe', 'lib/gcc/x86_64-w64-mingw32/*/lto1.exe', 'include/c++',
        'lib/libSPIRV-Tools*.a', 'lib/libglslang*.a', 'lib/libshaderc*.a', 'lib/libcrypto.a', 'lib/libssl.a']

LAUNCH_BAT = r'''@echo off
rem Woohoo! Flight School: starts the launcher with the kit's compiler and tools first on PATH (nothing is installed)
setlocal
set "KIT=%~dp0"
set "PATH=%KIT%mingw64\bin;%PATH%"
start "" "%KIT%mingw64\bin\pythonw.exe" "%KIT%source\launcher\pwr_launcher.py"
'''

README = '''Woohoo! Flight School: Windows build kit
=========================================

Builds and plays your own copy of Pilotwings Resort (3DS, USA) on Windows 10/11 (64-bit).
This kit contains no game code or data. You need a decrypted dump of the game (.cia, .3ds/.cci or .cxi).

1. Unzip this folder anywhere (a short path without special characters works best).
2. Double-click "Woohoo! Flight School.bat".
3. Game ROM: Browse to your dump. Build (a few minutes). Then Launch.

The game is built into %LOCALAPPDATA%\\pwr-port (bin\\pwr.exe and its DLLs); profiles, saves and settings are kept
there too. After building, the kit folder is only needed to rebuild.

You need a GPU driver with Vulkan 1.3 (current AMD, NVIDIA and Intel drivers have it).

The built pwr.exe contains code translated from the game: do not share it.

Project, instructions and issues: https://github.com/thefixinhixon/woohoo-flight-school
Licence: GPL-3.0-or-later (source/LICENSE). The bundled tools keep their own licences: THIRD-PARTY.txt.
Not affiliated with Nintendo or Monster Games.
'''


def fetch(url, dest=None):
    print('  get', url, flush=True)
    with urllib.request.urlopen(url, timeout=120) as r: data = r.read()
    if dest:
        with open(dest, 'wb') as f: f.write(data)
    return data


def unzstd(data):
    try:
        from compression import zstd   # Python 3.14+
        return zstd.decompress(data)
    except ImportError:
        return subprocess.run(['zstd', '-dc'], input=data, capture_output=True, check=True).stdout


def read_db(data):
    """{name: desc dict} and {provided name: name} from the repository database"""
    if data[:4] == b'\x28\xb5\x2f\xfd': data = unzstd(data)
    pkgs, provides = {}, {}
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        for m in t.getmembers():
            if not m.name.endswith('/desc'): continue
            d, key = {}, None
            for line in t.extractfile(m).read().decode().splitlines():
                if line.startswith('%') and line.endswith('%'): key = line.strip('%'); d[key] = []
                elif line and key: d[key].append(line)
            name = d['NAME'][0]; pkgs[name] = d
            for p in d.get('PROVIDES', []): provides.setdefault(re.split('[<>=]', p)[0], name)
    return pkgs, provides


def resolve(pkgs, provides, roots):
    need, todo = {}, [PFX + r for r in roots]
    while todo:
        dep = re.split('[<>=]', todo.pop())[0]
        n = dep if dep in pkgs else provides.get(dep)
        if not n: raise SystemExit(f'unresolved dependency {dep}')
        if n in need: continue
        need[n] = pkgs[n]; todo += pkgs[n].get('DEPENDS', [])
    return need


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out', default=os.path.join(REPO, 'build-kit'), help='work and output folder (default build-kit/)')
    ap.add_argument('--name', default='WoohooFlightSchool-Windows-kit', help='folder and zip name')
    a = ap.parse_args()
    cache = os.path.join(a.out, 'cache'); kit = os.path.join(a.out, a.name)
    os.makedirs(cache, exist_ok=True)
    if os.path.exists(kit): shutil.rmtree(kit)
    os.makedirs(kit)

    print('packages:', flush=True)
    pkgs, provides = read_db(fetch(MIRROR + 'mingw64.db'))
    need = resolve(pkgs, provides, ROOTS)
    for n, d in sorted(need.items()):
        fn = d['FILENAME'][0]; path = os.path.join(cache, fn)
        if not (os.path.exists(path) and hashlib.sha256(open(path, 'rb').read()).hexdigest() == d['SHA256SUM'][0]):
            data = fetch(MIRROR + fn, path)
            if hashlib.sha256(data).hexdigest() != d['SHA256SUM'][0]: raise SystemExit(f'{fn}: SHA-256 mismatch')
        with tarfile.open(fileobj=io.BytesIO(unzstd(open(path, 'rb').read()))) as t:
            ms = [m for m in t.getmembers() if m.name.startswith('mingw64/') and (m.isfile() or m.isdir() or m.issym() or m.islnk())]
            t.extractall(kit, members=ms, **({'filter': 'tar'} if sys.version_info >= (3, 12) else {}))
    print(f'  {len(need)} packages')

    import glob
    for pat in TRIM:
        for p in glob.glob(os.path.join(kit, 'mingw64', pat)): shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
    for p in glob.glob(os.path.join(kit, 'mingw64', '**', '__pycache__'), recursive=True): shutil.rmtree(p, ignore_errors=True)
    for exe in ('pythonw.exe', 'gcc.exe', 'cmake.exe', 'ninja.exe', 'glslc.exe', 'SDL2.dll', 'libshaderc_shared.dll'):
        if not os.path.exists(os.path.join(kit, 'mingw64', 'bin', exe)): raise SystemExit(f'kit lacks mingw64/bin/{exe}')
    if not glob.glob(os.path.join(kit, 'mingw64', 'lib', 'python3*', 'lib-dynload', '_tkinter*')):
        raise SystemExit('kit lacks Python\'s _tkinter module (the launcher needs Tk)')

    print('source:', flush=True)
    files = subprocess.run(['git', '-C', REPO, 'ls-files', '-z'], capture_output=True, check=True).stdout.decode().split('\0')
    for f in filter(None, files):
        dst = os.path.join(kit, 'source', f); os.makedirs(os.path.dirname(dst), exist_ok=True); shutil.copy2(os.path.join(REPO, f), dst)
    print(f'  {len(files) - 1} files')

    with open(os.path.join(kit, 'Woohoo! Flight School.bat'), 'w', newline='\r\n') as f: f.write(LAUNCH_BAT)
    with open(os.path.join(kit, 'README.txt'), 'w', newline='\r\n') as f: f.write(README)
    with open(os.path.join(kit, 'THIRD-PARTY.txt'), 'w', newline='\r\n') as f:
        f.write('Third-party software in mingw64/, as packaged by MSYS2 (https://www.msys2.org/). Each package keeps its own\n'
                'licence. The source code of each package version is available from MSYS2 at the address given (kept there\n'
                'for every published package version), and from the project\'s upstream.\n\n')
        for n, d in sorted(need.items()):
            v = d['VERSION'][0]; base = d.get('BASE', [n])[0]
            f.write(f'{n} {v}\n  licence: {", ".join(d.get("LICENSE", ["see package"]))}\n'
                    f'  upstream: {d.get("URL", ["-"])[0]}\n  source: {SOURCES}{base}-{v}.src.tar.zst\n')

    z = os.path.join(a.out, a.name + '.zip')
    print('zip:', z, flush=True)
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for root, dirs, fs in os.walk(kit):
            dirs.sort()
            for fn in sorted(fs):
                p = os.path.join(root, fn)
                if os.path.islink(p):   # MSYS2 packages hardly use links; store the target's content
                    p = os.path.realpath(p)
                    if not os.path.isfile(p): continue
                zf.write(p, os.path.relpath(os.path.join(root, fn), a.out))
    mb = lambda p: sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(p) for f in fs) / 1e6
    print(f'  kit {mb(kit):.0f} MB unpacked, zip {os.path.getsize(z) / 1e6:.0f} MB')


if __name__ == '__main__':
    main()
