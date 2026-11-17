/*
 * DisplayExtras.inc -- the keyboard, included into the display view's
 * @implementation (see DisplayGlue.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

- (void)extraInit {}

/* The frame clock follows the window (viewDidMoveToWindow); the app asks for
 * it explicitly once the window is ordered in. */
- (void)start { [self updateVisibility]; }

/* ---- input -------------------------------------------------------------------
 * msxsession_key wants an X11/xkb keysym (or 0, meaning "look at unicode
 * instead") plus the raw unicode character -- the session does the MSX-key
 * translation itself via msx_key_from_event (which also declines F10/F11/
 * F12, reserved for the frontend -- menu/fullscreen/debugger here -- so no
 * special-case swallowing is needed in this view). Table unchanged from
 * the CoCo/ADAM siblings: NSEvent.charactersIgnoringModifiers already
 * gives a layout-aware unshifted character, and these are already X11
 * keysym values (e.g. 0xFF52 = XK_Up). */

- (BOOL)acceptsFirstResponder
{
    return YES;
}

static uint32_t keysym_for_char(unichar c)
{
    switch (c) {
    case NSUpArrowFunctionKey:    return 0xFF52;
    case NSDownArrowFunctionKey:  return 0xFF54;
    case NSLeftArrowFunctionKey:  return 0xFF51;
    case NSRightArrowFunctionKey: return 0xFF53;
    case NSHomeFunctionKey:       return 0xFF50;
    case NSEndFunctionKey:        return 0xFF57;
    case NSPageUpFunctionKey:     return 0xFF55;
    case NSPageDownFunctionKey:   return 0xFF56;
    case NSInsertFunctionKey:     return 0xFF63;
    case NSDeleteFunctionKey:     return 0xFFFF; /* forward delete */
    case 0x7F:                    return 0xFF08; /* backspace */
    case 0x0D: case 0x03:         return 0xFF0D; /* return / enter */
    case 0x09:                    return 0xFF09; /* tab */
    case 0x1B:                    return 0xFF1B; /* escape */
    default:
        if (c >= NSF1FunctionKey && c <= NSF1FunctionKey + 11)
            return 0xFFBE + (uint32_t)(c - NSF1FunctionKey);
        return 0;
    }
}

- (void)forwardKeyEvent:(NSEvent *)event down:(int)down
{
    NSString *chars = event.charactersIgnoringModifiers;
    unichar c = chars.length ? [chars characterAtIndex:0] : 0;
    const int ctrl = (event.modifierFlags & NSEventModifierFlagControl) != 0;
    const int shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
    uint32_t keysym = keysym_for_char(c);
    uint32_t unicode = (c >= 0x20 && c < 0xF700) ? c : 0;

    if (!keysym && !unicode)
        return; /* not a key the emulated keyboard has */
    msxsession_key(_session, down, keysym, unicode, ctrl, shift);
}

- (void)keyDown:(NSEvent *)event
{
    [self forwardKeyEvent:event down:1];
}

- (void)keyUp:(NSEvent *)event
{
    [self forwardKeyEvent:event down:0];
}

