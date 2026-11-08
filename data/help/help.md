This is the text of FujiNet Go MSX's built-in Help (Help ▸ FujiNet Go MSX Help,
or Ctrl+Shift+H; ⌘? on a Mac). The app shows each part only to the frontend it
is about; here you see them all. Every topic below is one page in the app.

# Getting started

FujiNet Go MSX is an MSX, MSX2 or MSX2+ with a FujiNet built in. The machine is
openMSX's, booting C-BIOS, so no manufacturer firmware is needed. The FujiNet
runs inside the app, and openMSX reaches it through the FujiNet cartridge, so
disks, cassettes and the network are there from the first start.

1. **Start the app.** The machine boots C-BIOS. Type on your keyboard: it is the
   MSX's keyboard. See [Controllers and keys](#controllers-and-keys).
2. **Pick a machine.** The Machine menu has MSX, MSX2 and MSX2+. See
   [Machines](#machines).
3. **Open FujiNet's CONFIG.** In C-BIOS, press <kbd>C</kbd> at the start screen
   for the FujiNet client; it browses hosts and the SD folder. See
   [FujiNet](#fujinet).
4. **Or bring your own files.** **Import Disk, Cassette or Cartridge…** copies a
   `.dsk` or `.cas` image to FujiNet's SD folder and a `.rom` to the cartridge
   folder. See [Disks, cassettes and cartridges](#disks-cassettes-and-cartridges).
5. **Connect to a different FujiNet.** The FujiNet settings can dial another
   instance. See [An external FujiNet](#an-external-fujinet).

## This Help

The list on the left has a topic for each part of the app; the search box
above it finds every section that has all the words you type. A **Help**
button in the FujiNet settings, the Debugger and the FujiNet console log opens
the topic for what it shows, and <kbd>Ctrl+Shift+H</kbd> does the same for the
window you are in.

<!-- only: gnome kde windows -->

In the FujiNet settings, the Debugger and the console log, <kbd>F1</kbd> does
too. In the main window <kbd>F1</kbd> is an MSX function key, so it goes to the
machine.

<!-- end -->

The same text is on GitHub, as
[data/help/help.md](https://github.com/FujiNetWIFI/fujinet-go-msx-desktop/blob/main/data/help/help.md).

<!-- only: macos -->

On a Mac, the Help menu's search field also finds Help topics.

<!-- end -->

<!-- context: main -->
# The window

The machine's picture fills the window. The menus have:

- **MSX**, **MSX2** and **MSX2+**, and **Reset**: see [Machines](#machines).
- **Import Disk, Cassette or Cartridge…**: see
  [Disks, cassettes and cartridges](#disks-cassettes-and-cartridges).
- **FujiNet Preferences…**, **FujiNet Configuration…** and **FujiNet Console
  Log…**: see [FujiNet](#fujinet).
- **Debugger** (<kbd>F12</kbd>), **Help** and **About**.

<!-- only: gnome -->

On GNOME, everything is in the main menu (the button at the right of the
header bar).

<!-- end -->

<!-- only: kde -->

The menu bar has Machine, Media, FujiNet and Help. <kbd>F11</kbd> toggles full
screen.

<!-- end -->

<!-- only: windows -->

The menu bar has Machine, Media, FujiNet, View and Help. The FujiNet menu has
**Connection…**, which takes the place of the FujiNet preferences: see
[Connection settings](#connection-settings). View has Fullscreen
(<kbd>F11</kbd>) and Debugger.

<!-- end -->

<!-- only: macos -->

On a Mac, the app menu has Settings and Quit, and the Machine, Media, FujiNet
and View menus have the rest.

<!-- end -->

## The picture

The window remembers its size. <kbd>F11</kbd> toggles full screen.

# Controllers and keys

The keyboard is the MSX's keyboard: a key you press types on the MSX, translated
by openMSX as it would be on a real one. There are no hand controllers on
screen; a gamepad plays the joystick ports.

## The keyboard

Everything you type goes to the machine, except the keys the app keeps for
itself:

| Shortcut | Does |
|---|---|
| <kbd>Ctrl+Shift+H</kbd> | Help |
| <kbd>F11</kbd> | Fullscreen |
| <kbd>F12</kbd> | Debugger |

<!-- only: linux -->

Key combinations with the Super (Windows) key are the desktop's.

<!-- end -->

## Gamepads

Gamepads are picked up as they are plugged in, and can come and go at any
time. openMSX binds them to the two MSX general-purpose joystick ports itself,
through its own `msxjoystick1` and `msxjoystick2`, digitally.

# Machines

The Machine menu picks what the app emulates, and the choice is remembered:

- **MSX**: the first-generation MSX, with the TMS9918A video chip.
- **MSX2**: with the V9938 video chip. This is the default.
- **MSX2+**: with the V9958 video chip.

All three boot **C-BIOS**, the free MSX BIOS, bundled with the app. **Reset**
restarts the machine as its reset button would. Choosing another machine
restarts the session on it.

# BIOS and ROMs

**C-BIOS is bundled**: it is freely redistributable, so MSX, MSX2 and MSX2+ boot
with no manufacturer firmware, in every build. MSX turboR and the ROM sets of
real machines are copyrighted firmware and are never shipped.

# Disks, cassettes and cartridges

Files come in two ways: FujiNet serves disks and cassettes, and openMSX loads
cartridges.

## Import Disk, Cassette or Cartridge

**Import Disk, Cassette or Cartridge…** copies a file where it is used:

- A `.dsk` (a disk image) or `.cas` (a cassette image) goes to FujiNet's SD
  folder, to be mounted from FujiNet's web page or CONFIG.
- A `.rom` (a cartridge) goes to the app's cartridge folder; cartridges are
  openMSX's business, not FujiNet's.

Other file types are refused.

# FujiNet

The FujiNet is FujiNet's own firmware, built as a shared library and run
inside the app, and the emulator joins it over openMSX's FujiNet cartridge
device, on a local socket (port 65505). The direction is unusual: FujiNet
listens and the emulator connects out to it, retrying each second, so the app
starts FujiNet first and a fresh boot finds it waiting.

## CONFIG

In C-BIOS, press <kbd>C</kbd> at the start screen to open FujiNet's CONFIG
client. It browses hosts: the SD folder and servers on the network. Mount a disk
or a cassette and boot it.

## The FujiNet menu

- **FujiNet Preferences…** sets how the app finds FujiNet: see
  [FujiNet settings](#fujinet-settings).
- **FujiNet Configuration…** shows FujiNet's own web page, which mounts disks and
  cassettes, joins networks and sets up hosts. It is on port 64003 on this
  computer only.
- **FujiNet Console Log…** shows what FujiNet prints. See
  [The console log](#the-console-log).

<!-- context: fujilog -->
## The console log

The FujiNet Console Log window shows FujiNet's own messages as they come,
following the end while you are at the bottom.

## An external FujiNet

**Use external FujiNet** skips the built-in FujiNet and connects to another
instance already running, at the **Address:port** you give, for example
`127.0.0.1:65505`; use that instance's `[BOIP]` listen port (65505 by default).
FujiNet Configuration and the console log belong to the built-in FujiNet, so they
say the FujiNet is external; use the other instance's own.

## Behind the scenes

- FujiNet's BoIP port is 65505 and its web UI 64003, on this computer only. A
  standalone `fujinet-pc-msx` holding 65505 makes the link loop between
  connecting and disconnecting; stop it, or point the app at it.
- FujiNet's settings and its SD folder are in the app's `fujinet` folder.

# Preferences

The only preferences are FujiNet's. They apply by restarting the session when
the window closes.

<!-- only: macos -->

On a Mac this window is called **Settings** (⌘,).

<!-- end -->

<!-- context: prefs/FujiNet -->
## FujiNet settings

- **Use external FujiNet**: skip the built-in FujiNet and connect to another
  instance (setting `fujinet_external`).
- **Address:port**: where to connect, for example `127.0.0.1:65505` (see
  [An external FujiNet](#an-external-fujinet); setting
  `fujinet_external_addr`).

<!-- context: prefs/Connection -->
## Connection settings

On Windows, **FujiNet ▸ Connection…** has the same two settings: **Use external
FujiNet** and the **Address:port** (see
[An external FujiNet](#an-external-fujinet)).

## The settings file

The settings file is `settings.ini` (see [Files and folders](#files-and-folders)).
Each setting above names its key. The others are `machine` (the machine's id) and
`window_width` and `window_height` (the window's size).

# Debugger

The Debugger (<kbd>F12</kbd>) stops the machine and shows what it is doing, over
openMSX's own debug interface. It has seven tabs; closing it lets the machine go
on as it was. Opening it stops the machine.

| Button | Does |
|---|---|
| Run or Stop (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Step (<kbd>F7</kbd>) | One instruction |
| Step Over (<kbd>F8</kbd>) | Over a call |
| Step Out (<kbd>Shift+F8</kbd>) | On until the routine returns |
| Frame (<kbd>Shift+F7</kbd>) | Runs to the next VDP interrupt |
| Run to Cursor | On to the selected line |
| Load Symbols | Reads a symbol file |
| Help | Opens the topic of the tab that shows |

<!-- context: debugger/console -->
## Console

The Console takes short commands, or any of openMSX's own Tcl commands, with
history and completion on <kbd>Tab</kbd>. Addresses are `$C000`, `0xC000`,
`C000h`, decimal or a label. A breakpoint's condition is a Tcl expression, for
example `[reg A] == 5`. `help` lists the commands; anything else goes to openMSX's
Tcl console (`debug list`, `reg A 5`, `slotmap`, and so on).

| Command | Does |
|---|---|
| `step`, `over`, `out` | One instruction; over a call; to the return |
| `run`, `stop` | Go on or break |
| `frame` | Runs to the next VDP interrupt |
| `runto` | Runs to an address |
| `break` | An execute breakpoint |
| `bpr`, `bpw` | Memory reads or writes |
| `bpin`, `bpout` | I/O port reads or writes |
| `delete`, `enable`, `clear` | Manage breakpoints (also `disable`, `breaks`) |
| `regs` | The CPU registers |
| `mem`, `poke` | Dumps memory; writes bytes |
| `disasm` | Disassembles |
| `label`, `labels` | Name an address; load labels |

<!-- context: debugger/cpu -->
## CPU and memory

The Z80's registers and flags, editable while stopped, and a hex dump of memory.

<!-- context: debugger/disassembly -->
## Disassembly

The Z80 disassembled at the PC, with labels from the BIOS table or a symbol file.
Breakpoints show in the gutter. **Follow PC** keeps the current line in view.

<!-- context: debugger/vdp -->
## VDP

The video chip, decoded: the TMS9918A (MSX), the V9938 (MSX2) or the V9958
(MSX2+). Its modes, the table addresses, the palette RAM, sprite mode 2 and the
V9938 and V9958 command engine.

<!-- context: debugger/breakpoints -->
## Breakpoints

Execute breakpoints, memory read and write watchpoints and I/O port watchpoints,
each with a Tcl condition, a hit count, and enable or disable. **Add** makes one.

<!-- context: debugger/io -->
## Slots and I/O

The slot selection and map, the PSG and the memory mapper.

<!-- context: debugger/fujinet -->
## FujiNet packets

Every FujiBus packet between the MSX and FujiNet, both ways, with hex, recorded
from the first boot whether or not the debugger is open. **Clear** empties the
list.

# Files and folders

The app keeps its settings in one file, and everything else in its data
folder:

<!-- only: linux -->

On Linux, the settings are `~/.config/fujinet-go-msx/settings.ini` and
the data is in `~/.local/share/fujinet-go-msx` (or under
`$XDG_CONFIG_HOME` and `$XDG_DATA_HOME`). The GNOME and KDE apps share them.
As a Flatpak, they are under `~/.var/app/`, in the app's own folder.

<!-- end -->

<!-- only: windows -->

On Windows, the settings are `%APPDATA%\fujinet-go-msx\settings.ini` and
the data is in `%LOCALAPPDATA%\fujinet-go-msx`.

<!-- end -->

<!-- only: macos -->

On a Mac, the settings are `~/.config/fujinet-go-msx/settings.ini` and
the data is in `~/.local/share/fujinet-go-msx`.

<!-- end -->

In the data folder:

| Folder | Has |
|---|---|
| `carts` | imported cartridges |
| `fujinet` | FujiNet's settings, its SD folder (`SD`) and its data |

# Troubleshooting

- **Unsupported media**: only `.dsk` and `.cas` go to FujiNet, and `.rom` is a
  cartridge.
- **CONFIG shows no host list, or the link loops**: a stray standalone
  `fujinet-pc-msx` is holding port 65505. Stop it, or point the app at it with
  **Use external FujiNet**.
- **Keys don't reach the machine**: a dialog or another window is in front, or
  the key is one the app keeps (see
  [Controllers and keys](#controllers-and-keys)).
- **A gamepad does nothing**: it drives the joystick ports, so the game must
  read them.

# About and licences

FujiNet Go MSX is free software under the GNU General Public License, version 3
or later. Copyright © 2026 Thomas Cherryhomes.

- **openMSX** (the MSX emulator, GPL-2.0-or-later) is the machine.
- **C-BIOS** (the free MSX BIOS, BSD-licensed) is the system ROM.
- **The FujiNet firmware** (GPL-3.0-or-later) runs inside the app.
- "MSX" is a trademark of its owner, used to name the machine emulated; this
  project is not affiliated with it.

[fujinet.online](https://fujinet.online/) ·
[fujinet-go-msx-desktop](https://github.com/FujiNetWIFI/fujinet-go-msx-desktop)
