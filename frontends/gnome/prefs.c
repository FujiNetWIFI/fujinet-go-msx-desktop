/*
 * Preferences dialog: the FujiNet section (use the built-in FujiNet, or
 * connect to an external one at host:port). Applied by restarting the
 * session when the dialog closes. Values persist through the shared
 * settings store, so the KDE frontend sees the same configuration.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include "window.h"

#define EXTERNAL_PLACEHOLDER "127.0.0.1:65505"

typedef struct {
    MsxWindow *window;
    msxsession *session;
    gboolean dirty;
} PrefsState;

/* The address field: valid text (or none, meaning 127.0.0.1:65505) is saved
 * and marks the session dirty; anything else is flagged and not saved. */
static void addr_changed(GtkEditable *entry, gpointer user_data)
{
    PrefsState *state = user_data;
    const char *text = gtk_editable_get_text(entry);
    char host[128];
    int port;

    if (msxsession_parse_hostport(text, host, sizeof host, &port) != 0) {
        gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
        return;
    }
    gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
    if (g_strcmp0(msxsession_get_str(state->session, "fujinet_external_addr",
                                     ""), text) == 0)
        return;
    msxsession_set_str(state->session, "fujinet_external_addr", text);
    state->dirty = TRUE;
}

static void external_toggled(GObject *row, GParamSpec *pspec,
                             gpointer user_data)
{
    PrefsState *state = g_object_get_data(G_OBJECT(row), "prefs-state");
    int on = adw_switch_row_get_active(ADW_SWITCH_ROW(row)) ? 1 : 0;
    (void)pspec;

    gtk_widget_set_sensitive(GTK_WIDGET(user_data), on);
    if (msxsession_get_int(state->session, "fujinet_external", 0) == on)
        return;
    msxsession_set_int(state->session, "fujinet_external", on);
    state->dirty = TRUE;
}

static void prefs_closed(GtkWidget *dialog, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)dialog;
    if (state->dirty) {
        msx_window_restart_session(state->window);
        msx_window_toast(state->window,
                         "FujiNet options applied (session restarted)");
    }
    g_free(state);
}

void msx_prefs_show(MsxWindow *parent, msxsession *session)
{
    PrefsState *state = g_new0(PrefsState, 1);
    AdwPreferencesDialog *dialog =
        ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    AdwPreferencesPage *page =
        ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *fujinet =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    GtkWidget *external, *addr;

    state->window = parent;
    state->session = session;

    adw_preferences_group_set_title(fujinet, "FujiNet");
    adw_preferences_group_set_description(
        fujinet,
        "Skip the built-in FujiNet and connect to another instance that is "
        "already running. Use its [BOIP] listen port (65505 by default); "
        "applied by restarting the session when this dialog closes.");
    external = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(external),
                                  "Use external FujiNet");
    adw_switch_row_set_active(
        ADW_SWITCH_ROW(external),
        msxsession_get_int(session, "fujinet_external", 0));
    g_object_set_data(G_OBJECT(external), "prefs-state", state);
    addr = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(addr),
                                  "Address:port, e.g. "
                                  EXTERNAL_PLACEHOLDER);
    gtk_editable_set_text(GTK_EDITABLE(addr),
                          msxsession_get_str(session, "fujinet_external_addr",
                                             ""));
    g_signal_connect(addr, "changed", G_CALLBACK(addr_changed), state);
    gtk_widget_set_sensitive(addr, adw_switch_row_get_active(
                                       ADW_SWITCH_ROW(external)));
    g_signal_connect(external, "notify::active", G_CALLBACK(external_toggled),
                     addr);
    adw_preferences_group_add(fujinet, external);
    adw_preferences_group_add(fujinet, addr);

    adw_preferences_page_add(page, fujinet);
    adw_preferences_dialog_add(dialog, ADW_PREFERENCES_PAGE(page));
    g_signal_connect(dialog, "closed", G_CALLBACK(prefs_closed), state);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}
