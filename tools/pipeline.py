"""ROM -> native build -> launch: the static recompilation pipeline, for the command line and the launcher.

usage:
  python3 tools/pipeline.py check                          build tools present?
  python3 tools/pipeline.py verify <rom>                   container, title, encryption, code hash
  python3 tools/pipeline.py build <rom> [--jobs N] [--compiler gcc|clang] [--debug] [--force]
  python3 tools/pipeline.py run [--profile NAME] [-- extra pwr args]
  python3 tools/pipeline.py profiles [list | create NAME | delete NAME]
  common: --home DIR   install folder (default $PWR_HOME, else ~/.local/share/pwr-port or %LOCALAPPDATA%\\pwr-port)

Stages of build (each prints "[n/5] <stage>"):
  1 verify     the ROM is a CIA, CCI/.3ds or CXI holding a supported title, not encrypted, and its decompressed
               code.bin has the SHA-256 this recompiler was written against (function seeds, hooks and addresses
               in recomp/ are specific to that binary)
  2 extract    tools/cia.py --runtime-only -> <home>/game/extracted/{code.bin, exheader.bin, romfs.bin}
  3 recompile  recomp/arm2c.py -> <home>/game/build/gen/*.c (the game's ARM code as C)
  4 compile    CMake + GCC/Clang (Ninja when available) -> <home>/game/build/pwr
  5 install    <home>/bin/pwr and <home>/manifest.json; on Windows also the DLLs it needs (SDL2, winpthread, shaderc)
Windows: build from an MSYS2 MINGW64 shell (README.md lists the packages); Ninja is required there, as CMake would
otherwise pick Visual Studio, whose compiler cannot build the generated code.
Nothing from the ROM leaves the install folder; the repository holds only the recompiler and the runtime.

Install folder:
  bin/pwr[.exe]          the native game (+ its DLLs on Windows)
  game/extracted/        files the game reads at run time
  game/build/            CMake build tree (generated C in gen/)
  manifest.json          title, code hash, build time, recompiler revision
  profiles/<name>/       save/ (the game's save data) + settings.json (window, layout, filters, 3D resolution, audio,
                         extra arguments; the in-game F1 overlay writes its changes back here)
"""
import argparse, hashlib, json, os, platform, shutil, struct, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import cia  # noqa: E402

# Titles the recompiler supports. code_sha256 is of the decompressed ExeFS .code (what arm2c.py translates).
SUPPORTED = {
    0x0004000000031C00: dict(name='Pilotwings Resort (USA)', product='CTR-P-AWAE',
                             code_sha256='5c8d6c2626743f5afb17cfdf82fff9b285cab925c15384ef7c6ab8d3e6d35470'),
}

DEFAULT_SETTINGS = dict(scale=2, fullscreen='off', layout='stacked', auto_gameplay='large', aspect='keep', filter='nearest', fsr_sharpness=0.25,
                        resolution=1, aniso=1, show_fps=False, select_layout=True, free_look=False, draw_distance=1, mute=False, args='')
EXE = 'pwr.exe' if os.name == 'nt' else 'pwr'
# Windows: DLLs copied next to pwr.exe. vulkan-1.dll is not among them: the GPU driver installs the system's loader.
WIN_DLLS = ['SDL2.dll', 'libwinpthread-1.dll']
WIN_DLLS_OPTIONAL = [('libshaderc_shared.dll', 'shaderc_shared.dll')]   # translated vertex shaders; without it a slower GPU interpreter
# Linux build kit (tools/make_linux_kit.py, an AppImage): $PWR_KIT is its usr/ folder with bin/ (cc = zig cc, ar, ranlib, ninja,
# glslc) and sysroot/ (SDL2 and Vulkan headers and link libraries, libshaderc). pwr then uses the system's SDL2 and Vulkan loader.
KIT = os.environ.get('PWR_KIT') if os.name != 'nt' else None


def default_home():
    if os.environ.get('PWR_HOME'): return os.environ['PWR_HOME']
    if os.name == 'nt': return os.path.join(os.environ.get('LOCALAPPDATA', os.path.expanduser('~')), 'pwr-port')
    return os.path.join(os.environ.get('XDG_DATA_HOME', os.path.expanduser('~/.local/share')), 'pwr-port')


