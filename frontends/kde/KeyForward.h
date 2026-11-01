/*
 * Key translation shared by the GPU and software displays: Qt key events to
 * the X11 keysyms msx_key_from_event expects (core/src/input_map.c), so this
 * frontend and the GNOME one share one mapping table -- the numeric XK_*
 * values are exactly the ones that file hardcodes, deliberately kept in sync.
 * Both press and release are forwarded: openMSX's Keyboard tracks held keys,
 * so a missed release leaves that key down in the matrix.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QEvent>
#include <QKeyEvent>

#include "msxsession.h"

/* Handles QEvent::ShortcutOverride: claim every key except the reserved
 * chords (F10 menu, F11 fullscreen, F12 debugger) so menu accelerators cannot
 * shadow what the MSX should receive. Alt in particular must NOT reach the
 * menu bar: it is a real key on an MSX keyboard. Returns true if claimed. */
bool msxClaimShortcut(QEvent *event);

/* Auto-repeat is swallowed (openMSX holds a key until released; the MSX BIOS
 * does its own repeat). Always accepts the event. */
void msxKeyEvent(msxsession *session, QKeyEvent *event, int down);
