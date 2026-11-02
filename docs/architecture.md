# Woohoo! Flight School: architecture

Woohoo! Flight School turns a legally owned retail dump of *Pilotwings Resort (USA)* (3DS) into a native x86-64 program. It does
not emulate the game at run time. The pipeline translates the game's ARM11 machine code into C once, on your machine,
compiles that C together with a hand-written runtime, and runs the result. The repository contains only the recompiler,
the runtime and the tools. No game code and no game data are part of it, and none are ever produced into it.

```
 your dump (.cia / .3ds / .cci / .cxi)
        |
        v
 [1 verify]    tools/pipeline.py      container, title ID, encryption, SHA-256 of the game code
        |
 [2 extract]   tools/cia.py           -> <install>/game/extracted/{code.bin, exheader.bin, romfs.bin}
        |
 [3 recompile] recomp/arm2c.py        -> <install>/game/build/gen/*.c  (every ARM/Thumb function as a C function)
        |       (tools/funcs.py finds the functions)
        |
 [4 compile]   CMake + GCC/Clang      generated C + recomp/runtime/*.c + shaders -> pwr
        |
 [5 install]   <install>/bin/pwr, manifest.json, profiles/
        |
        v
 launch        launcher/pwr_launcher.py or "pipeline.py run": profile settings -> command line, save folder -> PWR_SAVE_DIR
```

The command-line pipeline and the GUI launcher run the same code (`tools/pipeline.py`).

## 1. ROM ingestion and validation

| Step | Where | What |
|---|---|---|
| File picker / path | launcher section 1, or `pipeline.py verify <rom>` / `build <rom>` | Any of `.cia`, `.3ds`/`.cci`, `.cxi`. The last ROM and install folder are remembered (`~/.config/pwr-port/launcher.json`). |
| Container | `cia.locate_ncch()` | CIA: content 0 after the certificate, ticket and TMD blocks. CCI/NCSD: partition 0. CXI: an NCCH at offset 0. |
| Header | `cia.ncch_info()` | `NCCH` magic, program ID, product code, the NoCrypto flag. Encrypted content is refused, never guessed at. Decrypting your dump is your job, with your own console's keys. |
| Title | `pipeline.SUPPORTED` | Program ID `0004000000031C00` (`CTR-P-AWAE`). |
| Code integrity | `pipeline.verify()` | The ExeFS `.code` is BLZ-decompressed in memory and hashed. It must equal the SHA-256 the recompiler was written against (`5c8d6c26...`). The function seeds, hooks (`arm2c.HOOKS`) and the runtime's knowledge of the game's addresses only fit that exact binary; another revision or a modified dump is reported, not built. |
| Damaged files | `pipeline.verify()` | Truncated or unreadable dumps give an error, not a crash. |

## 2. Static recompilation and build

**Function discovery** (`tools/funcs.py`) walks the code from the entry point. It follows calls, branches, jump
tables, ARM-to-Thumb stubs (`add ip, pc, #n; bx ip`), pointers in data and literal pools, and the RVCT `.init_array`.
Run-time finds can be added to `recomp/extra_seeds.txt`.

**Translation** (`recomp/arm2c.py`) emits `void f_<addr>(CPU *c)` per function:
- intra-function branches become `goto`;
- calls become C calls;
- indirect calls go through a sorted address table (`rt_call_indirect`);
- unsupported encodings become `rt_unimpl` traps, counted at generation time.

**Hooks** run host code at chosen guest addresses, for example the game's debug printf and its GX command-list enqueue.

**Build** (`recomp/CMakeLists.txt`):
- The generated C is compiled with `-O2`.
- GLSL shaders are compiled to SPIR-V by `glslc` and embedded.
- Everything links into `pwr` with SDL2 and Vulkan.
- `pipeline.py` uses Ninja when present, Release by default (`--debug` for RelWithDebInfo).
- Compilers: GCC or Clang. Plain MSVC cannot build the generated code, which uses GCC/Clang builtins.
- Windows: MinGW-w64 (MSYS2 MINGW64 natively, or `recomp/cmake/mingw-w64-x86_64.cmake` to cross-compile from Linux).
  The Windows branches of the runtime:
  - guest memory is 4 GiB reserved with pages committed on first touch (a vectored exception handler), and
    `rt_mem_commit` before file I/O into it, which does not fault;
  - `timeBeginPeriod(1)`, so sleeps are not rounded to the 15.6 ms tick;
  - `rt_replace_file` (MoveFileEx), as `rename()` does not replace an existing file;
  - libshaderc via LoadLibrary;
  - a manifest (`runtime/win/`) with the UTF-8 code page and per-monitor DPI awareness.
  `pipeline.py` copies SDL2.dll, libwinpthread-1.dll and libshaderc_shared.dll next to `pwr.exe`. `pwr` reads `$PWR_SETTINGS` (the profile's
  settings.json) at start for its options, before the command line, so a plain script can start it with a profile's settings. Tested by running
  the cross-built exe under Wine, and by the user on a real Windows PC (2026-11-01: works).
