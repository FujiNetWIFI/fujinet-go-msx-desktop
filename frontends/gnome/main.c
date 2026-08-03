/*
 * FujiNet Go MSX -- GNOME frontend entry point.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <adwaita.h>

#include "msxsession.h"
#include "window.h"

static msxsession *g_session;

/* The installed icons are named after the desktop-entry id. Running straight
 * out of the build tree there is nothing installed to look up, so point the
 * icon theme at the in-tree artwork (named for the project, not the id) and
 * report that name instead. */
const char *msx_icon_name(void)
{
    static const char *name;
    GtkIconTheme *theme;

    if (name)
        return name;

    name = MSX_APP_ID;
    theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    if (theme && !gtk_icon_theme_has_icon(theme, name)) {
        gtk_icon_theme_add_search_path(theme, MSX_SOURCE_ICON_DIR);
        if (gtk_icon_theme_has_icon(theme, "fujinet-go-msx"))
            name = "fujinet-go-msx";
    }
    return name;
}

static void on_activate(AdwApplication *app, gpointer user_data)
{
    GtkWindow *win;
    (void)user_data;

    win = gtk_application_get_active_window(GTK_APPLICATION(app));
    if (win) {
        gtk_window_present(win);
        return;
    }

    if (!g_session) {
        msxsession_start_opts opts;
        g_session = msxsession_new(NULL);
        if (!g_session) {
            g_printerr("fatal: could not create the session\n");
            g_application_quit(G_APPLICATION(app));
            return;
        }
        msxsession_default_opts(g_session, &opts);
        if (msxsession_start(g_session, &opts) != 0)
            g_printerr("session start: %s\n",
                       msxsession_last_error(g_session));
    }

    win = GTK_WINDOW(msx_window_new(app, g_session));
    gtk_window_set_icon_name(win, msx_icon_name());
    gtk_window_present(win);
}

static void on_shutdown(GApplication *app, gpointer user_data)
{
    (void)app;
    (void)user_data;
    if (g_session) {
        msxsession_free(g_session);
        g_session = NULL;
    }
}

int main(int argc, char *argv[])
{
    g_autoptr(AdwApplication) app = adw_application_new(
        MSX_APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
    return g_application_run(G_APPLICATION(app), argc, argv);
}