class PipelineError(Exception):
    pass


# ---------------------------------------------------------------- 0 prerequisites
def check(log=print):
    """returns a list of (tool, path or None, hint); a None path means missing"""
    have = lambda *names: next((shutil.which(n) for n in names if shutil.which(n)), None)
    rows = [
        ('python3 >= 3.8', sys.executable if sys.version_info >= (3, 8) else None, 'install Python 3.8+'),
        ('cmake >= 3.16', have('cmake'), 'install CMake'),
        ('C compiler', have('cc', 'gcc', 'clang', 'clang-cl'), 'install GCC or Clang (MSVC cl.exe cannot build the generated code)'),
        ('glslc', have('glslc'), 'install shaderc / the Vulkan SDK (compiles the renderer shaders)'),
        ('ninja (optional)' if os.name != 'nt' else 'ninja', have('ninja'),
         'optional: faster builds' if os.name != 'nt' else 'install Ninja (MSYS2: mingw-w64-x86_64-ninja)'),
    ]
    if KIT:
        rows.append(('SDL2 (system)', _system_sdl2(), 'install SDL2 with your package manager (libsdl2-2.0-0, SDL2 or sdl2)'))
    if os.name == 'nt':
        rows.append(('SDL2.dll', _find_dll('SDL2.dll'), 'install SDL2 (MSYS2: mingw-w64-x86_64-SDL2)'))
        rows.append(('shaderc DLL (optional)', _find_dll(*WIN_DLLS_OPTIONAL[0]), 'optional: faster vertex shaders (MSYS2: mingw-w64-x86_64-shaderc)'))
    for tool, path, hint in rows:
        log(f'{tool:18} {path or "MISSING: " + hint}')
    log('SDL2 and Vulkan development files are checked by CMake in the compile stage.')
    return rows


def _system_sdl2():
    """the system's SDL2 library, which a kit-built pwr loads at run time, or None"""
    import ctypes, ctypes.util
    for n in ('libSDL2-2.0.so.0', ctypes.util.find_library('SDL2-2.0'), ctypes.util.find_library('SDL2')):
        try:
            if n: ctypes.CDLL(n); return n
        except OSError: pass
    return None


def _find_dll(*names):
    """first of names in the C compiler's folder (MSYS2 keeps its DLLs there), PATH or the Vulkan SDK"""
    dirs = [os.path.dirname(c) for c in (shutil.which('gcc'), shutil.which('clang')) if c]
    dirs += os.environ.get('PATH', '').split(os.pathsep)
    if os.environ.get('VULKAN_SDK'): dirs.append(os.path.join(os.environ['VULKAN_SDK'], 'Bin'))
    for d in dirs:
        for n in names:
            if d and os.path.isfile(os.path.join(d, n)): return os.path.join(d, n)
    return None


# ---------------------------------------------------------------- 1 verify
def _code_section(f, base, h):
    mu = 0x200
    g = lambda o: struct.unpack_from('<I', h, o)[0]
    f.seek(base + 0x200); exh = f.read(0x800)
    E = base + g(0x1a0) * mu; f.seek(E); eh = f.read(0x200)
    for i in range(10):
        n = eh[i * 16:i * 16 + 8].rstrip(b'\0').decode('ascii', 'replace'); o, s = struct.unpack_from('<II', eh, i * 16 + 8)
        if n == '.code':
            f.seek(E + 0x200 + o); d = f.read(s)
            if len(d) < s: raise PipelineError('the file is truncated (ExeFS .code runs past its end)')
            return cia.blz_decompress(d) if exh[0xd] & 1 else d
    raise PipelineError('ExeFS has no .code section')