- Windows build kit (`tools/make_windows_kit.py`): the MSYS2 MINGW64 packages the build needs (GCC, CMake, Ninja, SDL2,
  Vulkan headers and loader, shaderc, Python with Tk; dependencies resolved from the repository database, checked
  against its SHA-256 sums, documentation and test suites removed), this repository's files and a `.bat` that starts the
  launcher with them first on PATH. No game material. `THIRD-PARTY.txt` lists each package's licence and source.
- Linux build kit (`tools/make_linux_kit.py`): an AppImage with a standalone Python with Tk (python-build-standalone),
  Zig as the C compiler (`zig cc`, Clang; its C libraries for other systems removed), CMake, Ninja, and from LunarG's
  Vulkan SDK `glslc`, `libshaderc` and the Vulkan loader, plus Ubuntu 22.04's SDL2 headers and library. Every download
  is pinned by SHA-256 and nothing downloaded is run while the kit is assembled (squashfs + the AppImage runtime). With
  `$PWR_KIT` set (AppRun does), `pipeline.py` builds with the kit's compiler and sysroot (`-DPWR_KIT`), links `pwr` with
  rpath `$ORIGIN` and copies `libshaderc` next to it; at run time `pwr` uses the system's SDL2 and Vulkan loader (the
  kit's SDL2 is copied only when the system has none). The AppImage mounts at a new path each start, so the pipeline
  configures the build folder afresh when the sources moved. Needs glibc 2.29 or newer. `AppImage --cli <args>` runs
  `pipeline.py`.
- GitHub Actions (`.github/workflows/`): `build-check.yml` compiles the runtime and shaders without a game
  (`-DPWR_RUNTIME_ONLY=ON`) on Linux and on Windows/MSYS2 for every push; `build-kits.yml` assembles both kits for every
  published release, tests them (Windows runner; Ubuntu 22.04 container: the AppImage's pipeline check, the runtime
  compiled with the kit, a program linked against SDL2/Vulkan/libshaderc and run) and attaches them to the release. No
  workflow ever sees a dump or builds the game.

## 3. Runtime

`recomp/runtime/` is written for this project:

| File | Role |
|---|---|
| `runtime.c`, `cpu.h` | Guest memory (4 GiB reservation), CPU state, flags, VFP, indirect-call dispatch. |
| `kernel.c` | HLE 3DS kernel: threads on one run token with 3DS priorities, synchronization objects, address arbiters, timers, SVCs. |
| `services.c`, `fs.c`, `mii.c` | IPC services: `srv`, `APT`, `fs` (RomFS from `romfs.bin`, saves under `PWR_SAVE_DIR`), `cfg`, `hid`, `ndm`, a default Mii database. |
| `gsp.c` | GPU service: GX command queue and interrupts, command-list snapshots, display transfer, memory fill. |
| `pica.c`, `pica_sw.c` | PICA200 command processor. The software rasterizer is the reference. |
| `pica_vk.c`, `pica_vsgen.c`, `shaders/pica.*` | Vulkan PICA renderer: guest vertex shaders translated to GLSL, TEV, fragment lighting, fog and stencil in shaders. |
| `dsp.c` | DSP HLE: source decoding (PCM, DSP-ADPCM), three intermediate mixes, SDL output steered by the sound card's clock. |
| `input.c` | Keyboard, mouse (touch) and gamepad into the HID shared memory. |
| `video.c`, `shaders/present.*`, `shaders/fsr*` | Window, presentation, layouts, filters, settings overlay. |

Guest memory stays authoritative. Render targets are written back after every command list, because the game reads
them (display transfer, render-to-texture). Debugging switches (`PWR_*` environment variables) are described in the source files that read them.

## 4. Launcher, profiles and assets

- **Assets.** The native binary reads the game's data at run time from `<install>/game/extracted/`: `code.bin`
  (loaded at 0x100000), `exheader.bin`, and `romfs.bin` (raw RomFS image, read through the `fs` service). Textures,
  audio and video are not converted ahead of time; the runtime decodes them the way the console does.
  `tools/extract_all.sh` produces PNG/WAV/OBJ/JSON conversions for analysis only.
