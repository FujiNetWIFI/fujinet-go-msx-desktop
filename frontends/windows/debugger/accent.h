/*
 * The app's accent colour for the debugger windows: the icon's blue, from
 * the session header, so the PC line, the stopped Run button and the
 * changed-register rows all follow the icon. ink is the text on an accent
 * fill; light is the accent for lines and text on a dark ground.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSX_WIN_DBG_ACCENT_H
#define MSX_WIN_DBG_ACCENT_H

#include <windows.h>
#include <commctrl.h>

#include "msxsession.h"

#define MSX_RGB_OF(hex) RGB(((hex) >> 16) & 0xFF, ((hex) >> 8) & 0xFF, (hex) & 0xFF)
#define MSX_ACCENT_COLORREF MSX_RGB_OF(MSXSESSION_ACCENT_RGB)
#define MSX_ACCENT_INK_COLORREF MSX_RGB_OF(MSXSESSION_ACCENT_INK_RGB)
#define MSX_ACCENT_LIGHT_COLORREF MSX_RGB_OF(MSXSESSION_ACCENT_LIGHT_RGB)

/* Handles one NM_CUSTOMDRAW from a toggle button (comctl32 v6, see
 * app.manifest.in): on = its state; the accent fill with the ink while it is
 * on, the themed look while it is off. The result is what the parent's
 * WM_NOTIFY should return. */
static inline LRESULT msx_accent_customdraw(const NMCUSTOMDRAW *cd, int on)
{
    static HBRUSH accent;
    char text[64];
    RECT r;
    if (cd->dwDrawStage != CDDS_PREPAINT || !on) return CDRF_DODEFAULT;
    if (!accent) accent = CreateSolidBrush(MSX_ACCENT_COLORREF);
    r = cd->rc;
    FillRect(cd->hdc, &r, accent);
    FrameRect(cd->hdc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
    SetBkMode(cd->hdc, TRANSPARENT);
    SetTextColor(cd->hdc, MSX_ACCENT_INK_COLORREF);
    text[0] = '\0';
    GetWindowTextA(cd->hdr.hwndFrom, text, sizeof text);
    {
        HFONT f = (HFONT)SendMessageA(cd->hdr.hwndFrom, WM_GETFONT, 0, 0);
        HGDIOBJ old = f ? SelectObject(cd->hdc, f) : NULL;
        DrawTextA(cd->hdc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (old) SelectObject(cd->hdc, old);
    }
    if (GetFocus() == cd->hdr.hwndFrom) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(cd->hdc, &r);
    }
    return CDRF_SKIPDEFAULT;
}

#endif
