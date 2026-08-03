/*
 * Debugger window for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "msxsession.h"

G_BEGIN_DECLS

/* Shows (creating on first use) the debugger window for the session. */
void msx_debugger_show(GtkWindow *parent, msxsession *session);

G_END_DECLS
