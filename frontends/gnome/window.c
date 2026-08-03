/*
 * MsxWindow: main window of the GNOME frontend. Header bar over the
 * emulator display; keyboard capture routes everything except F11
 * (fullscreen) and F12 (debugger, M6) to the MSX. No on-screen input panels
 * are shown unless the user asks for them.
 *
 * M4 adds: machine switching (live, via msxsession_set_machine -- no
 * restart) and media import (disk/cassette -> FujiNet SD, cartridge ROM ->
 * carts_dir), plus FujiNet Configuration/Console Log. Preferences and the
 * debugger land in M5/M6.
 *
 * msxsession_import_rom() (turboR/real-machine ROM -> roms_dir) is NOT
 * wired into this menu yet: openMSX needs a machine-config XML (matching
 * the real machine's hardwareconfig, referencing the ROM by name/hash) to
 * actually boot from an imported ROM, and this milestone builds the
 * session-level copy-in but not that generator. Exposing "Import System
 * ROM..." here would suggest it enables turboR/real-machine emulation when
 * it does not yet do anything the user can act on.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include <string.h>

#include "debugger/dbg_window.h"
#include "display.h"

struct _MsxWindow {
    AdwApplicationWindow parent_instance;

    msxsession *session;
    MsxDisplay *display;
    AdwToastOverlay *toasts;
};

G_DEFINE_FINAL_TYPE(MsxWindow, msx_window, ADW_TYPE_APPLICATION_WINDOW)

void msx_window_toast(MsxWindow *self, const char *message)
{
    adw_toast_overlay_add_toast(self->toasts, adw_toast_new(message));
}

/* ---- keyboard capture ---------------------------------------------------
 * Everything goes to the machine except F11 (fullscreen) and F12
 * (debugger, M6). Both press and release are forwarded: openMSX's Keyboard
 * tracks held keys, so a missed release leaves that key down in the
 * matrix. */

static gboolean forward_key(MsxWindow *self, guint keyval,
                            GdkModifierType state, int down)
{
    msxsession_key(self->session, down, keyval, gdk_keyval_to_unicode(keyval),
                  (state & GDK_CONTROL_MASK) != 0,
                  (state & GDK_SHIFT_MASK) != 0);
    return TRUE;
}

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    MsxWindow *self = MSX_WINDOW(user_data);
    (void)controller;
    (void)keycode;

    switch (keyval) {
    case GDK_KEY_F11:
        if (gtk_window_is_fullscreen(GTK_WINDOW(self)))
            gtk_window_unfullscreen(GTK_WINDOW(self));
        else
            gtk_window_fullscreen(GTK_WINDOW(self));
        return TRUE;
    case GDK_KEY_F12:
        msx_debugger_show(GTK_WINDOW(self), self->session);
        return TRUE;
    default:
        break;
    }

    return forward_key(self, keyval, state, 1);
}

static void on_key_released(GtkEventControllerKey *controller, guint keyval,
                            guint keycode, GdkModifierType state,
                            gpointer user_data)
{
    MsxWindow *self = MSX_WINDOW(user_data);
    (void)controller;
    (void)keycode;
    if (keyval == GDK_KEY_F11 || keyval == GDK_KEY_F12)
        return;
    forward_key(self, keyval, state, 0);
}

/* ---- actions ------------------------------------------------------------ */

static void action_reset(GSimpleAction *action, GVariant *param,
                         gpointer user_data)
{
    MsxWindow *self = MSX_WINDOW(user_data);
    (void)action;
    (void)param;
    msxsession_reset(self->session);
    msx_window_toast(self, "Reset");
}

/* Machine is a stateful radio action, like the sibling repos' machine/TV-
 * input/etc menus: switching is live (msxsession_set_machine), so the menu
 * is also the readout of what is currently running. */

static const struct { const char *label; const char *id; } k_machines[] = {
    { "MSX",   MSXSESSION_MACHINE_MSX },
    { "MSX2",  MSXSESSION_MACHINE_MSX2 },
    { "MSX2+", MSXSESSION_MACHINE_MSX2P },
};

static int machine_index(const char *id)
{
    size_t i;
    for (i = 0; i < G_N_ELEMENTS(k_machines); i++)
        if (strcmp(k_machines[i].id, id) == 0) return (int)i;
    return 1; /* MSX2 */
}

