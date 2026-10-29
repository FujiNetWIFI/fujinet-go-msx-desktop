/*
 * Preferences dialog for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "msxsession.h"

G_BEGIN_DECLS

typedef struct _MsxWindow MsxWindow;

/* Shows the preferences dialog. The FujiNet options restart the session
 * when the dialog closes. */
void msx_prefs_show(MsxWindow *parent, msxsession *session);

G_END_DECLS
