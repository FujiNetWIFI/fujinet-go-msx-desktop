/*
 * Debugger window for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "msxsession.h"

G_BEGIN_DECLS

/* Shows (creating on first use) the debugger window for the session;
 * showing attaches the engine, which stops the machine. */
void msx_debugger_show(GtkWindow *parent, msxsession *session);

/* Shows it if hidden or absent, hides it (detaching, so the machine runs)
 * if showing: the main window's F12 and menu item. */
void msx_debugger_toggle(GtkWindow *parent, msxsession *session);

G_END_DECLS
