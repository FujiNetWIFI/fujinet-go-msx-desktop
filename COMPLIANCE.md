# Licence and provenance

Per-component origin, licence, and how each enters the build. Written before
the first public build, as the family convention requires.

| Component | Origin | Licence | How it enters the build |
|---|---|---|---|
| This application | `FujiNetWIFI/fujinet-go-msx-desktop` | GPL-3.0-or-later | the repository itself |
| openMSX (emulator core) | `FujiNetWIFI/openMSX`, branch `feat/fujinet` | GPL-2.0-or-later (see below) | pinned checkout `third_party/openmsx`, staged into `core/openmsx-generated` and built with openMSX's own make chain (`cmake/StageOpenMSX.cmake` + `tools/openmsx/build-openmsx-desktop.sh`) |
| FujiNet firmware (RS232 target) | `FujiNetWIFI/fujinet-firmware` | GPL-3.0-or-later | pinned checkout `third_party/fujinet-firmware`, built as a shared library and `dlopen`'d |
| C-BIOS | vendored in openMSX (`Contrib/cbios`) | freely redistributable (see below) | bundled with the app; boots MSX/MSX2/MSX2+ with no manufacturer firmware |
| SDL2 | built by openMSX's own 3rd-party chain | Zlib | openMSX's own video/audio/joystick backend — this app links no SDL3 |
| Tcl | built by openMSX's own 3rd-party chain | TCL/BSD-style | openMSX's scripting engine and debugger command interface |
| freetype | built by openMSX's own 3rd-party chain | FTL or GPL-2.0 | openMSX's on-screen text rendering |
| libpng | built by openMSX's own 3rd-party chain | libpng license | openMSX's screenshot/icon handling |
| libogg / libvorbis / libtheora | built by openMSX's own 3rd-party chain | BSD-style (Xiph.Org) | openMSX's laserdisc support (unused by this app but part of the shared chain) |
| zlib | built by openMSX's own 3rd-party chain | Zlib | compression |
| GLEW | shimmed on some platforms, else built by openMSX's chain | BSD/MIT | openMSX's `SDLGL-PP` renderer, which this app uses headlessly (see "Frame capture" below) |
| mbedTLS 3.6.5 | system or built from the pinned tag | Apache-2.0 | linked into the FujiNet library |
| libssh / libsmb2 / libnfs | pulled in by the FujiNet build | LGPL-2.1 | linked into the FujiNet library |
| expat, cJSON | pulled in by the FujiNet build | MIT | linked into the FujiNet library |

A combined, distributed binary is bound by openMSX's GPL-2.0-or-later and
FujiNet's GPL-3.0 copyleft — the combined work is distributed under
**GPL-3.0-or-later**, the same net effect as the Android sibling
(`fujinet-go-msx`) reaches for the same pairing.

## openMSX's licence: a determination, not a quotation

openMSX's `README` states that source files without their own header are
"licensed under the GNU Public License (GPL), of which you can find a copy in
the file 'GPL.txt'" — and the copy it ships is GPLv2, with no "or later"
clause stated anywhere in the README or `main.cc`. Taken completely literally
this reads as GPL-2.0-only, which would make it **incompatible** with
FujiNet's GPL-3.0-only pairing.

`fujinet-go-msx` (the Android sibling) already faced this and determined
openMSX to be **GPL-2.0-or-later** in practice — the ambiguous README wording,
the absence of any explicit "-only" restriction in the source, and openMSX's
own long-standing distribution alongside GPL-3 components in other projects.
This repository carries the identical determination so the two MSX apps agree
with each other. It is stated here as a determination this project has made,
not as a claim about what openMSX's maintainers have formally declared; anyone
redistributing this software should be aware of the ambiguity in the upstream
licensing text.

## openMSX is used through its own build chain, with narrow patches

