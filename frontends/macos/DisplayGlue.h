/*
 * DisplayGlue.h -- what DisplayView.m needs to know about THIS machine: the
 * session's functions, the frame size and the shape of the picture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "msxsession.h"

#define FN_VIEW      DisplayView
#define FN_SESSION   msxsession
#define FN_FB_W      MSXSESSION_FB_WIDTH
#define FN_FB_H      MSXSESSION_FB_HEIGHT
#define FN_INITIAL_W 800
#define FN_INITIAL_H 600

/* The keyboard and start() (DisplayExtras.inc). */
#define FN_EXTRA_METHODS "DisplayExtras.inc"

/* openMSX paces itself; the session's vsync call only logs. */
static inline int fn_machine_hz(FN_SESSION *s) { (void)s; return 60; }

static inline void fn_notify_vsync(FN_SESSION *s, int64_t ns)
{
    msxsession_notify_vsync(s, ns);
}

/* Every frame is the whole 640 x 480; nothing to measure. */
static inline int fn_pull_frame(FN_SESSION *s, uint32_t *dst, int *lines, uint64_t *serial)
{
    *lines = FN_FB_H;
    return msxsession_copy_frame(s, dst, serial);
}

/* The picture's own shape: openMSX's 640 x 480 is already the 4:3 picture. */
static inline double fn_picture_aspect(FN_SESSION *s, int lines, BOOL tv)
{
    (void)s; (void)tv;
    return (double)FN_FB_W / (double)lines;
}