- **Profiles.** `<install>/profiles/<name>/` holds:
  - `save/` (the game's save data and extdata, passed as `PWR_SAVE_DIR`);
  - `settings.json` (window scale, layout, filters, 3D resolution, anisotropic filtering, audio, extra arguments).

  The launcher edits settings; `pipeline.command_line()` turns them into `pwr` options. The in-game overlay writes its
  changes back through `PWR_SETTINGS`, keeping keys it does not know.
- **Launch.** The game runs with the profile folder as its working directory, so logs and debug dumps land there.
  The launcher shows when it exits.

## 5. Window management and screen layouts

`video.c` uploads the two decoded LCD framebuffers (400x240 top, 320x240 bottom) as textures. It draws them into a
window-sized image and copies that image to the swapchain. A copy rather than a render into the swapchain keeps
software swapchains working.

- **Display modes.** Windowed, borderless fullscreen (desktop resolution) or exclusive fullscreen. F11 or Alt+Enter
  toggles. Aspect modes: `keep` (letterbox), `integer` (largest whole-number scale), `stretch`.
- **Layouts** (F9 cycles):
  - `stacked`: top over bottom;
  - `side`: side by side;
  - `large`: big top, small bottom at its lower right;
  - `top` / `bottom`: one screen;
  - `auto`: driven by a state machine.
- **Auto layout state machine.**
  - Starts in MENU (stacked, bottom screen fully visible and touchable).
  - Goes to GAMEPLAY (`auto_gameplay`: `large` = bottom collapsed to a small view, or `top` = hidden) once the circle
    pad has been held for 1.2 s (the player is flying).
  - Returns to MENU on Start/Select (pause menu), a click on the bottom screen, or 8 s without steering (landing,
    results).
  - Works best in fullscreen; in a window the inactive layout is letterboxed.
- **Touch.** Mouse input is mapped through the bottom screen's current rectangle, whatever the layout.

## 6. Settings overlay and graphical enhancements

**Overlay.** F1 opens it, drawn with an embedded font at 1x-4x (the window's scale, so text stays sharp). Arrows/D-pad pick and change items, Escape/B/F1 closes.
While it is open the game gets no input. Every item applies immediately and is saved to the profile on close.

| Enhancement | Implementation |
|---|---|
| Internal resolution 1x-4x | `pica_vk.c` renders every target N times larger (viewport, scissor, render area scaled). Guest uploads replicate pixels; write-back to guest memory goes through a linear GPU downscale, so the game still reads its own resolution. At DisplayTransfer the high-resolution display target is copied into a per-framebuffer scan-out image. `video.c` un-rotates it and presents it as long as nothing else wrote that framebuffer. Rendering above the window size and filtering down acts as supersampling anti-aliasing. |
| Anisotropic filtering up to 16x | Sampler setting in `pica_vk.c`, clamped to the device limit, linear-filtered textures only. |
| FSR 1 | AMD FidelityFX Super Resolution 1.0. EASU upscales each screen into an image of its output size, then RCAS sharpens it into the window (`shaders/fsr_easu.frag`, `fsr_rcas.frag`, AMD's unmodified headers in `shaders/fsr/`, MIT). Sharpness 0 (most) to 2 stops. |
| FXAA | Luma-edge anti-aliasing in source-texel space before the upscale (`present.frag`). |
| Other scaling filters | `nearest`, `linear`, `sharp` (sharp bilinear: crisp pixels without shimmering at non-integer scales). |
| Frame counter | Game frames per second in the corner. |
| Draw distance 1x-4x | `drawdist.c`: arm2c hooks the game's "read a REAL setting by name" methods (`f_0019b50c`, `f_0019b634`) and scales the art-tweak fade-in distances of information points, rings and labels as they are read; the scene culling frustum's far distance (a literal read from guest memory) is scaled after code.bin is loaded. |
| Free look (off by default) | `freelook.c`: arm2c hooks the game's chase camera updates (plane `f_00149aa4`, rocket belt `f_001e34f0`, hang glider `f_001ffb8c`) before and after they run. After it, the camera's world matrices are rotated around the vehicle by the right stick's angle (eased, kept above the vehicle's height); before the next update the game's own matrices are restored, so its smoothing and collision never see the change, while culling and rendering use the orbited camera. |

**Not implemented:**
- MSAA: PICA render targets are also read back by the game; supersampling through the internal resolution is used
  instead.
- SMAA.

## 7. Clean-room and licensing policy

- **No game material in the repository.** `.gitignore` keeps dumps, `extracted/`, every build tree (the generated C is
  derived from the game's code) and saves out. The generated C exists only in a user's install folder, built from
  their own dump.
- **The runtime is written for this project** from observed behaviour of the game, public hardware documentation
  (3dbrew) and experiments on this machine.
- **Project licence: GPL-3.0-or-later** ([`LICENSE`](../LICENSE)). It covers the project's own files: the translator,
  runtime, tools and launcher. It does not cover the game, its data or the C code generated from it, which remain the
  property of their owners and are never distributed.
- **Reference emulators were read, not copied.** Azahar (GPL-2.0-or-later) and Zakuro (MIT) were read for hardware
  behaviour; no code from either is included so far. Both licences allow borrowing into this project; anything borrowed
  is credited in the file and listed below.
- **Third-party code and data included as-is, with their licences:**
  - AMD FidelityFX FSR 1 headers (MIT, `recomp/runtime/shaders/fsr/`);
  - the overlay font, rendered from DejaVu Sans Mono (Bitstream Vera licence; notice in `font_ui.h`, generator
    `tools/gen_font.py`).
- **No decryption keys and no decryption.** Encrypted dumps are rejected.
