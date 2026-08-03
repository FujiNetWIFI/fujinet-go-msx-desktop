/*
 * MsxDisplay: the emulator video widget for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "msxsession.h"

G_BEGIN_DECLS

#define MSX_TYPE_DISPLAY (msx_display_get_type())
G_DECLARE_FINAL_TYPE(MsxDisplay, msx_display, MSX, DISPLAY, GtkWidget)

GtkWidget *msx_display_new(msxsession *session);

G_END_DECLS
