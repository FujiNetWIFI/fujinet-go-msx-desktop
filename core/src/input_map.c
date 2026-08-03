/*
 * msx_key_from_event -- translate a desktop key event (X11/xkb keysym) to
 * the SDL_Keycode + modifier bits core/openmsx/msx_host.hh's
 * msxhost_inject_key() forwards to openMSX's Keyboard.
 *
 * Unlike the sibling targets' input_map.c, there is no MSX keyboard-matrix
 * table here: openMSX's own Keyboard::processKeyEvent already does
 * SDL keycode/scancode -> matrix-row/column translation internally (it is
 * the same translation desktop openMSX does for real SDL keyboard input).
 * This file's only job is the desktop-keysym -> SDL_Keycode step, which
 * openMSX does not need to know exists.
 *
 * Most printable characters need no table at all: X11 keysyms and SDL
 * keycodes are both, for the Latin-1 range, numerically identical to their
 * ASCII/Unicode code point (XK_a == 'a' == SDLK_a == 0x61) -- both are
 * defined that way (X11's keysymdef.h; SDL_keycode.h's SDLK_a = 'a'). The
 * table below covers the two families' *non-printable* ranges, which use
 * completely different numeric spaces (X11: 0xFF00+; SDL: keycodes with
 * SDLK_SCANCODE_MASK ((1<<30)) set).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL.h>

#include "msxsession.h"

/* X11 keysym values (see /usr/include/X11/keysymdef.h), hardcoded rather
 * than pulling in <X11/keysymdef.h> so this file -- and the macOS/Windows
 * frontends that synthesize X11-keysym-shaped values through their own
 * small mapping tables, per the family convention ("GDK keyvals are already
 * that") -- carries no X11 header dependency. */
#define XK_BackSpace  0xff08
#define XK_Tab        0xff09
#define XK_Return     0xff0d
#define XK_Escape     0xff1b
#define XK_Delete     0xffff
#define XK_Home       0xff50
#define XK_Left       0xff51
#define XK_Up         0xff52
#define XK_Right      0xff53
#define XK_Down       0xff54
#define XK_Page_Up    0xff55
#define XK_Page_Down  0xff56
#define XK_End        0xff57
#define XK_Insert     0xff63
#define XK_KP_Enter   0xff8d
#define XK_F1         0xffbe
#define XK_F2         0xffbf
#define XK_F3         0xffc0
#define XK_F4         0xffc1
#define XK_F5         0xffc2
#define XK_F6         0xffc3
#define XK_F7         0xffc4
#define XK_F8         0xffc5
#define XK_F9         0xffc6
#define XK_F10        0xffc7
#define XK_F11        0xffc8
#define XK_F12        0xffc9
#define XK_Shift_L    0xffe1
#define XK_Shift_R    0xffe2
#define XK_Control_L  0xffe3
#define XK_Control_R  0xffe4
#define XK_Caps_Lock  0xffe5
#define XK_Alt_L      0xffe9
#define XK_Alt_R      0xffea
#define XK_Super_L    0xffeb
#define XK_Super_R    0xffec

typedef struct { uint32_t keysym; SDL_Keycode sdlk; } key_map_entry;

static const key_map_entry k_specials[] = {
    { XK_BackSpace,  SDLK_BACKSPACE },
    { XK_Tab,        SDLK_TAB },
    { XK_Return,     SDLK_RETURN },
    { XK_KP_Enter,   SDLK_KP_ENTER },
    { XK_Escape,     SDLK_ESCAPE },
    { XK_Delete,     SDLK_DELETE },
    { XK_Home,       SDLK_HOME },
    { XK_End,        SDLK_END },
    { XK_Page_Up,    SDLK_PAGEUP },
    { XK_Page_Down,  SDLK_PAGEDOWN },
    { XK_Insert,     SDLK_INSERT },
    { XK_Left,       SDLK_LEFT },
    { XK_Right,      SDLK_RIGHT },
    { XK_Up,         SDLK_UP },
    { XK_Down,       SDLK_DOWN },
    { XK_Caps_Lock,  SDLK_CAPSLOCK },
    { XK_Shift_L,    SDLK_LSHIFT },
    { XK_Shift_R,    SDLK_RSHIFT },
    { XK_Control_L,  SDLK_LCTRL },
    { XK_Control_R,  SDLK_RCTRL },
    { XK_Alt_L,      SDLK_LALT },
    { XK_Alt_R,      SDLK_RALT },
    { XK_Super_L,    SDLK_LGUI },
    { XK_Super_R,    SDLK_RGUI },
    { XK_F1,  SDLK_F1 },  { XK_F2,  SDLK_F2 },  { XK_F3,  SDLK_F3 },
    { XK_F4,  SDLK_F4 },  { XK_F5,  SDLK_F5 },  { XK_F6,  SDLK_F6 },
    { XK_F7,  SDLK_F7 },  { XK_F8,  SDLK_F8 },  { XK_F9,  SDLK_F9 },
    /* F10, F11, F12 are reserved for the frontends (menu, fullscreen,
     * debugger) and are filtered out below before this table is consulted;
     * listing them here would be dead code. */
};

int msx_key_from_event(uint32_t keysym, uint32_t unicode, int ctrl_down,
                       int shift_down, unsigned *keycode_out,
                       uint16_t *mods_out)
{
    SDL_Keycode sdlk = 0;
    Uint16 mods = 0;
    size_t i;
    (void)unicode;

    if (keysym == XK_F10 || keysym == XK_F11 || keysym == XK_F12)
        return 0;

    if (keysym < 0x100) {
        /* Latin-1 range: X11 keysyms and SDL keycodes are both defined to
         * equal the ASCII/Unicode code point here, so no table lookup is
         * needed. */
        sdlk = (SDL_Keycode)keysym;
    } else {
        for (i = 0; i < sizeof(k_specials) / sizeof(k_specials[0]); i++) {
            if (k_specials[i].keysym == keysym) {
                sdlk = k_specials[i].sdlk;
                break;
            }
        }
        if (!sdlk)
            return 0; /* not a key the MSX keyboard has */
    }

    if (ctrl_down)  mods |= KMOD_LCTRL;
    if (shift_down) mods |= KMOD_LSHIFT;

    *keycode_out = (unsigned)sdlk;
    *mods_out = mods;
    return 1;
}
