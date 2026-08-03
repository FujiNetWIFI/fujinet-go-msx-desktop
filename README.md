# FujiNet Go MSX — desktop

A self-contained MSX / MSX2 / MSX2+ with FujiNet built in. One shared C
session core plus four native frontends: GTK4/libadwaita on GNOME, Qt6
Widgets on KDE, AppKit on macOS and Win32 on Windows.

Emulation is [openMSX](https://openmsx.org/) (the `FujiNetWIFI` fork, branch
`feat/fujinet`). FujiNet is not a subprocess: the firmware is built as a
shared library, `dlopen`'d into the process, and joined to the emulator over
its **FujiNet cartridge device** (`src/serial/FujiNet.cc`), carried on
loopback TCP **65505**.

Note the direction, shared with the CoCo port: here **FujiNet listens and the
emulator connects out to it** — openMSX's own connect loop already retries
once a second, so the session starts FujiNet first and a fresh openMSX boot
finds it waiting.

This is the fourth member of the FujiNet Go desktop family;
[`fujinet-go-adam-desktop`](https://github.com/FujiNetWIFI/fujinet-go-adam-desktop)
is the model repository, and its `PORTING.md` is the specification this
follows.

The Android sibling, [`fujinet-go-msx`](https://github.com/FujiNetWIFI/fujinet-go-msx),
already solved the MSX-specific half — openMSX boots C-BIOS with the FujiNet
extension, and video/audio/keyboard/joystick are all live on real hardware.
This repository ports that to the desktop shape the other three follow.

**Status: early scaffold, in active development.** See `TODO` for the current
milestone.

| Frontend | Toolkit | Binary | Status |
|---|---|---|---|
| GNOME | GTK4 + libadwaita (+ WebKitGTK) | `fujinet-go-msx-gnome` | in progress |
| KDE | Qt6 Widgets (+ QtWebEngine) | `fujinet-go-msx-kde` | not started |
| macOS | AppKit (+ WKWebView) | `FujiNet Go MSX.app` | not started |
| Windows | Win32 (GDI + DwmFlush) | `fujinet-go-msx-windows.exe` | not started |

The maintainer develops on Linux without Mac or Windows hardware: those
builds are compiled, tested and packaged on CI's macOS and Windows runners,
and reports from real users on those platforms are very welcome.

## Planned features

- MSX / MSX2 / MSX2+ (and user-supplied turboR ROMs) via openMSX, with C-BIOS
  bundled so the free-BIOS machines boot with zero manufacturer firmware —
  see "MSX system ROMs" below.
- In-process FujiNet: disk/cartridge mounting from the local SD folder, and
  network config through the embedded FujiNet web UI, plus a live
  console-log window.
- Digital joystick support on both MSX general-purpose ports, driven through
  openMSX's own `msxjoystick1`/`2` plugs.
- Import disk/ROM images into the FujiNet SD folder; Import System ROMs… for
  turboR or real-machine firmware this project does not (and cannot) ship.
- Settings/Preferences windows (GNOME, KDE, macOS ⌘,, Windows) for machine
  profile, aspect ratio, smooth scaling, and FujiNet enable.

### Developer debugger (F12)

Breakpoints, pause/step into/over/out, run-to, a Z80 disassembler, memory and
register views, and a decoded VDP pane that understands the TMS9918A (MSX1),
V9938 (MSX2) and V9958 (MSX2+) — modes, table addressing, the palette RAM,
sprite mode 2, and the V9938/V9958 command engine (VRAM-to-VRAM blit unit).
Available in every frontend over the one shared engine; it extends
`fujinet-go-adam-desktop`'s TMS9918A visualizer to the V9938/V9958 — see
`TODO` for the current state.

## Building

```sh
git clone https://github.com/FujiNetWIFI/fujinet-go-msx-desktop
cd fujinet-go-msx-desktop
cmake -B build -G Ninja
cmake --build build
./build/frontends/gnome/fujinet-go-msx-gnome
```

No preparation step: the build provides its own dependencies. A plain
`git clone`, a source tarball with no git metadata, and a flatpak build all
end up with a usable checkout.

Build dependencies: CMake ≥ 3.20, Ninja, a C11/C++23 compiler, Python 3, and
(for openMSX's own build chain) Tcl, SDL2, SDL2_ttf, freetype, libpng and
zlib — declared per-platform in `cmake/StageOpenMSX.cmake` and the flatpak
manifests. The GNOME frontend additionally needs libadwaita ≥ 1.4 / GTK ≥
4.10, and the KDE one Qt6 ≥ 6.4 Widgets. WebKitGTK 6.0 and QtWebEngine are
optional; without them the FujiNet web UI opens in the system browser.

### Useful options

| Option | Default | Meaning |
|---|---|---|
| `FRONTEND` | `all` | `gnome`, `kde`, `macos`, `windows`, `all`, `none`. `all` considers only the host's viable frontends. |
| `WITH_FUJINET` | `ON` | Build and embed the FujiNet runtime. |
| `WITH_WEBVIEW` | `ON` | Embed the FujiNet web UI rather than opening a browser. |
| `OPENMSX_SRC` | — | Build against an out-of-tree openMSX checkout instead of the pinned submodule. |
| `FUJINET_SRC` | — | Likewise for the FujiNet firmware. |
| `OPENMSX_RESTAGE` | `OFF` | Re-stage openMSX — how to pick up uncommitted edits in an `OPENMSX_SRC` checkout. |

Developing against local checkouts:

```sh
cmake -B build -G Ninja \
      -DOPENMSX_SRC=$HOME/Workspace/openMSX \
      -DFUJINET_SRC=$HOME/Workspace/fujinet-pc-msx \
      -DOPENMSX_RESTAGE=ON
```

## MSX system ROMs

**C-BIOS is bundled unconditionally** — it is freely redistributable (see
`COMPLIANCE.md`) — so MSX, MSX2 and MSX2+ boot with no manufacturer firmware
at all, out of the box, in every build this project publishes.

MSX turboR and specific real-machine ROM sets are copyrighted firmware and
are **never** shipped. Supply your own with **Import System ROMs…**, which
copies them into the session's private ROM directory and binds them to a
machine profile at boot.

## Testing

```sh
ctest --test-dir build --output-on-failure
```

## Running it

If openMSX boots but the FujiNet CONFIG screen (`c` from C-BIOS) shows no
host list, or the link loops connect/disconnect, check for a stray standalone
`fujinet-pc-msx` holding 65505 before suspecting the bus:

```sh
ss -tlnp | grep 65505
```

## Licence

GPL-3.0-or-later. See `COMPLIANCE.md` for per-component provenance, the
openMSX licence determination, and what is and is not patched against
openMSX.