Unlike XRoar/AppleWin/adamcore in the sibling repos, openMSX is not staged and
compiled by this project's own CMake — see `fujinet-go-adam-desktop`'s
`PORTING.md` §"New work" (this repo's `TODO`) for why
(570 C++23 translation units, generated config headers, and a Tcl/SDL2/
freetype/libpng/zlib/ogg/vorbis/theora/GLEW dependency chain that openMSX's
own `GNUmakefile` + `build/3rdparty` chain already solves per-platform).
`cmake/StageOpenMSX.cmake` copies the pinned checkout into
`core/openmsx-generated/`, applies the patches below (idempotent, anchored to
exact text, hard-failing on a missing anchor), and drives
`tools/openmsx/build-openmsx-desktop.sh`, which invokes openMSX's own
`GNUmakefile` (`OPENMSX_TARGET_OS=linux|darwin|mingw-w64`). The resulting
objects are archived into `libopenmsx.a` and linked into the session library.

Patches applied to the staged tree, all recorded in
`core/openmsx-generated/.source-info`:

1. **`src/serial/FujiNet.cc`: `FUJINET_DEFAULT_PORT` 1985 → 65505.** Upstream's
   default (and the Android sibling's 1986) would collide with other FujiNet
   Go targets or a standalone `fujinet-pc-msx` running on the same machine;
   65505 is this family's dedicated high port for MSX (alongside ADAM 65214,
   Apple II 64001, CoCo 64002/65504).
2. **`src/video/PostProcessor.cc`: a frame-sink hook in `rotateFrames()`.**
   Desktop frame capture cannot use the Android app's `-Wl,--wrap,SDL_GL_SwapWindow`
   + `glReadPixels` mechanism (`--wrap` is a GNU ld/lld feature absent on
   macOS's linker). Instead this hooks `PostProcessor::rotateFrames()` — the
   same per-frame CPU-side consumer openMSX's own AVI recorder already uses —
   and reuses `getScaledFrame()` (already in that file) to normalise to
   320×240/640×480 without any GL readback.
3. **No touch-keyboard release-delay patch.** The Android sibling ports a
   release-delay into `EventDelay` because its on-screen keyboard injects a
   press+release back-to-back, faster than the MSX matrix scan. A desktop
   physical keyboard produces real down/up pairs with real dwell time, so this
   patch is deliberately **not** carried here.

No frontend source of openMSX's own (SDL-native GUI, ImGui debugger overlay)
is used for presentation; those are only what supplies the GL context this app
captures frames from. Execution control and inspection go through openMSX's
own `MSXCPUInterface` (pause/step/continue) and Tcl `debug` commands
(breakpoints, block reads).

## C-BIOS system ROMs

C-BIOS (© BouKiCHi and the C-BIOS team) is **freely redistributable** — see
`third_party/openmsx/Contrib/README.cbios` in the staged checkout — and is
bundled unconditionally. **MSX, MSX2 and MSX2+ boot with zero manufacturer
firmware**, unlike every other target in this family — `fujinet-go-adam-desktop`'s
`PORTING.md` calls this "the model to imitate" for any target with a free BIOS
available.

**MSX turboR and real-machine ROMs are never bundled.** Manufacturer MSX
firmware is copyrighted and not freely licensed; a user who wants to emulate a
specific real machine or turboR imports their own ROM dump through
`msxsession_import_rom()`, which copies it into the session's private ROM
directory and binds it to a machine profile. `core/tests/no_embedded_roms.py`
asserts their absence from every shipped frontend binary — not just the test
binary, which the Apple II port in this family learned the hard way is not
sufficient.

## Debug symbol tables

The built-in MSX BIOS/BDOS symbol table (`core/debugger/symbols_builtin.c`) —
entry points like `CHGET`, `CHPUT`, `WRTVDP`, `LDIRVM` — is names and
addresses taken from public MSX BIOS disassemblies. Like ADAM's EOS/OS7
tables, this is a table of *facts* and contains no program code.