static void action_machine(GSimpleAction *action, GVariant *param,
                           gpointer user_data)
{
    MsxWindow *self = MSX_WINDOW(user_data);
    int idx = g_variant_get_int32(param);
    g_autofree char *msg = NULL;

    if (idx < 0 || (size_t)idx >= G_N_ELEMENTS(k_machines)) return;
    msxsession_set_machine(self->session, k_machines[idx].id);
    g_simple_action_set_state(action, g_variant_new_int32(idx));
    msg = g_strdup_printf("%s (live switch)", k_machines[idx].label);
    msx_window_toast(self, msg);
}

static void import_done(GObject *source, GAsyncResult *result,
                        gpointer user_data)
{
    MsxWindow *self = MSX_WINDOW(user_data);
    g_autoptr(GFile) file =
        gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    char dest[1024];
    g_autofree char *path = NULL;
    g_autofree char *msg = NULL;

    if (!file)
        return;
    path = g_file_get_path(file);
    if (!path)
        return;
    if (msxsession_import_media(self->session, path, dest, sizeof(dest)) != 0) {
        msx_window_toast(self, msxsession_last_error(self->session));
        return;
    }
    msg = g_strdup_printf("Copied to FujiNet SD: %s. Mount it from FujiNet "
                          "Configuration.", strrchr(dest, '/') + 1);
    msx_window_toast(self, msg);
}

static void action_import_media(GSimpleAction *action, GVariant *param,
                                gpointer user_data)
{
    MsxWindow *self = MSX_WINDOW(user_data);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GtkFileFilter *filter = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    static const char *const exts[] = { "dsk", "cas", "rom", NULL };
    int i;
    (void)action;
    (void)param;

    gtk_file_filter_set_name(filter, "MSX disk, cassette or cartridge images");
    for (i = 0; exts[i]; i++)
        gtk_file_filter_add_suffix(filter, exts[i]);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_title(dialog, "Import Disk, Cassette or Cartridge Image");
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, import_done, self);
    g_object_unref(filters);
    g_object_unref(dialog);
}

static void action_fujinet_config(GSimpleAction *action, GVariant *param,
                                  gpointer user_data)
{
    MsxWindow *self = MSX_WINDOW(user_data);
    GtkUriLauncher *launcher;
    (void)action;
    (void)param;
    /* No embedded WebKitGTK view yet (M5+); open the system browser. */
    launcher = gtk_uri_launcher_new(msxsession_fujinet_webui_url(self->session));
    gtk_uri_launcher_launch(launcher, GTK_WINDOW(self), NULL, NULL, NULL);
    g_object_unref(launcher);
}

static void action_fujinet_log(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    MsxWindow *self = MSX_WINDOW(user_data);
    char buf[16384];
    GtkWidget *dialog, *scroll, *view;
    GtkTextBuffer *tbuf;
    (void)action;
    (void)param;

    msxsession_fujinet_copy_log(self->session, buf, sizeof(buf));

    dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "FujiNet Console Log");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 700, 500);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(self));

    view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    tbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(tbuf, buf, -1);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_window_set_child(GTK_WINDOW(dialog), scroll);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void action_debugger(GSimpleAction *action, GVariant *param,
                            gpointer user_data)
{
    MsxWindow *self = MSX_WINDOW(user_data);
    (void)action;
    (void)param;
    msx_debugger_show(GTK_WINDOW(self), self->session);
}

static void action_about(GSimpleAction *action, GVariant *param,
                         gpointer user_data)
{
    MsxWindow *self = MSX_WINDOW(user_data);
    (void)action;
    (void)param;
    adw_show_about_dialog(
        GTK_WIDGET(self),
        "application-name", "FujiNet Go MSX",
        "application-icon", msx_icon_name(),
        "developer-name", "Thomas Cherryhomes",
        "version", MSX_VERSION_STRING,
        "license-type", GTK_LICENSE_GPL_3_0,
        "comments", "Self-contained MSX / MSX2 / MSX2+ with built-in FujiNet",
        "website", "https://fujinet.online/",
        NULL);
}

/* ---- construction ------------------------------------------------------- */

