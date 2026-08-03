/*
 * MsxDisplay: paints the emulator's latest XRGB8888 frame with a
 * GdkMemoryTexture, letterboxed to 4:3 inside whatever size the window
 * manager gives us (tiling or floating). A frame-clock tick callback feeds
 * the session's (advisory-only) vsync notification and pulls the latest
 * frame.
 *
 * Unlike the sibling targets, the frame's geometry is always exactly
 * MSXSESSION_FB_WIDTH x MSXSESSION_FB_HEIGHT (640x480, already 4:3) --
 * openMSX's own FrameSource::getLinePtr640_480() scales every source video
 * mode to this fixed size before msxsession ever sees it, so there is no
 * per-frame texture resize or aspect-mode table to carry here. Smooth
 * scaling / aspect-mode preferences land with prefs.c in M4.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "display.h"

struct _MsxDisplay {
    GtkWidget parent_instance;

    msxsession *session;
    GdkTexture *texture;
    /* Heap-allocated: a GObject instance must stay under 64K, and this is
     * ~1.2MB. */
    uint32_t *fb;
    uint64_t serial;
    guint tick_id;
};

G_DEFINE_FINAL_TYPE(MsxDisplay, msx_display, GTK_TYPE_WIDGET)

/* The core's XRGB8888 is a uint32 0x00RRGGBB, so in memory on a
 * little-endian host the bytes run B,G,R,X -- which is exactly
 * GDK_MEMORY_B8G8R8X8, and no per-pixel conversion is needed. On a
 * big-endian host the same uint32 lays out X,R,G,B. */
#if G_BYTE_ORDER == G_BIG_ENDIAN
#define MSX_GDK_FORMAT GDK_MEMORY_X8R8G8B8
#else
#define MSX_GDK_FORMAT GDK_MEMORY_B8G8R8X8
#endif

static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data)
{
    MsxDisplay *self = MSX_DISPLAY(widget);
    (void)user_data;

    if (!self->session)
        return G_SOURCE_CONTINUE;

    /* Frame-clock time is CLOCK_MONOTONIC microseconds. Advisory only for
     * MSX -- see msxsession.h -- but kept so MSX_PACE_LOG=1 has something
     * to report. */
    msxsession_notify_vsync(self->session,
                            gdk_frame_clock_get_frame_time(clock) * 1000);

    if (msxsession_copy_frame(self->session, self->fb, &self->serial)) {
        GBytes *bytes = g_bytes_new(self->fb,
                                    (gsize)MSXSESSION_FB_WIDTH *
                                    MSXSESSION_FB_HEIGHT * sizeof(uint32_t));
        g_clear_object(&self->texture);
        self->texture = gdk_memory_texture_new(
            MSXSESSION_FB_WIDTH, MSXSESSION_FB_HEIGHT, MSX_GDK_FORMAT, bytes,
            (gsize)MSXSESSION_FB_WIDTH * sizeof(uint32_t));
        g_bytes_unref(bytes);
        gtk_widget_queue_draw(widget);
    }
    return G_SOURCE_CONTINUE;
}

static void msx_display_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    MsxDisplay *self = MSX_DISPLAY(widget);
    const float w = (float)gtk_widget_get_width(widget);
    const float h = (float)gtk_widget_get_height(widget);
    const float aspect = (float)MSXSESSION_FB_WIDTH / (float)MSXSESSION_FB_HEIGHT;
    float dw, dh;
    graphene_rect_t dest;

    gtk_snapshot_append_color(snapshot, &(GdkRGBA){0, 0, 0, 1},
                              &GRAPHENE_RECT_INIT(0, 0, w, h));
    if (!self->texture || w < 1 || h < 1)
        return;

    if (w / h > aspect) {
        dh = h;
        dw = h * aspect;
    } else {
        dw = w;
        dh = w / aspect;
    }

    dest = GRAPHENE_RECT_INIT((w - dw) / 2.0f, (h - dh) / 2.0f, dw, dh);
    gtk_snapshot_append_scaled_texture(snapshot, self->texture,
                                       GSK_SCALING_FILTER_NEAREST, &dest);
}

static void msx_display_dispose(GObject *object)
{
    MsxDisplay *self = MSX_DISPLAY(object);
    if (self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
    g_clear_object(&self->texture);
    g_clear_pointer(&self->fb, g_free);
    G_OBJECT_CLASS(msx_display_parent_class)->dispose(object);
}

static void msx_display_class_init(MsxDisplayClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    widget_class->snapshot = msx_display_snapshot;
    object_class->dispose = msx_display_dispose;
}

static void msx_display_init(MsxDisplay *self)
{
    self->fb = g_malloc0((gsize)MSXSESSION_FB_WIDTH *
                         MSXSESSION_FB_HEIGHT * sizeof(uint32_t));
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
    self->tick_id =
        gtk_widget_add_tick_callback(GTK_WIDGET(self), tick_cb, NULL, NULL);
}

GtkWidget *msx_display_new(msxsession *session)
{
    MsxDisplay *self = g_object_new(MSX_TYPE_DISPLAY, NULL);
    self->session = session;
    return GTK_WIDGET(self);
}