def verify(rom, log=print):
    """dict with container, program_id, product, title, ok, problems; raises PipelineError for unreadable files"""
    if not os.path.isfile(rom): raise PipelineError(f'no such file: {rom}')
    r = dict(rom=os.path.abspath(rom), size=os.path.getsize(rom), problems=[])
    try:
        with open(rom, 'rb') as f:
            base, kind = cia.locate_ncch(f); info = cia.ncch_info(f, base)
            r.update(container=kind, program_id=f'{info["program_id"]:016X}', product=info['product'], encrypted=not info['nocrypto'])
            t = SUPPORTED.get(info['program_id'])
            r['title'] = t['name'] if t else None
            if not t: r['problems'].append(f'title {info["program_id"]:016X} ({info["product"]}) is not supported by this recompiler')
            if not info['nocrypto']: r['problems'].append('the NCCH is encrypted: decrypt your own dump first (this tool does not decrypt)')
            if t and info['nocrypto']:
                h = hashlib.sha256(_code_section(f, base, info['header'])).hexdigest()
                r['code_sha256'] = h
                if h != t['code_sha256']:
                    r['problems'].append('the game code differs from the supported version (other revision or modified dump); '
                                         f'expected {t["code_sha256"][:16]}..., got {h[:16]}...')
    except (ValueError, struct.error, IndexError, UnicodeDecodeError) as e:
        raise PipelineError(f'{rom}: unreadable or damaged ({e.__class__.__name__}: {e})')
    r['ok'] = not r['problems']
    log(f'{r["rom"]}: {r.get("container", "?").upper()}, title {r.get("program_id")} {r.get("product")} '
        f'({r["title"] or "unknown"}), {"encrypted" if r.get("encrypted") else "decrypted"}')
    for p in r['problems']: log(f'  problem: {p}')
    if r['ok']: log('  ok: supported title, code hash matches')
    return r


# ---------------------------------------------------------------- 2-5 build
def _run(cmd, log, cwd=None, env=None):
    log('$ ' + ' '.join(f'"{c}"' if ' ' in c else c for c in cmd))
    # Windows: no console window per tool when the launcher runs without one (pythonw, the build kit)
    p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace',
                         creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    for line in p.stdout: log(line.rstrip('\n'))
    if p.wait(): raise PipelineError(f'{os.path.basename(cmd[0])} failed with exit code {p.returncode}')


