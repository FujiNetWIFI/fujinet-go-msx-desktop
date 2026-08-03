/*
 * MsxWindow: main window of the GNOME frontend. Header bar over the
 * emulator display; keyboard capture routes everything except F11
 * (fullscreen) and F12 (debugger, M6) to the MSX. No on-screen input panels
 * are shown unless the user asks for them.
 *
 * M2 scope: no menu (machine switching, media import, FujiNet config,
 * preferences land in M3/M4/M5), just Reset + fullscreen + keyboard.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

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
        /* Debugger lands in M6; swallow the key rather than forward it. */
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

    self->session = session;

    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
                                    G_N_ELEMENTS(win_actions), self);

    header = ADW_HEADER_BAR(adw_header_bar_new());

    reset_btn = GTK_BUTTON(gtk_button_new_from_icon_name("view-refresh-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(reset_btn), "Reset");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(reset_btn), "win.reset");
    adw_header_bar_pack_start(header, GTK_WIDGET(reset_btn));

    menu = g_menu_new();
    g_menu_append(menu, "About FujiNet Go MSX", "win.about");
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
     * alongside the main window (M6; a no-op until then). */
    return GTK_WIDGET(self);
}
