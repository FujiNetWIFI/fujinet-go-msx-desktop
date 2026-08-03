/*
 * MsxWindow: main window of the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "msxsession.h"

G_BEGIN_DECLS

#define MSX_TYPE_WINDOW (msx_window_get_type())
G_DECLARE_FINAL_TYPE(MsxWindow, msx_window, MSX, WINDOW,
                     AdwApplicationWindow)

GtkWidget *msx_window_new(AdwApplication *app, msxsession *session);
void msx_window_toast(MsxWindow *self, const char *message);

/* main.c: the icon name to use, which differs between an installed app and
 * one running out of the build tree. */
const char *msx_icon_name(void);

G_END_DECLS
