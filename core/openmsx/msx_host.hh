/*
 * msx_host -- the desktop host driving openMSX for FujiNet Go MSX.
 *
 * Ported from the Android sibling's app/src/main/cpp/msx_host.cpp, with the
 * Android-specific halves removed:
 *   - No audio ring: openMSX plays through its own SDL2 Mixer, straight to
 *     the real audio device. This app links no SDL3 and owns no audio
 *     backend of its own (see COMPLIANCE.md / TODO).
 *   - No -Wl,--wrap,SDL_GL_SwapWindow + glReadPixels frame capture (a GNU
 *     ld/lld feature absent on macOS). Frames arrive instead through
 *     msxhost_notify_frame(), called from a patch in openMSX's own
 *     PostProcessor::rotateFrames() (tools/openmsx/patches/
 *     patch-staged-tree.py) -- the same per-frame CPU-side hook point
 *     openMSX's own AVI recorder taps, no GL readback needed.
 *   - No JNI/ANativeWindow: the session (core/src/session.c) owns the
 *     frame double-buffer and serial; frontends pull it on their own tick.
 *
 * openMSX still needs a real (headless) GL context to construct its
 * SDLGL-PP renderer -- RendererFactory offers only DUMMY and SDLGL_PP, and
 * DummyRenderer produces no frames -- so SDL_VIDEODRIVER=offscreen (an EGL
 * pbuffer) is used exactly as on Android; nothing is ever drawn to a screen
 * by openMSX itself.
 *
 * Threading: msxhost_core_start() spawns openMSX's own thread (which owns
 * the Reactor and blocks in Reactor::run() until a QuitEvent) and returns
 * once the boot has either succeeded or failed. msxhost_core_stop() posts
 * the QuitEvent and joins. msxhost_inject_key/msxhost_set_joystick_button
 * are safe to call from any thread; they enqueue openMSX Event objects
 * drained on the openMSX thread from inside the frame hook.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

/* Included from both C (core/src/session.c) and C++ (this file's own .cc):
 * plain C headers and an #ifdef-guarded extern "C" block, same pattern as
 * the sibling repos' <target>debug.h headers. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One decoded video frame, XRGB8888, tightly packed, always exactly
 * 640x480 -- openMSX's own FrameSource::getLinePtr640_480() scales any
 * source resolution (MSX1's 256x192 through MSX2's interlaced 512x424) to
 * this fixed size, so no variable-geometry handling is needed on either
 * side of this call. Invoked on the openMSX thread from inside the patched
 * PostProcessor::rotateFrames(). */
typedef void (*MsxFrameSink)(const uint32_t* xrgb8888, int width, int height, void* user);
void msxhost_set_frame_sink(MsxFrameSink sink, void* user);

/* Select the openMSX machine to boot (e.g. "C-BIOS_MSX2", a real-machine id,
 * or a user-imported-ROM profile id). Must be called before
 * msxhost_core_start(). */
void msxhost_select_machine(const char* machine_id);

/* --- core lifecycle ---------------------------------------------------- */
/* Boots openMSX with the selected machine + the FujiNet extension. Blocks
 * until the boot has succeeded or failed (unlike the Android host, which
 * only spawns the thread) so a caller learns about a bad machine id or a
 * renderer failure synchronously. Returns 0 on failure (see
 * msxhost_last_error()), 1 on success. Plain int, not bool: this header is
 * included from plain C (core/src/session.c) as well as C++. */
int msxhost_core_start(void);
void msxhost_core_stop(void);
void msxhost_core_reset(void);
int msxhost_is_running(void);
const char* msxhost_last_error(void);

/* Always 640, 480 -- see MsxFrameSink above. Kept as a function (not a
 * compile-time constant) for parity with the family's per-target
 * *session_get_geometry-shaped contracts, and in case a future MSX mode
 * needs a different fixed output size. */
void msxhost_get_geometry(int* width, int* height);

/* --- input --------------------------------------------------------------
 * Keyboard: an SDL_Keycode (core/src/input_map.c's msx_key_from_event()
 * produces one from a desktop keysym) + the translated character, forwarded
 * to openMSX's Keyboard through the global EventDistributor -> EventDelay
 * path -- the same path desktop openMSX uses for real SDL input. */
void msxhost_inject_key(int down, unsigned keycode, uint32_t character, uint16_t mods);

/* MSX general-purpose joystick port (0 or 1). id is one of the MSXHOST_JOY_*
 * constants below; digital directions are injected as full-throw axis
 * motion (comfortably clearing the binding's dead zone), matching the
 * Android host. */
