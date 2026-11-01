/*
 * KeyForward -- see KeyForward.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeyForward.h"

namespace {

/* Qt key code -> X11/xkb keysym, for the keys msx_key_from_event resolves
 * by keysym rather than by character. Values match
 * core/src/input_map.c's XK_* table exactly. */
quint32 keysymForKey(const QKeyEvent *event)
{
    /* F1..F9. F10, F11 and F12 are the frontend's own (menu, fullscreen,
     * debugger) and never reach here -- event() lets those three through as
     * shortcuts instead of claiming them. */
    if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F9)
        return 0xFFBE + (quint32)(event->key() - Qt::Key_F1);

    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:     return 0xFF0D;
    case Qt::Key_Escape:    return 0xFF1B;
    case Qt::Key_Tab:       return 0xFF09;
    case Qt::Key_Backspace: return 0xFF08;
    case Qt::Key_Delete:    return 0xFFFF;
    case Qt::Key_Insert:    return 0xFF63;
    case Qt::Key_Home:      return 0xFF50;
    case Qt::Key_End:       return 0xFF57;
    case Qt::Key_PageUp:    return 0xFF55;
    case Qt::Key_PageDown:  return 0xFF56;
    case Qt::Key_Left:      return 0xFF51;
    case Qt::Key_Up:        return 0xFF52;
    case Qt::Key_Right:     return 0xFF53;
    case Qt::Key_Down:      return 0xFF54;
    /* Modifiers are real MSX keys, so forwarded rather than swallowed. Qt
     * reports left and right as the same key code for Shift/Control/Alt, so
     * both land on the left one -- input_map.c's table has no right-hand
     * equivalent to distinguish anyway for the keys this needs. */
    case Qt::Key_Shift:     return 0xFFE1; /* Shift_L */
    case Qt::Key_Control:   return 0xFFE3; /* Control_L */
    case Qt::Key_Alt:       return 0xFFE9; /* Alt_L */
    case Qt::Key_AltGr:     return 0xFFEA; /* Alt_R */
    case Qt::Key_Meta:      return 0xFFEB; /* Super_L */
    case Qt::Key_CapsLock:  return 0xFFE5;
    default:                return 0;
    }
}

} // namespace

static void msxForwardKey(msxsession *session, const QKeyEvent *event, int down)
{
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    quint32 unicode = 0;
    if (!event->text().isEmpty())
        unicode = event->text().at(0).unicode();

    quint32 keysym = keysymForKey(event);
    if (keysym == 0) {
        /* Printable: Qt gives no text for a Ctrl combo, so fall back to the
         * unshifted letter, which is what the keysym contract wants. */
        if (ctrl && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z)
            keysym = 'a' + (quint32)(event->key() - Qt::Key_A);
        else
            keysym = unicode;
    }
    if (keysym == 0)
        return;

    msxsession_key(session, down, keysym, unicode, ctrl ? 1 : 0,
                  shift ? 1 : 0);
}

bool msxClaimShortcut(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() != Qt::Key_F10 && ke->key() != Qt::Key_F11 &&
            ke->key() != Qt::Key_F12) {
            event->accept();
            return true;
        }
    }
    return false;
}

void msxKeyEvent(msxsession *session, QKeyEvent *event, int down)
{
    if (!event->isAutoRepeat())
        msxForwardKey(session, event, down);
    event->accept();
}