def _git_rev():
    try: return subprocess.run(['git', '-C', REPO, 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip() or None
    except OSError: return None


def build(rom, home=None, jobs=None, compiler=None, debug=False, force=False, log=print):
    home = home or default_home(); t0 = time.time()
    game = os.path.join(home, 'game'); ex = os.path.join(game, 'extracted'); bd = os.path.join(game, 'build')
    log('[1/5] verify'); v = verify(rom, log)
    if not v['ok']: raise PipelineError('the ROM did not pass verification')
    log('[2/5] extract')
    have = os.path.join(ex, 'code.bin')
    if not force and os.path.exists(have) and os.path.exists(os.path.join(ex, 'romfs.bin')) and \
            hashlib.sha256(open(have, 'rb').read()).hexdigest() == v['code_sha256']:
        log(f'  {ex}: already extracted from this title, kept (--force to redo)')
    else:
        cia.extract(rom, ex, runtime_only=True, log=lambda m: log('  ' + m))
    log('[3/5] recompile')
    _run([sys.executable, os.path.join(REPO, 'recomp', 'arm2c.py'), os.path.join(ex, 'code.bin'), os.path.join(ex, 'exheader.bin'),
          os.path.join(bd, 'gen')], log)
    log('[4/5] compile')
    cfg = ['cmake', '-S', os.path.join(REPO, 'recomp'), '-B', bd, f'-DGEN_DIR={os.path.join(bd, "gen")}',
           f'-DCMAKE_BUILD_TYPE={"RelWithDebInfo" if debug else "Release"}']
    cache = os.path.join(bd, 'CMakeCache.txt')
    if os.path.exists(cache):   # the sources moved (e.g. the AppImage kit mounts at a new path each start): configure afresh
        home_dir = next((l.split('=', 1)[1].strip() for l in open(cache, errors='replace') if l.startswith('CMAKE_HOME_DIRECTORY:')), None)
        if home_dir and os.path.normcase(os.path.realpath(home_dir)) != os.path.normcase(os.path.realpath(os.path.join(REPO, 'recomp'))):
            log(f'  sources moved ({home_dir}): configuring the build folder afresh')
            os.remove(cache); shutil.rmtree(os.path.join(bd, 'CMakeFiles'), ignore_errors=True)
    if KIT:
        k = os.path.join(KIT, 'bin')
        cfg += [f'-DPWR_KIT={os.path.join(KIT, "sysroot")}', f'-DCMAKE_C_COMPILER={os.path.join(k, "cc")}',
                f'-DCMAKE_AR={os.path.join(k, "ar")}', f'-DCMAKE_RANLIB={os.path.join(k, "ranlib")}']
    if shutil.which('ninja') and not os.path.exists(cache): cfg += ['-G', 'Ninja']
    elif os.name == 'nt' and not os.path.exists(os.path.join(bd, 'CMakeCache.txt')):
        raise PipelineError('Ninja not found: on Windows CMake would pick Visual Studio, which cannot build the generated code')
    if compiler: cfg.append(f'-DCMAKE_C_COMPILER={compiler}')
    _run(cfg, log)
    _run(['cmake', '--build', bd, '-j', str(jobs or os.cpu_count() or 4), '--target', 'pwr'], log)
    log('[5/5] install')
    os.makedirs(os.path.join(home, 'bin'), exist_ok=True)
    built = next((p for p in (os.path.join(bd, EXE), os.path.join(bd, 'Release', EXE), os.path.join(bd, 'RelWithDebInfo', EXE)) if os.path.exists(p)), None)
    if not built: raise PipelineError(f'no {EXE} in {bd}')
    shutil.copy2(built, os.path.join(home, 'bin', EXE))
    if KIT:   # translated vertex shaders; SDL2 from the system, the kit's copy only if there is none
        lib = os.path.join(KIT, 'sysroot', 'lib')
        shutil.copy2(os.path.join(lib, 'libshaderc_shared.so.1'), os.path.join(home, 'bin', 'libshaderc_shared.so.1'))
        if not _system_sdl2():
            shutil.copy2(os.path.join(lib, 'libSDL2-2.0.so.0'), os.path.join(home, 'bin', 'libSDL2-2.0.so.0'))
            log('  warning: no SDL2 on this system; using the kit\'s copy, which needs the usual desktop libraries. '
                'If the game does not start, install SDL2 (libsdl2-2.0-0, SDL2 or sdl2).')
    if os.name == 'nt':
        for names in [(n,) for n in WIN_DLLS] + WIN_DLLS_OPTIONAL:
            src = _find_dll(*names)
            if src: shutil.copy2(src, os.path.join(home, 'bin', os.path.basename(src)))
            elif names[0] in WIN_DLLS: raise PipelineError(f'{names[0]} not found (PATH, the compiler folder); pwr.exe needs it')
            else: log(f'  {names[0]} not found: vertex shaders will use the slower GPU interpreter')
    man = dict(title=v['title'], program_id=v['program_id'], code_sha256=v['code_sha256'], rom=v['rom'],
               built=time.strftime('%Y-%m-%d %H:%M:%S'), recompiler=_git_rev(), platform=platform.platform(), build_type='RelWithDebInfo' if debug else 'Release')
    json.dump(man, open(os.path.join(home, 'manifest.json'), 'w'), indent=2)
    create_profile(home, 'default', exist_ok=True)
    log(f'done in {time.time() - t0:.0f} s: {os.path.join(home, "bin", EXE)}')
    return man


# ---------------------------------------------------------------- profiles + launch
def profile_dir(home, name): return os.path.join(home, 'profiles', name)


def list_profiles(home):
    d = os.path.join(home, 'profiles')
    return sorted(n for n in os.listdir(d) if os.path.isdir(os.path.join(d, n))) if os.path.isdir(d) else []


def load_settings(home, name):
    s = dict(DEFAULT_SETTINGS)
    try: s.update(json.load(open(os.path.join(profile_dir(home, name), 'settings.json'))))
    except (OSError, ValueError): pass
    return s


def save_settings(home, name, settings):
    os.makedirs(profile_dir(home, name), exist_ok=True)
    json.dump(settings, open(os.path.join(profile_dir(home, name), 'settings.json'), 'w'), indent=2)


def create_profile(home, name, exist_ok=False):
    if not name or any(c in name for c in '/\\:') or name in ('.', '..'): raise PipelineError(f'bad profile name: {name!r}')
    d = profile_dir(home, name)
    if os.path.isdir(d) and not exist_ok: raise PipelineError(f'profile {name} exists')
    os.makedirs(os.path.join(d, 'save'), exist_ok=True)
    if not os.path.exists(os.path.join(d, 'settings.json')): save_settings(home, name, DEFAULT_SETTINGS)


def delete_profile(home, name):
    d = profile_dir(home, name)
    if not os.path.isdir(d): raise PipelineError(f'no profile {name}')
    shutil.rmtree(d)


def command_line(home, name, extra=()):
    """the pwr command line for a profile: settings -> options"""
    s = load_settings(home, name)
    cmd = [os.path.join(home, 'bin', EXE), f'--scale={int(s["scale"])}', f'--layout={s["layout"]}', f'--auto-gameplay={s["auto_gameplay"]}',
           f'--aspect={s["aspect"]}', f'--filter={s["filter"]}', f'--fsr-sharpness={float(s["fsr_sharpness"]):.2f}',
           f'--resolution={int(s["resolution"])}', f'--aniso={int(s["aniso"])}', f'--show-fps={1 if s["show_fps"] else 0}',
           f'--select-layout={1 if s["select_layout"] else 0}', f'--free-look={1 if s["free_look"] else 0}', f'--draw-distance={int(s["draw_distance"])}']
    if s['fullscreen'] != 'off': cmd.append(f'--fullscreen={s["fullscreen"]}')
    if s['mute']: cmd.append('--mute')
    cmd += s['args'].split() + list(extra)
    cmd.append(os.path.join(home, 'game', 'extracted'))
    return cmd


def launch(home=None, name='default', extra=(), wait=True, log=print):
    home = home or default_home()
    if not os.path.exists(os.path.join(home, 'bin', EXE)): raise PipelineError(f'not built yet: run "pipeline.py build <rom>" (looked in {home})')
    create_profile(home, name, exist_ok=True)
    env = dict(os.environ, PWR_SAVE_DIR=os.path.join(profile_dir(home, name), 'save'),
               PWR_SETTINGS=os.path.join(profile_dir(home, name), 'settings.json'))   # the in-game overlay saves changes here
    cmd = command_line(home, name, extra)
    log('$ ' + ' '.join(cmd))
    if os.name == 'nt' and not wait:   # from the launcher: no console window; the game's messages go to the profile's pwr.log
        out = open(os.path.join(profile_dir(home, name), 'pwr.log'), 'w')
        p = subprocess.Popen(cmd, cwd=profile_dir(home, name), env=env, stdout=out, stderr=subprocess.STDOUT,
                             creationflags=subprocess.CREATE_NO_WINDOW)
        out.close()
        return p
    p = subprocess.Popen(cmd, cwd=profile_dir(home, name), env=env)
    return p.wait() if wait else p


# ---------------------------------------------------------------- command line
def main(argv=None):
    ap = argparse.ArgumentParser(description='Static recompilation pipeline: ROM -> native build -> launch')
    ap.add_argument('--home', default=None, help='install folder')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('check')
    p = sub.add_parser('verify'); p.add_argument('rom')
    p = sub.add_parser('build'); p.add_argument('rom'); p.add_argument('--jobs', type=int); p.add_argument('--compiler')
    p.add_argument('--debug', action='store_true', help='RelWithDebInfo instead of Release'); p.add_argument('--force', action='store_true', help='extract again')
    p = sub.add_parser('run'); p.add_argument('--profile', default='default'); p.add_argument('extra', nargs=argparse.REMAINDER)
    p = sub.add_parser('profiles'); p.add_argument('action', nargs='?', default='list', choices=['list', 'create', 'delete']); p.add_argument('name', nargs='?')
    a = ap.parse_args(argv); home = a.home or default_home()
    try:
        if a.cmd == 'check': return 0 if all(r[1] for r in check() if 'optional' not in r[0]) else 1
        if a.cmd == 'verify': return 0 if verify(a.rom)['ok'] else 1
        if a.cmd == 'build': build(a.rom, home, a.jobs, a.compiler, a.debug, a.force); return 0
        if a.cmd == 'run': return launch(home, a.profile, [x for x in a.extra if x != '--'])
        if a.cmd == 'profiles':
            if a.action == 'list': print('\n'.join(list_profiles(home)) or '(none)')
            elif a.action == 'create': create_profile(home, a.name)
            else: delete_profile(home, a.name)
            return 0
    except PipelineError as e:
        print(f'error: {e}', file=sys.stderr); return 1


if __name__ == '__main__':
    sys.exit(main())