static const GActionEntry win_actions[] = {
    {.name = "reset", .activate = action_reset},
    {.name = "import-media", .activate = action_import_media},
    {.name = "fujinet-config", .activate = action_fujinet_config},
    {.name = "fujinet-log", .activate = action_fujinet_log},
    {.name = "debugger", .activate = action_debugger},
    {.name = "about", .activate = action_about},
};

static void msx_window_class_init(MsxWindowClass *klass)
{
    (void)klass;
}

static void msx_window_init(MsxWindow *self)
{
    (void)self;
}

static GMenu *build_menu(msxsession *session)
{
    GMenu *menu = g_menu_new();
    GMenu *machine = g_menu_new();
    GMenu *media = g_menu_new();
    GMenu *fujinet = g_menu_new();
    GMenu *tail = g_menu_new();
    size_t i;
    (void)session;

    for (i = 0; i < G_N_ELEMENTS(k_machines); i++) {
        g_autofree char *detail =
            g_strdup_printf("win.machine(%d)", (int)i);
        g_menu_append(machine, k_machines[i].label, detail);
    }
    g_menu_append(machine, "Reset", "win.reset");
    g_menu_append_section(menu, "Machine", G_MENU_MODEL(machine));

    g_menu_append(media, "Import Disk, Cassette or Cartridge…",
                 "win.import-media");
    g_menu_append_section(menu, "Media", G_MENU_MODEL(media));

    g_menu_append(fujinet, "FujiNet Configuration…", "win.fujinet-config");
    g_menu_append(fujinet, "FujiNet Console Log…", "win.fujinet-log");
    g_menu_append_section(menu, "FujiNet", G_MENU_MODEL(fujinet));

    g_menu_append(tail, "Debugger (F12)", "win.debugger");
    g_menu_append(tail, "About FujiNet Go MSX", "win.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(tail));

    g_object_unref(machine);
    g_object_unref(media);
    g_object_unref(fujinet);
    g_object_unref(tail);
    return menu;
}

GtkWidget *msx_window_new(AdwApplication *app, msxsession *session)
{
    MsxWindow *self = g_object_new(MSX_TYPE_WINDOW,
                                   "application", app,
                                   "title", "FujiNet Go MSX",
                                   "default-width", 960,
                                   "default-height", 780,
                                   NULL);
    AdwToolbarView *view;
    AdwHeaderBar *header;
    GtkButton *reset_btn;
    GtkMenuButton *menu_button;
    GMenu *menu;
    GtkEventController *keys;

    GSimpleAction *machine_action;

    self->session = session;

    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
                                    G_N_ELEMENTS(win_actions), self);

    machine_action = g_simple_action_new_stateful(
        "machine", G_VARIANT_TYPE_INT32,
        g_variant_new_int32(machine_index(msxsession_machine(session))));
    g_signal_connect(machine_action, "change-state",
                     G_CALLBACK(action_machine), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(machine_action));
    g_object_unref(machine_action);

    header = ADW_HEADER_BAR(adw_header_bar_new());

    reset_btn = GTK_BUTTON(gtk_button_new_from_icon_name("view-refresh-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(reset_btn), "Reset");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(reset_btn), "win.reset");
    adw_header_bar_pack_start(header, GTK_WIDGET(reset_btn));

    menu = build_menu(session);
    menu_button = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_menu_button_set_icon_name(menu_button, "open-menu-symbolic");
    gtk_menu_button_set_menu_model(menu_button, G_MENU_MODEL(menu));
    g_object_unref(menu);
    adw_header_bar_pack_end(header, GTK_WIDGET(menu_button));

    self->display = MSX_DISPLAY(msx_display_new(session));
    self->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(self->toasts, GTK_WIDGET(self->display));

    view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(view, GTK_WIDGET(header));
    adw_toolbar_view_set_content(view, GTK_WIDGET(self->toasts));
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self),
                                       GTK_WIDGET(view));

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), keys);

    gtk_widget_grab_focus(GTK_WIDGET(self->display));

    /* Developer affordance, matching the sibling repos: open the debugger
     * alongside the main window. */
    if (getenv("MSX_OPEN_DEBUGGER"))
        msx_debugger_show(GTK_WINDOW(self), session);
    return GTK_WIDGET(self);
}
