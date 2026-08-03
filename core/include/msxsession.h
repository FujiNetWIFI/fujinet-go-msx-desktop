/*
 * msxsession -- toolkit-agnostic desktop session for FujiNet Go MSX.
 *
 * Owns the headless openMSX core (driven on its own thread through
 * core/openmsx/msx_host.hh -- openMSX paces itself; unlike the sibling
 * targets there is no phase-locked emulator loop here to drive, see
 * msxsession_notify_vsync below), the shared settings store (M4), and the
 * media path layout (M4). Frontends (GTK, Qt, AppKit, Win32) drive this API
 * and only do windowing, painting, and event translation.
 *
 * Note the transport direction: like the CoCo port, *FujiNet listens* and
 * the emulator connects out to it (openMSX's FujiNet cartridge device,
 * src/serial/FujiNet.cc, already retries its outbound connect once a second
 * on failure). msxsession_start therefore starts FujiNet first (M3).
 *
 * Audio and gamepads are NOT owned here: openMSX plays through its own SDL2
 * Mixer straight to the real audio device, and binds real joysticks to its
 * msxjoystick1/2 plugs itself (Tcl commands issued from msxsession_start,
 * see COMPLIANCE.md / TODO for why this diverges from the sibling repos'
 * shared audio_sdl.c/gamepad_sdl.c layer). This app links no SDL3.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MSXSESSION_H
#define MSXSESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frame geometry IS fixed, unlike the sibling repos: openMSX's own
 * FrameSource::getLinePtr640_480() scales every source resolution (MSX1's
 * 256x192 through MSX2's interlaced 512x424) to exactly this size before
 * msxsession ever sees it, so there is no variable-geometry handling to do
 * on this side either. */
#define MSXSESSION_FB_WIDTH  640
#define MSXSESSION_FB_HEIGHT 480

/* FujiBus-over-TCP loopback port -- FujiNet listens here and openMSX's
 * FujiNet cartridge device connects out -- fixed to match the family
 * convention of a dedicated high port per target (ADAM 65214, Apple II
 * 64001, CoCo 64002/65504), so a standalone fujinet-pc-msx or another
 * FujiNet Go target on the same machine never collides with this one. This
 * value is also what tools/openmsx/patches/patch-staged-tree.py compiles
 * into FUJINET_DEFAULT_PORT -- the two must agree. */
#define MSXSESSION_BOIP_PORT  65505
#define MSXSESSION_WEBUI_PORT 64003

/* The MSX has two general-purpose joystick ports. */
#define MSXSESSION_JOYSTICK_PORTS 2

typedef struct msxsession msxsession;
typedef struct msxdebug msxdebug;

/* All members optional (NULL = default).
 *  config_dir: default $XDG_CONFIG_HOME/fujinet-go-msx
 *  data_dir:   default $XDG_DATA_HOME/fujinet-go-msx
 *  openmsx_share: the openMSX runtime share tree (init.tcl, scripts/,
 *              machines/ incl. C-BIOS, extensions/ incl. FujiNet.xml +
 *              fujinet-config.rom). Default searches $MSX_OPENMSX_SHARE,
 *              the executable's directory, the install datadir, then
 *              tools/openmsx/work/out/share (a dev build's output).
 *  fujinet_lib: path to libfujinet.so/.dylib/.dll; default searches
 *              $FUJINET_LIB, the executable's directory, the install libdir,
 *              then tools/fujinet/work/out. "" disables. (M3)
 *  fujinet_runtime_src: directory holding the pristine fnconfig.ini +
 *              data/ + SD/ used to provision the user's runtime tree on
 *              first start. (M3) */
typedef struct {
    const char *config_dir;
    const char *data_dir;
    const char *openmsx_share;
    const char *fujinet_lib;
    const char *fujinet_runtime_src;
} msxsession_paths;

/* Creates the session and resolves the openMSX share tree. Does not start
 * emulation. Returns NULL only on out-of-memory / unusable directories. */
msxsession *msxsession_new(const msxsession_paths *paths);
void msxsession_free(msxsession *s);

/* ---- settings (shared INI; one store for all frontends/platforms; M4) --- */
int         msxsession_get_int(msxsession *s, const char *key, int def);
void        msxsession_set_int(msxsession *s, const char *key, int value);
const char *msxsession_get_str(msxsession *s, const char *key,
                               const char *def);
void        msxsession_set_str(msxsession *s, const char *key,
                               const char *value);
void        msxsession_settings_flush(msxsession *s);

/* ---- machine selection ---------------------------------------------------
 * openMSX machine ids, not an enum: C-BIOS ships MSX/MSX2/MSX2+ under fixed
 * ids, and turboR / real-machine profiles the user imports (M4) are
 * arbitrary strings openMSX's own machine config resolves. The four C-BIOS
 * ids below are the ones this build always has; msxsession_machine_name()
 * exists once M4 adds a profile list a frontend can enumerate. */
#define MSXSESSION_MACHINE_MSX    "C-BIOS_MSX1"
#define MSXSESSION_MACHINE_MSX2   "C-BIOS_MSX2"
#define MSXSESSION_MACHINE_MSX2P  "C-BIOS_MSX2+"

/* ---- lifecycle ------------------------------------------------------------
 * Joystick enable joins this in M4 (audio is n/a -- see above). */
typedef struct {
    const char *machine; /* an MSXSESSION_MACHINE_* id, or a profile id (M4) */
    int enable_fujinet;  /* start the in-process FujiNet runtime */
} msxsession_start_opts;

