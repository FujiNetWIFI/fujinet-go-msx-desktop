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

#ifdef __cplusplus
}  // extern "C"
#endif
