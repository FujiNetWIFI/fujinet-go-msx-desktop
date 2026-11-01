/*
 * Where the picture goes inside the display, shared by the GPU and the
 * software display: the fixed 640x480 frame (already 4:3), letterboxed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QRectF>
#include <QSizeF>

#include "msxsession.h"

inline QRectF displayRect(QSizeF avail)
{
    constexpr double want = (double)MSXSESSION_FB_WIDTH / MSXSESSION_FB_HEIGHT;
    const double w = avail.width(), h = avail.height();
    double sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
    return QRectF((w - sw) / 2.0, (h - sh) / 2.0, sw, sh);
}