enum {
    MSXHOST_JOY_UP = 0,
    MSXHOST_JOY_DOWN,
    MSXHOST_JOY_LEFT,
    MSXHOST_JOY_RIGHT,
    MSXHOST_JOY_TRIG_A,
    MSXHOST_JOY_TRIG_B,
};
void msxhost_set_joystick_button(int port, int id, int pressed);

/* --- live machine switching (M4) -----------------------------------------
 * Switches the running machine in place, via openMSX's own
 * Reactor::switchMachine() (the "machine <id>" Tcl command) followed by
 * re-inserting the FujiNet extension (openMSX's own machine switch tears
 * down and rebuilds the whole MSXMotherBoard, including whatever
 * extensions the previous one had -- "ext FujiNet" is not carried over
 * automatically). Queued and executed on the openMSX thread from the frame
 * hook, same as key/joystick input; safe to call from any thread. No-op if
 * openMSX is not currently running (msxhost_select_machine + a fresh
 * msxhost_core_start is the path for that case instead). */
void msxhost_switch_machine(const char* machine_id);

/* --- synchronous Tcl command RPC (M6, the debugger) -----------------------
 * Executes an arbitrary openMSX Tcl command ON the openMSX thread (queued
 * and drained the same way as msxhost_switch_machine, but this call BLOCKS
 * the caller until the command has actually run and returns its result),
 * safe from any thread. This is the one primitive core/debugger/debugger.c
 * is built entirely on top of: pause/step/continue ("debug break"/"step"/
 * "cont" -- themselves calling MSXCPUInterface::doBreak/doStep/doContinue,
 * which per CPUCore.cc's own comment "may only get called by the main
 * emulation thread", exactly why this has to be queued rather than called
 * directly), breakpoints ("debug set_bp"/"remove_bp"/"list_bp", including
 * *conditional* one-shot breakpoints for step-over/step-out -- openMSX has
 * no native step-over, so a one-shot breakpoint at the return address with
 * an SP-floor condition does the same job adamdebug.c's own exec-hook does
 * on the ADAM target), registers (the "reg"/"cpuregs" Tcl procs already in
 * share/scripts/_cpuregs.tcl, which this app already bundles), and memory/
 * VDP snapshot reads ("debug read_block <debuggable> ...").
 *
 * result_out receives the Tcl result (or, on a Tcl exception, the error
 * message) as a NUL-terminated string, truncated to fit; pass NULL/0 if the
 * caller does not need it (e.g. "debug break", "cont"). Returns 1 if the
 * command executed without a Tcl exception, 0 on a Tcl exception AND on
 * timeout (openMSX is not running, or the debug-pump hook -- see
 * patch-staged-tree.py -- somehow isn't draining; bounded so a wedged
 * command can never hang the caller forever).
 *
 * Only one call may be in flight at a time; concurrent callers block on
 * each other (debugger.c already serializes through its own engine mutex,
 * so this is a backstop, not the primary serialization). */
int msxhost_execute_sync(const char* command, char* result_out, int result_max);

/* Binary-safe variant for commands whose result is a Tcl bytearray -- in
 * particular "debug read_block <debuggable> <offset> <size>", the VDP/CPU-
 * regs/memory snapshot primitive the VDP debugger and register view are
 * built on. Do NOT fetch such a result through msxhost_execute_sync(): a
 * Tcl bytearray's *string* representation (what getString()/%s produce) is
 * a UTF-8-ish reencoding where byte values above 0x7F expand to multi-byte
 * sequences and 0x00 truncates the C string outright -- exactly the byte
 * range VDP registers, palette entries and VRAM contents live in. This
 * reads the Tcl_Obj's byte-array representation directly instead, so every
 * byte 0x00-0xFF survives intact.
 *
 * Copies min(<actual result length>, result_max) bytes into result_out and
 * writes the true length to *result_len_out (which may exceed result_max
 * if the buffer was too small, same convention as snprintf; pass NULL if
 * not needed). Returns 1 on success, 0 on a Tcl exception or timeout
 * (*result_len_out set to 0, result_out untouched -- there is no error
 * message on this path; use msxhost_execute_sync for commands where a
 * human-readable failure reason matters). */
int msxhost_execute_sync_binary(const char* command, uint8_t* result_out,
                                int result_max, int* result_len_out);

#ifdef __cplusplus
}  // extern "C"
#endif