/* Fills opts from the settings store (default: C-BIOS MSX2). */
void msxsession_default_opts(msxsession *s, msxsession_start_opts *opts);

/* Boots openMSX (and, once M3 lands, starts FujiNet first). Blocks until the
 * boot has succeeded or failed, so a bad machine id is reported here rather
 * than producing a black window. Returns 0 on success, -1 with
 * msxsession_last_error() set. */
int  msxsession_start(msxsession *s, const msxsession_start_opts *opts);
void msxsession_stop(msxsession *s);
int  msxsession_is_running(const msxsession *s);
const char *msxsession_last_error(const msxsession *s);

/* ---- video -----------------------------------------------------------------
 * The emulator thread stores the latest frame (published from
 * core/openmsx/msx_host.cc's msxhost_notify_frame -> the internal frame
 * sink below); the UI thread pulls it on its own vsync/frame-clock tick.
 * copy_frame copies the latest MSXSESSION_FB_WIDTH x MSXSESSION_FB_HEIGHT
 * frame into dst (XRGB8888, tightly packed) iff its serial differs from
 * *serial_inout, updates *serial_inout, and returns 1; returns 0 when the
 * frame is unchanged (dst untouched). Pass *serial_inout = 0 to force a
 * copy (e.g. first paint after a window map). dst must hold
 * MSXSESSION_FB_WIDTH * MSXSESSION_FB_HEIGHT uint32_t. */
int  msxsession_copy_frame(msxsession *s, uint32_t *dst,
                           uint64_t *serial_inout);

/* Feed the UI's vsync ticks (frame-clock time, CLOCK_MONOTONIC ns).
 * Advisory only: unlike the sibling targets there is no phase-locked
 * emulator loop to steer with this -- openMSX's own RealTime paces itself,
 * and its 50/60Hz switching means the family's phase-lock design would
 * often lock to the wrong rate anyway (see PORTING.md's CoCo/MSX heads-up).
 * Kept for frontend-code symmetry with the sibling targets and for the
 * MSX_PACE_LOG=1 diagnostic; does not affect emulation timing. */
void msxsession_notify_vsync(msxsession *s, int64_t frame_time_ns);

/* ---- input -----------------------------------------------------------------
 * Frontends translate their toolkit's key events to X11/xkb keysyms (GDK
 * keyvals already are; Qt, AppKit and Win32 map through a small table),
 * then call msxsession_key, so one mapping table serves every frontend. */
void msxsession_key(msxsession *s, int down, uint32_t keysym,
                    uint32_t unicode, int ctrl_down, int shift_down);

/* Translate a desktop key event to the SDL_Keycode openMSX's Keyboard
 * expects. keysym is an X11/xkb keysym, unicode the translated character (0
 * if none), ctrl_down/shift_down the modifier state. Writes the keycode +
 * SDL modifier bits to *keycode_out / *mods_out and returns 1 when the key
 * is forwarded, 0 when it is not.
 *
 * F10, F11 and F12 are reserved for the frontends (menu, fullscreen,
 * debugger) and are never forwarded.
 *
 * Pure function, shared by every frontend and unit-tested on its own. */
int msx_key_from_event(uint32_t keysym, uint32_t unicode, int ctrl_down,
                       int shift_down, unsigned *keycode_out,
                       uint16_t *mods_out);

/* MSX general-purpose joystick port (0 or 1), digital. button is one of the
 * MSXSESSION_JOY_* ids. */
enum {
    MSXSESSION_JOY_UP = 0,
    MSXSESSION_JOY_DOWN,
    MSXSESSION_JOY_LEFT,
    MSXSESSION_JOY_RIGHT,
    MSXSESSION_JOY_TRIG_A,
    MSXSESSION_JOY_TRIG_B,
};
void msxsession_joystick_button(msxsession *s, int port, int button,
                                int pressed);

/* Hard reset (the MSX front-panel reset). */
void msxsession_reset(msxsession *s);

/* ---- FujiNet (M3) ---------------------------------------------------------*/
int         msxsession_fujinet_running(const msxsession *s);
const char *msxsession_fujinet_webui_url(const msxsession *s);
int         msxsession_fujinet_copy_log(msxsession *s, char *dst, int max);

/* ---- media import (M4) ----------------------------------------------------
 * Disk/cartridge images (.dsk .rom .cas) go to the FujiNet SD directory;
 * cartridge ROMs the user wants inserted directly go to the session's own
 * cartridge directory. Returns 0 on success and writes the destination path
 * into dest_out. */
int msxsession_import_media(msxsession *s, const char *src_path,
                            char *dest_out, int dest_sz);
int msxsession_import_rom(msxsession *s, const char *src_path,
                          char *dest_out, int dest_sz);

/* Directory paths (valid for the session's lifetime). */
const char *msxsession_roms_path(const msxsession *s);
const char *msxsession_carts_path(const msxsession *s);
const char *msxsession_config_path(const msxsession *s);
const char *msxsession_data_path(const msxsession *s);
const char *msxsession_sd_path(const msxsession *s);

/* ---- debugger (M6) ---------------------------------------------------------
 * Lazily created; engaging it (breakpoints/pause) drives openMSX's own
 * MSXCPUInterface::doBreak/doStep/doContinue and Tcl debug commands. See
 * msxdebug.h. */
msxdebug *msxsession_debugger(msxsession *s);

#ifdef __cplusplus
}
#endif

#endif /* MSXSESSION_H */
