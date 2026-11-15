/*
 * The debugger's Video tab (Win32).
 *
 * Everything that decides what to show lives in the core (msxvidtab.h): this file is
 * controls and a GDI picture. The picture is the controller's cached 0xFFRRGGBB buffer drawn
 * 1:1 with StretchDIBits (BI_RGB ignores the alpha byte), double buffered; a mouse move
 * redraws only the marks. Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dbg_video.h"

#include <commctrl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accent.h"
#include "msxsession.h"
#include "msxdebug.h"
#include "msxvdp.h"
#include "msxvidcap.h"
#include "msxvidtab.h"

#define PAGE_CLASS L"FujiNetGoMSXVideoPage"
#define PIC_CLASS L"FujiNetGoMSXVideoPic"
#define COL_CLASS L"FujiNetGoMSXVideoColours"

#define ACCENT MSX_ACCENT_COLORREF                  /* the icon blue: fills and outlines on light */
#define ACCENT_INK MSX_ACCENT_INK_COLORREF          /* ink on the accent fill */
#define ACCENT_LIGHT MSX_ACCENT_LIGHT_COLORREF      /* accent lines and text on a dark ground */
#define MAX_OVERLAYS 16
#define PAL_COLS 4          /* the 16 colours, 4 x 4 */
#define PAL_ROWS 4

enum {
    IDV_VIEW = 3000, IDV_SOURCE, IDV_LABEL_SRC, IDV_ZOOM, IDV_BREAK, IDV_STATUS, IDV_NOTE, IDV_SEC, IDV_PIXEL, IDV_UNPIN,
    IDV_REGS, IDV_LINES, IDV_BEAM, IDV_TIMING, IDV_PIC, IDV_COLS, IDV_LABEL_VIEW, IDV_LABEL_ZOOM,
    IDV_BANK, IDV_LABEL_BANK, IDV_VRAM_TITLE, IDV_VRAM_ADDR, IDV_VRAM_PREV, IDV_VRAM_NEXT, IDV_VRAM_NOTE, IDV_VRAM_VIEW,
    IDV_OV0 = 3200
};

#define VRAM_STEP 256       /* the VRAM section's page: 16 rows of 16 bytes */
#define POKE_MAX 64

#define MAX_ZOOMS 16

typedef struct {
    unsigned char *hot;
    int nhot;
    uint64_t sig;
    int have_sig;
} listfill;

typedef struct {
    HWND page, view, source, zoom, brk, status, note, pic, sec;
    HWND ov[MAX_OVERLAYS];
    unsigned ov_bit[MAX_OVERLAYS];
    int nov;
    HWND label_view, label_zoom, label_src, label_bank, bank;
    HWND pixel, unpin, regs, lines, cols, beam, timing;
    HWND vram_title, vram_addr, vram_prev, vram_next, vram_note, vram_view;
    int vram_note_set;          /* the note holds a message, not just the address */
    msxdebug *engine;
    HFONT ui, mono;
    int dpi;
    msxvid_tab *tab;
    int updating;
    int scroll_x, scroll_y;
    int tracking;
    int pal_hover, col_tracking;
    int zoom_val[MAX_ZOOMS], nzoom;
    listfill rf, lf;
    BITMAPINFO bmi;
} vidwin;

#define S(v, n) MulDiv((n), (v)->dpi, 96)

/* ---- text ------------------------------------------------------------------------ */

/* UTF-8 to UTF-16; '\n' becomes CR LF (multi-line controls want it) */
static wchar_t *wide(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0), i;
    wchar_t *w = malloc(((size_t)n + 1) * sizeof *w), *o, *d;
    if (!w) return NULL;
    o = malloc(((size_t)n * 2 + 2) * sizeof *w);
    if (!o) {
        free(w);
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    d = o;
    for (i = 0; w[i]; i++) {
        if (w[i] == L'\n') *d++ = L'\r';
        *d++ = w[i];
    }
    *d = 0;
    free(w);
    return o;
}

/* a control's text, set only when it changed (no flicker on the live refresh) */
static void set_text(HWND h, const char *utf8)
{
    wchar_t *w = wide(utf8), cur[2048];
    if (!w) return;
    GetWindowTextW(h, cur, 2048);
    if (wcscmp(cur, w) != 0) SetWindowTextW(h, w);
    free(w);
}

static uint64_t fnv(uint64_t h, const void *data, size_t n)
{
    const unsigned char *p = data;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

/* ---- the picture --------------------------------------------------------------------- */

static void pic_update_scroll(vidwin *v)
{
    RECT rc;
    SCROLLINFO si;
    int pw = 0, ph = 0;
    GetClientRect(v->pic, &rc);
    msxvid_tab_picture(v->tab, &pw, &ph);
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = pw > 0 ? pw - 1 : 0;
    si.nPage = (UINT)rc.right;
    if (v->scroll_x > pw - rc.right) v->scroll_x = pw > rc.right ? pw - rc.right : 0;
    si.nPos = v->scroll_x;
    SetScrollInfo(v->pic, SB_HORZ, &si, TRUE);
    si.nMax = ph > 0 ? ph - 1 : 0;
    si.nPage = (UINT)rc.bottom;
    if (v->scroll_y > ph - rc.bottom) v->scroll_y = ph > rc.bottom ? ph - rc.bottom : 0;
    si.nPos = v->scroll_y;
    SetScrollInfo(v->pic, SB_VERT, &si, TRUE);
}

static void pic_paint(vidwin *v, HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps), mem;
    RECT rc;
    HBITMAP bmp, old;
    int pw = 0, ph = 0, marks, hover[4] = { 0 }, pin[4] = { 0 };
    const uint32_t *px;
    HBRUSH bg = CreateSolidBrush(RGB(18, 18, 24));
    GetClientRect(h, &rc);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
    old = SelectObject(mem, bmp);
    FillRect(mem, &rc, bg);
    px = msxvid_tab_picture(v->tab, &pw, &ph);
    if (!px) {
        const char *why = msxvid_tab_why(v->tab);
        wchar_t *w = wide(why && *why ? why : "Nothing to show");
        RECT tr = rc;
        InflateRect(&tr, -S(v, 10), -S(v, 10));
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(140, 140, 148));
        SelectObject(mem, v->ui);
        if (w) DrawTextW(mem, w, -1, &tr, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        free(w);
    } else {
        HPEN pen_h, pen_p, pen_k, oldpen;
        HGDIOBJ oldbr;
        v->bmi.bmiHeader.biWidth = pw;      /* the buffer is pw words per row, whatever the view */
        v->bmi.bmiHeader.biHeight = -ph;    /* top-down */
        SetStretchBltMode(mem, COLORONCOLOR);
        StretchDIBits(mem, -v->scroll_x, -v->scroll_y, pw, ph, 0, 0, pw, ph, px, &v->bmi, DIB_RGB_COLORS, SRCCOPY);
        marks = msxvid_tab_marks(v->tab, hover, pin);
        oldbr = SelectObject(mem, GetStockObject(NULL_BRUSH));
        pen_h = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        pen_p = CreatePen(PS_SOLID, 2, ACCENT_LIGHT);
        pen_k = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
        if (marks & 1) {            /* hover: a white box just outside the pixel, over a dark one */
            oldpen = SelectObject(mem, pen_k);
            Rectangle(mem, hover[0] - v->scroll_x - 2, hover[1] - v->scroll_y - 2, hover[0] - v->scroll_x + hover[2] + 2,
                      hover[1] - v->scroll_y + hover[3] + 2);
            SelectObject(mem, pen_h);
            Rectangle(mem, hover[0] - v->scroll_x - 1, hover[1] - v->scroll_y - 1, hover[0] - v->scroll_x + hover[2] + 1,
                      hover[1] - v->scroll_y + hover[3] + 1);
            SelectObject(mem, oldpen);
        }
        if (marks & 2) {            /* pin: a thick yellow box */
            oldpen = SelectObject(mem, pen_k);
            Rectangle(mem, pin[0] - v->scroll_x - 3, pin[1] - v->scroll_y - 3, pin[0] - v->scroll_x + pin[2] + 3,
                      pin[1] - v->scroll_y + pin[3] + 3);
            SelectObject(mem, pen_p);
            Rectangle(mem, pin[0] - v->scroll_x - 1, pin[1] - v->scroll_y - 1, pin[0] - v->scroll_x + pin[2] + 1,
                      pin[1] - v->scroll_y + pin[3] + 1);
            SelectObject(mem, oldpen);
        }
        SelectObject(mem, oldbr);
        DeleteObject(pen_h);
        DeleteObject(pen_p);
        DeleteObject(pen_k);
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    DeleteObject(bg);
    EndPaint(h, &ps);
}

/* Source applies to the Decoded view, Bank to its Patterns source */
static void enable_choosers(vidwin *v)
{
    const int decoded = msxvid_tab_view(v->tab) == MSXVID_V_DECODED;
    EnableWindow(v->source, decoded);
    EnableWindow(v->bank, decoded && msxvid_tab_source(v->tab) == MSXVID_SRC_PATTERNS);
}

static void update_pixel(vidwin *v)
{
    const msxvid_pixel *p = msxvid_tab_pixel(v->tab);
    set_text(v->pixel, p && p->text[0] ? p->text
                                       : "Move over the picture to inspect a pixel.\n"
                                         "Click to pin it; click again to unpin.\n"
                                         "Double-click to break on writes to what drew it.");
    if (v->cols) InvalidateRect(v->cols, NULL, FALSE);
    if (v->brk) EnableWindow(v->brk, msxvid_tab_can_break(v->tab));
}

static void set_note(vidwin *v, const char *text)
{
    set_text(v->note, text);
}

/* Break on writes to the RAM byte the inspected pixel came from, and say what was done. */
static void break_on_pixel(vidwin *v)
{
    char msg[160];
    int id = msxvid_tab_break_on_pixel(v->tab);
    if (id >= 0) snprintf(msg, sizeof msg, "Break on writes added (see Breakpoints)");
    else snprintf(msg, sizeof msg, "Nothing to break on: pin or hover a tile, a sprite, a bitmap pixel or a palette swatch");
    set_note(v, msg);
}

static LRESULT CALLBACK pic_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!v) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        pic_paint(v, h);
        return 0;
    case WM_SIZE:
        pic_update_scroll(v);
        return 0;
    case WM_HSCROLL:
    case WM_VSCROLL: {
        SCROLLINFO si;
        int bar = msg == WM_HSCROLL ? SB_HORZ : SB_VERT, pos;
        memset(&si, 0, sizeof si);
        si.cbSize = sizeof si;
        si.fMask = SIF_ALL;
        GetScrollInfo(h, bar, &si);
        pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINELEFT: pos -= 16; break;
        case SB_LINERIGHT: pos += 16; break;
        case SB_PAGELEFT: pos -= (int)si.nPage; break;
        case SB_PAGERIGHT: pos += (int)si.nPage; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        default: break;
        }
        if (pos < 0) pos = 0;
        if (bar == SB_HORZ) v->scroll_x = pos; else v->scroll_y = pos;
        pic_update_scroll(v);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        v->scroll_y -= GET_WHEEL_DELTA_WPARAM(wp) / 3;
        if (v->scroll_y < 0) v->scroll_y = 0;
        pic_update_scroll(v);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lp) + v->scroll_x, y = (short)HIWORD(lp) + v->scroll_y;
        if (!v->tracking) {
            TRACKMOUSEEVENT t;
            memset(&t, 0, sizeof t);
            t.cbSize = sizeof t;
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = h;
            TrackMouseEvent(&t);
            v->tracking = 1;
        }
        if (msxvid_tab_hover(v->tab, x, y)) {
            InvalidateRect(h, NULL, FALSE);
            update_pixel(v);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        v->tracking = 0;
        if (msxvid_tab_hover(v->tab, -1, -1)) {
            InvalidateRect(h, NULL, FALSE);
            update_pixel(v);
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(h);
        msxvid_tab_hover(v->tab, (short)LOWORD(lp) + v->scroll_x, (short)HIWORD(lp) + v->scroll_y);
        msxvid_tab_pin(v->tab, (short)LOWORD(lp) + v->scroll_x, (short)HIWORD(lp) + v->scroll_y);
        InvalidateRect(h, NULL, FALSE);
        update_pixel(v);
        return 0;
    case WM_LBUTTONDBLCLK:
        msxvid_tab_hover(v->tab, (short)LOWORD(lp) + v->scroll_x, (short)HIWORD(lp) + v->scroll_y);
        break_on_pixel(v);
        InvalidateRect(h, NULL, FALSE);
        update_pixel(v);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---- the Colours panel: the two colours as big swatches and a description ---- */

#define PAL_MAX_CELL 64

/* the grid's cell size, from the panel's size: 4 x 4 swatches, squared */
static void col_metrics(vidwin *v, const RECT *rc, int *cell, int *top)
{
    int cell_size = rc->right / PAL_COLS - S(v, 6);
    int by_height = (rc->bottom - S(v, 80)) / PAL_ROWS - S(v, 6);
    if (by_height < cell_size) cell_size = by_height;
    if (cell_size > S(v, PAL_MAX_CELL)) cell_size = S(v, PAL_MAX_CELL);
    if (cell_size < 12) cell_size = 12;
    *cell = cell_size;
    *top = 0;
}

/* "Medium green (2): #3EB849, 120 pixels, backdrop (R#7)" */
static void swatch_describe(const msxvid_swatch *swatch, char *out, size_t cap)
{
    snprintf(out, cap, "%s: #%06X  %ld pixels of the published frame%s%s%s%s", swatch->name, swatch->rgb & 0xFFFFFF, swatch->uses,
             swatch->role[0] ? "\nUsed as: " : "", swatch->role, swatch->changed ? "\nWritten to the palette this frame." : "",
             swatch->inspected ? "\nThe inspected pixel has it." : "");
}

static void col_paint(vidwin *v, HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps), mem;
    HBITMAP bmp, old;
    RECT rc;
    const msxvid_swatch *swatches;
    int count = msxvid_tab_swatches(v->tab, &swatches);
    int cell, top, index;
    HFONT oldf;
    GetClientRect(h, &rc);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
    old = SelectObject(mem, bmp);
    FillRect(mem, &rc, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(mem, TRANSPARENT);
    oldf = SelectObject(mem, v->mono);
    col_metrics(v, &rc, &cell, &top);
    for (index = 0; index < count && index < PAL_COLS * PAL_ROWS; index++) {
        const msxvid_swatch *swatch = &swatches[index];
        RECT cell_rect;
        HBRUSH fill = CreateSolidBrush(RGB((swatch->rgb >> 16) & 255, (swatch->rgb >> 8) & 255, swatch->rgb & 255));
        cell_rect.left = S(v, 3) + (index % PAL_COLS) * (cell + S(v, 6));
        cell_rect.top = top + S(v, 3) + (index / PAL_COLS) * (cell + S(v, 6));
        cell_rect.right = cell_rect.left + cell;
        cell_rect.bottom = cell_rect.top + cell;
        FillRect(mem, &cell_rect, fill);
        DeleteObject(fill);
        if (swatch->inspected) {
            HBRUSH accent_brush = CreateSolidBrush(ACCENT), white_brush = CreateSolidBrush(RGB(255, 255, 255));
            RECT outline = cell_rect;
            InflateRect(&outline, 1, 1);
            FrameRect(mem, &outline, accent_brush);
            InflateRect(&outline, -1, -1);
            FrameRect(mem, &outline, white_brush);
            DeleteObject(accent_brush);
            DeleteObject(white_brush);
        }
        if (index == v->pal_hover) {
            HBRUSH edge = CreateSolidBrush(RGB(0, 0, 0));
            RECT outline = cell_rect;
            InflateRect(&outline, 2, 2);
            FrameRect(mem, &outline, edge);
            DeleteObject(edge);
        }
    }
    {
        char text[300];
        RECT text_rect = { 0, top + PAL_ROWS * (cell + S(v, 6)) + S(v, 12), rc.right, rc.bottom };
        wchar_t *wide_text;
        int shown = v->pal_hover;
        text[0] = 0;
        if (shown < 0)
            for (index = 0; index < count; index++)
                if (swatches[index].inspected) shown = index;
        if (shown >= 0 && shown < count) swatch_describe(&swatches[shown], text, sizeof text);
        else snprintf(text, sizeof text, "The VDP's 16 colours. Hover a swatch for its use count and role.");
        wide_text = wide(text);
        SetTextColor(mem, GetSysColor(COLOR_WINDOWTEXT));
        if (wide_text) DrawTextW(mem, wide_text, -1, &text_rect, DT_WORDBREAK | DT_NOPREFIX);
        free(wide_text);
    }
    SelectObject(mem, oldf);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

/* the swatch under a point of the panel, or -1 */
static int col_hit(vidwin *v, HWND h, int px, int py)
{
    RECT rc;
    int cell, top, column, row, step;
    GetClientRect(h, &rc);
    col_metrics(v, &rc, &cell, &top);
    step = cell + S(v, 6);
    if (py < top + S(v, 3) || px < S(v, 3)) return -1;
    column = (px - S(v, 3)) / step;
    row = (py - top - S(v, 3)) / step;
    if (column >= PAL_COLS || row >= PAL_ROWS) return -1;
    if ((px - S(v, 3)) % step >= cell || (py - top - S(v, 3)) % step >= cell) return -1;
    return row * PAL_COLS + column;
}

static LRESULT CALLBACK col_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!v) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        col_paint(v, h);
        return 0;
    case WM_MOUSEMOVE: {
        int index = col_hit(v, h, (short)LOWORD(lp), (short)HIWORD(lp));
        if (!v->col_tracking) {
            TRACKMOUSEEVENT track;
            memset(&track, 0, sizeof track);
            track.cbSize = sizeof track;
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = h;
            TrackMouseEvent(&track);
            v->col_tracking = 1;
        }
        if (index != v->pal_hover) {
            v->pal_hover = index;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        v->col_tracking = 0;
        if (v->pal_hover != -1) {
            v->pal_hover = -1;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---- sections ----------------------------------------------------------------------- */

static void lv_cell(HWND lv, int row, int col, const char *text)
{
    wchar_t *w = wide(text);
    LVITEMW it;
    memset(&it, 0, sizeof it);
    it.iSubItem = col;
    it.pszText = w ? w : L"";
    if (col == 0) {
        it.mask = LVIF_TEXT;
        it.iItem = row;
        SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
    } else {
        SendMessageW(lv, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
    }
    free(w);
}

static void mark_hot(listfill *lf, int n, int i, int hot)
{
    if (i == 0) {
        free(lf->hot);
        lf->hot = calloc((size_t)(n > 0 ? n : 1), 1);
        lf->nhot = lf->hot ? n : 0;
    }
    if (lf->hot && i < lf->nhot) lf->hot[i] = (unsigned char)hot;
}

static void fill_regs(vidwin *v)
{
    const msxvid_reg *r;
    int n = msxvid_tab_regs(v->tab, &r), i;
    uint64_t sig = 1469598103934665603ull;
    char cell[32];
    for (i = 0; i < n; i++) {
        int hot = r[i].age < MSXVID_REG_FADE;
        sig = fnv(sig, r[i].name, strlen(r[i].name));
        sig = fnv(sig, r[i].value, strlen(r[i].value));
        sig = fnv(sig, r[i].meaning, strlen(r[i].meaning));
        sig = fnv(sig, &r[i].writes, sizeof r[i].writes);
        sig = fnv(sig, &hot, sizeof hot);
    }
    if (v->rf.have_sig && v->rf.sig == sig) return;
    v->rf.sig = sig;
    v->rf.have_sig = 1;
    SendMessageW(v->regs, WM_SETREDRAW, FALSE, 0);
    SendMessageW(v->regs, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; i < n; i++) {
        lv_cell(v->regs, i, 0, r[i].name);
        lv_cell(v->regs, i, 1, r[i].value);
        snprintf(cell, sizeof cell, "%d", r[i].writes);
        lv_cell(v->regs, i, 2, cell);
        if (r[i].first_line >= 0) snprintf(cell, sizeof cell, "%d", r[i].first_line);
        else snprintf(cell, sizeof cell, "-");
        lv_cell(v->regs, i, 3, cell);
        lv_cell(v->regs, i, 4, r[i].meaning);
        mark_hot(&v->rf, n, i, r[i].age < MSXVID_REG_FADE);
    }
    if (n == 0) mark_hot(&v->rf, 0, 0, 0);
    SendMessageW(v->regs, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(v->regs, NULL, TRUE);
}

static void fill_lines(vidwin *v)
{
    const msxvid_band *r;
    int n = msxvid_tab_lines(v->tab, &r), i;
    uint64_t sig = 1469598103934665603ull;
    for (i = 0; i < n; i++) {
        sig = fnv(sig, &r[i], sizeof r[i]);
    }
    sig = fnv(sig, &n, sizeof n);
    if (v->lf.have_sig && v->lf.sig == sig) return;
    v->lf.sig = sig;
    v->lf.have_sig = 1;
    SendMessageW(v->lines, WM_SETREDRAW, FALSE, 0);
    SendMessageW(v->lines, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; i < n; i++) {
        char text[200];
        snprintf(text, sizeof text, "%d-%d", r[i].first, r[i].last);
        lv_cell(v->lines, i, 0, text);
        snprintf(text, sizeof text, "%d", r[i].abs_first);
        lv_cell(v->lines, i, 1, text);
        lv_cell(v->lines, i, 2, r[i].mode);
        snprintf(text, sizeof text, "%04X", r[i].name_table);
        lv_cell(v->lines, i, 3, text);
        snprintf(text, sizeof text, "%04X", r[i].pattern_table);
        lv_cell(v->lines, i, 4, text);
        snprintf(text, sizeof text, "%04X", r[i].colour_table);
        lv_cell(v->lines, i, 5, text);
        snprintf(text, sizeof text, "%04X / %04X", r[i].sprite_attr, r[i].sprite_pattern);
        lv_cell(v->lines, i, 6, text);
        snprintf(text, sizeof text, "%d", r[i].backdrop);
        lv_cell(v->lines, i, 7, text);
        snprintf(text, sizeof text, "%d", r[i].scroll_y);
        lv_cell(v->lines, i, 8, text);
        snprintf(text, sizeof text, "%d", r[i].writes);
        lv_cell(v->lines, i, 9, text);
        mark_hot(&v->lf, n, i, r[i].writes > 0);
    }
    if (n == 0) mark_hot(&v->lf, 0, 0, 0);
    SendMessageW(v->lines, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(v->lines, NULL, TRUE);
}

/* the VRAM section: sixteen rows of the physical VRAM from the controller's base */
static void fill_vram(vidwin *v)
{
    static char text[4096];
    msxvid_tab_text(v->tab, MSXVID_S_VRAM, text, (int)sizeof text);
    set_text(v->vram_view, text);
    {
        char note[64];
        snprintf(note, sizeof note, "$%05X", (unsigned)msxvid_tab_vram_base(v->tab));
        if (!v->vram_note_set) set_text(v->vram_note, note);
    }
}

/* the address box: an address alone scrolls the dump there, an address
 * followed by hex bytes writes them first (the old VDP tab's one field) */
static void vram_accept(vidwin *v)
{
    uint8_t bytes[POKE_MAX];
    uint32_t vaddr;
    char buf[256], msg[96];
    int n;
    GetWindowTextA(v->vram_addr, buf, sizeof buf);
    n = msxvdp_parse_poke(buf, &vaddr, bytes, POKE_MAX);
    if (n < 0) {
        set_text(v->vram_note, "expected: addr [bytes...], e.g. 1800: 41 42");
        v->vram_note_set = 1;
        return;
    }
    if (n > 0) {
        msxdebug_vdp_write(v->engine, vaddr, bytes, n);
        snprintf(msg, sizeof msg, "wrote %d byte%s at $%05X", n, n == 1 ? "" : "s", (unsigned)vaddr);
        SetWindowTextA(v->vram_addr, "");
    } else {
        snprintf(msg, sizeof msg, "$%05X", (unsigned)vaddr);
    }
    set_text(v->vram_note, msg);
    v->vram_note_set = 1;
    msxvid_tab_set_vram_base(v->tab, vaddr);
    dbg_video_refresh(v->page, 1);
}

static void fill_timing(vidwin *v)
{
    const char *const *lines;
    int n = msxvid_tab_timing(v->tab, &lines), i;
    char buf[2048];
    size_t len = 0;
    buf[0] = 0;
    for (i = 0; i < n && len < sizeof buf - 160; i++) {
        const char *tab = strchr(lines[i], '\t');
        if (tab) len += (size_t)snprintf(buf + len, sizeof buf - len, "%-14.*s %s\n", (int)(tab - lines[i]), lines[i], tab + 1);
        else len += (size_t)snprintf(buf + len, sizeof buf - len, "%s\n", lines[i]);
    }
    if (!n) snprintf(buf, sizeof buf, "No machine");
    set_text(v->timing, buf);
    SendMessageW(v->beam, PBM_SETPOS, (WPARAM)msxvid_tab_beam_permille(v->tab), 0);
}

static int cur_section(vidwin *v)
{
    int s = (int)SendMessageW(v->sec, TCM_GETCURSEL, 0, 0);
    return s < 0 ? 0 : s;
}

static void show_section(vidwin *v)
{
    int s = cur_section(v);
    ShowWindow(v->pixel, s == MSXVID_S_PIXEL ? SW_SHOW : SW_HIDE);
    ShowWindow(v->unpin, s == MSXVID_S_PIXEL ? SW_SHOW : SW_HIDE);
    ShowWindow(v->regs, s == MSXVID_S_REGISTERS ? SW_SHOW : SW_HIDE);
    ShowWindow(v->cols, s == MSXVID_S_COLOURS ? SW_SHOW : SW_HIDE);
    ShowWindow(v->lines, s == MSXVID_S_LINES ? SW_SHOW : SW_HIDE);
    ShowWindow(v->beam, s == MSXVID_S_TIMING ? SW_SHOW : SW_HIDE);
    ShowWindow(v->timing, s == MSXVID_S_TIMING ? SW_SHOW : SW_HIDE);
    ShowWindow(v->vram_title, s == MSXVID_S_VRAM ? SW_SHOW : SW_HIDE);
    ShowWindow(v->vram_addr, s == MSXVID_S_VRAM ? SW_SHOW : SW_HIDE);
    ShowWindow(v->vram_prev, s == MSXVID_S_VRAM ? SW_SHOW : SW_HIDE);
    ShowWindow(v->vram_next, s == MSXVID_S_VRAM ? SW_SHOW : SW_HIDE);
    ShowWindow(v->vram_note, s == MSXVID_S_VRAM ? SW_SHOW : SW_HIDE);
    ShowWindow(v->vram_view, s == MSXVID_S_VRAM ? SW_SHOW : SW_HIDE);
}

/* ---- refresh -------------------------------------------------------------------------- */

#define R_RESET 0x8000u         /* the view or zoom changed: scroll back to the corner */

static void refresh_view(vidwin *v, unsigned what)
{
    if (what & (MSXVID_TAB_PICTURE | MSXVID_TAB_OFF)) {
        RECT rc;
        GetClientRect(v->pic, &rc);
        if (rc.right > 4 && rc.bottom > 4) msxvid_tab_set_viewport(v->tab, rc.right - 2, rc.bottom - 2);
        if (what & R_RESET) v->scroll_x = v->scroll_y = 0;
        pic_update_scroll(v);
        InvalidateRect(v->pic, NULL, FALSE);
    }
    if (what & MSXVID_TAB_TABLES) {
        set_text(v->status, msxvid_tab_status(v->tab));
        switch (cur_section(v)) {
        case MSXVID_S_REGISTERS: fill_regs(v); break;
        case MSXVID_S_COLOURS: InvalidateRect(v->cols, NULL, FALSE); break;
        case MSXVID_S_LINES: fill_lines(v); break;
        case MSXVID_S_TIMING: fill_timing(v); break;
        case MSXVID_S_VRAM: fill_vram(v); break;
        default: break;
        }
        update_pixel(v);
    }
}

void dbg_video_refresh(HWND page, int force)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(page, GWLP_USERDATA);
    unsigned what;
    if (!v) return;
    what = msxvid_tab_refresh(v->tab, force);
    if (force) {
        what |= MSXVID_TAB_PICTURE | MSXVID_TAB_TABLES;
        v->rf.have_sig = v->lf.have_sig = 0;
    }
    refresh_view(v, what);
}

/* ---- layout --------------------------------------------------------------------------- */

static int text_w(vidwin *v, HFONT f, const wchar_t *s)
{
    HDC dc = GetDC(v->page);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz;
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, old);
    ReleaseDC(v->page, dc);
    return sz.cx;
}

static void layout(vidwin *v)
{
    RECT rc, tr;
    int W, H, rw, lw, x, y, i, row_h = S(v, 24), lh = S(v, 18), gap = S(v, 6), mono_h;
    int right_x, tab_h, top;
    TEXTMETRICW tm;
    HDC dc;
    GetClientRect(v->page, &rc);
    W = rc.right;
    H = rc.bottom;
    if (W < 100 || H < 100) return;
    dc = GetDC(v->page);
    SelectObject(dc, v->mono);
    mono_h = GetTextMetricsW(dc, &tm) ? tm.tmHeight : S(v, 15);
    ReleaseDC(v->page, dc);

    /* toolbar: View, Zoom, Break on writes; then the overlay checkboxes, flowing */
    x = 0;
    y = 0;
    MoveWindow(v->label_view, x, y + (row_h - lh) / 2, S(v, 36), lh, TRUE);
    x += S(v, 36);
    MoveWindow(v->view, x, y, S(v, 110), S(v, 300), TRUE);
    x += S(v, 110) + gap * 2;
    MoveWindow(v->label_src, x, y + (row_h - lh) / 2, S(v, 50), lh, TRUE);
    x += S(v, 50);
    MoveWindow(v->source, x, y, S(v, 100), S(v, 300), TRUE);
    x += S(v, 100) + gap * 2;
    MoveWindow(v->label_bank, x, y + (row_h - lh) / 2, S(v, 36), lh, TRUE);
    x += S(v, 36);
    MoveWindow(v->bank, x, y, S(v, 70), S(v, 300), TRUE);
    x += S(v, 70) + gap * 2;
    MoveWindow(v->label_zoom, x, y + (row_h - lh) / 2, S(v, 40), lh, TRUE);
    x += S(v, 40);
    MoveWindow(v->zoom, x, y, S(v, 80), S(v, 300), TRUE);
    x += S(v, 80) + gap * 3;
    {
        int bw = text_w(v, v->ui, L"Break on writes") + S(v, 28);
        MoveWindow(v->brk, x, y - S(v, 1), bw, row_h, TRUE);
    }
    y += row_h + gap;
    x = 0;
    for (i = 0; i < v->nov; i++) {
        char dummy[1];
        wchar_t lbl[64];
        int bw;
        (void)dummy;
        GetWindowTextW(v->ov[i], lbl, 64);
        bw = text_w(v, v->ui, lbl) + S(v, 26);
        if (x + bw > W && x > 0) {
            x = 0;
            y += row_h - S(v, 2);
        }
        MoveWindow(v->ov[i], x, y, bw, row_h - S(v, 4), TRUE);
        x += bw + gap;
    }
    y += row_h;
    MoveWindow(v->status, 0, y, W, mono_h * 2 + S(v, 4), TRUE);
    y += mono_h * 2 + S(v, 8);
    top = y;

    /* the split: picture left, sections right */
    rw = S(v, 480);
    if (rw > W * 50 / 100) rw = W * 48 / 100;
    lw = W - rw - gap;
    right_x = lw + gap;
    MoveWindow(v->pic, 0, top, lw, H - top - lh - gap > 20 ? H - top - lh - gap : 20, TRUE);
    MoveWindow(v->note, 0, H - lh, W, lh, TRUE);

    MoveWindow(v->sec, right_x, top, rw, H - top - lh - gap, TRUE);
    SetRect(&tr, 0, 0, rw, H - top - lh - gap);
    SendMessageW(v->sec, TCM_ADJUSTRECT, FALSE, (LPARAM)&tr);
    tab_h = tr.top;
    {
        int cx = right_x + S(v, 6), cy = top + tab_h + S(v, 6), cw = rw - S(v, 12);
        int ch = (H - lh - gap) - cy - S(v, 6);
        MoveWindow(v->pixel, cx, cy, cw, ch - S(v, 34) > 20 ? ch - S(v, 34) : 20, TRUE);
        MoveWindow(v->unpin, cx, cy + (ch > S(v, 60) ? ch - S(v, 28) : S(v, 32)), S(v, 80), S(v, 26), TRUE);
        MoveWindow(v->regs, cx, cy, cw, ch, TRUE);
        MoveWindow(v->cols, cx, cy, cw, ch, TRUE);
        MoveWindow(v->lines, cx, cy, cw, ch, TRUE);
        MoveWindow(v->beam, cx, cy, cw, S(v, 20), TRUE);
        MoveWindow(v->timing, cx, cy + S(v, 30), cw, ch - S(v, 30), TRUE);
        {
            const int bw = S(v, 32), ah = S(v, 24);
            MoveWindow(v->vram_title, cx, cy, cw, lh, TRUE);
            MoveWindow(v->vram_addr, cx, cy + lh + S(v, 2), cw - 2 * bw - S(v, 140) - 3 * gap, ah, TRUE);
            MoveWindow(v->vram_prev, cx + cw - 2 * bw - S(v, 140) - 2 * gap, cy + lh + S(v, 1), bw, ah + S(v, 2), TRUE);
            MoveWindow(v->vram_next, cx + cw - bw - S(v, 140) - gap, cy + lh + S(v, 1), bw, ah + S(v, 2), TRUE);
            MoveWindow(v->vram_note, cx + cw - S(v, 140), cy + lh + S(v, 5), S(v, 140), lh, TRUE);
            MoveWindow(v->vram_view, cx, cy + lh + ah + S(v, 8), cw, ch - lh - ah - S(v, 8), TRUE);
        }
        {
            static const int rw_[5] = { 100, 70, 56, 52, 520 };
            for (i = 0; i < 5; i++) SendMessageW(v->regs, LVM_SETCOLUMNWIDTH, (WPARAM)i, S(v, rw_[i]));
        }
        {
            static const int lw_[10] = { 64, 52, 100, 56, 70, 66, 112, 64, 56, 56 };
            for (i = 0; i < 10; i++) SendMessageW(v->lines, LVM_SETCOLUMNWIDTH, (WPARAM)i, S(v, lw_[i]));
        }
    }
    pic_update_scroll(v);
}

/* ---- the page -------------------------------------------------------------------------- */

static HWND mk_in(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int id, HFONT font)
{
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                             (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), NULL);
    if (font) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

#define mk(v, cls, text, style, id, font) mk_in((v)->page, cls, text, style, id, font)

static void on_overlay(vidwin *v)
{
    unsigned m = 0;
    int i;
    for (i = 0; i < v->nov; i++)
        if (SendMessageW(v->ov[i], BM_GETCHECK, 0, 0) == BST_CHECKED) m |= v->ov_bit[i];
    msxvid_tab_set_overlays(v->tab, m);
    refresh_view(v, MSXVID_TAB_PICTURE | MSXVID_TAB_TABLES);
}

/* Enter in the VRAM address box applies it (to the page, which owns the controller). */
static WNDPROC g_vram_proc;

static LRESULT CALLBACK vram_edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageW(GetParent(h), WM_APP + 1, 0, 0);
        return 0;
    }
    if (msg == WM_CHAR && (wp == L'\r' || wp == L'\n')) return 0;
    return CallWindowProcW(g_vram_proc, h, msg, wp, lp);
}

static LRESULT CALLBACK page_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!v) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_SIZE:
        layout(v);
        refresh_view(v, MSXVID_TAB_PICTURE);
        return 0;
    case WM_APP + 1:
        vram_accept(v);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (v->updating) return 0;
        if (id == IDV_VIEW && code == CBN_SELCHANGE) {
            msxvid_tab_set_view(v->tab, (int)SendMessageW(v->view, CB_GETCURSEL, 0, 0));
            dbg_video_refresh(h, 1);
            v->scroll_x = v->scroll_y = 0;
            pic_update_scroll(v);
            enable_choosers(v);
        } else if (id == IDV_SOURCE && code == CBN_SELCHANGE) {
            msxvid_tab_set_source(v->tab, (int)SendMessageW(v->source, CB_GETCURSEL, 0, 0));
            dbg_video_refresh(h, 1);
            v->scroll_x = v->scroll_y = 0;
            pic_update_scroll(v);
            enable_choosers(v);
        } else if (id == IDV_BANK && code == CBN_SELCHANGE) {
            msxvid_tab_set_bank(v->tab, (int)SendMessageW(v->bank, CB_GETCURSEL, 0, 0));
            dbg_video_refresh(h, 1);
        } else if (id == IDV_VRAM_PREV || id == IDV_VRAM_NEXT) {
            const uint32_t base = msxvid_tab_vram_base(v->tab);
            msxvid_tab_set_vram_base(v->tab, id == IDV_VRAM_NEXT ? base + VRAM_STEP : base - VRAM_STEP);
            v->vram_note_set = 0;
            dbg_video_refresh(h, 1);
        } else if (id == IDV_ZOOM && code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(v->zoom, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < v->nzoom) msxvid_tab_set_zoom(v->tab, v->zoom_val[i]);
            refresh_view(v, MSXVID_TAB_PICTURE | R_RESET);
        } else if (id >= IDV_OV0 && id < IDV_OV0 + v->nov && code == BN_CLICKED) {
            on_overlay(v);
        } else if (id == IDV_BREAK && code == BN_CLICKED) {
            break_on_pixel(v);
        } else if (id == IDV_UNPIN && code == BN_CLICKED) {
            msxvid_tab_unpin(v->tab);
            InvalidateRect(v->pic, NULL, FALSE);
            update_pixel(v);
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == IDV_SEC && nh->code == TCN_SELCHANGE) {
            show_section(v);
            refresh_view(v, MSXVID_TAB_TABLES);
        } else if ((nh->idFrom == IDV_REGS || nh->idFrom == IDV_LINES) && nh->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lp;
            listfill *lf = nh->idFrom == IDV_REGS ? &v->rf : &v->lf;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                int i = (int)cd->nmcd.dwItemSpec;
                if (i >= 0 && i < lf->nhot && lf->hot[i]) {
                    if (nh->idFrom == IDV_REGS) {       /* a register that just changed: the accent */
                        cd->clrTextBk = ACCENT;
                        cd->clrText = ACCENT_INK;
                    } else {                            /* a line where something changed */
                        cd->clrTextBk = RGB(0xDD, 0xEE, 0xD5);
                        cd->clrText = RGB(0, 0, 0);
                    }
                }
                return CDRF_DODEFAULT;
            }
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        /* the read-only labels sit on the page colour */
        HDC dc = (HDC)wp;
        if ((HWND)lp == v->vram_view) {      /* the hex dump reads on the window colour, like the other text views */
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_DESTROY:
        msxvid_tab_free(v->tab);
        free(v->rf.hot);
        free(v->lf.hot);
        free(v);
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void register_classes(HINSTANCE inst)
{
    static int done;
    WNDCLASSW wc;
    if (done) return;
    done = 1;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = page_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = PAGE_CLASS;
    RegisterClassW(&wc);
    wc.lpfnWndProc = pic_proc;
    wc.lpszClassName = PIC_CLASS;
    wc.hbrBackground = NULL;
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    wc.style = CS_DBLCLKS;
    RegisterClassW(&wc);
    wc.lpfnWndProc = col_proc;
    wc.lpszClassName = COL_CLASS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.style = 0;
    RegisterClassW(&wc);
}

static HWND mk_list(vidwin *v, int id, const wchar_t *const *names, int n)
{
    HWND lv = mk(v, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
                 id, v->mono);
    LVCOLUMNW col;
    int i;
    SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                 LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    memset(&col, 0, sizeof col);
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    for (i = 0; i < n; i++) {
        col.pszText = (LPWSTR)names[i];
        col.cx = 80;
        SendMessageW(lv, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
    }
    return lv;
}

HWND dbg_video_create(HWND parent, msxdebug *engine, HFONT ui, HFONT mono, int dpi)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    vidwin *v = calloc(1, sizeof *v);
    int n, i;
    const msxvid_overlay_info *od;
    const int *zv;
    TCITEMW item;
    static const wchar_t *const regcols[5] = { L"Register", L"Value", L"Writes", L"Line", L"Meaning" };
    static const wchar_t *const linecols[10] = { L"Lines", L"Frame", L"Mode", L"Names", L"Patterns", L"Colours", L"Sprites",
                                                 L"Backdrop", L"Scroll", L"Writes" };
    if (!v) return NULL;
    register_classes(inst);
    v->ui = ui;
    v->mono = mono;
    v->engine = engine;
    v->dpi = dpi > 0 ? dpi : 96;
    v->pal_hover = -1;
    v->tab = msxvid_tab_new(engine);
    if (!v->tab) {
        free(v);
        return NULL;
    }
    memset(&v->bmi, 0, sizeof v->bmi);
    v->bmi.bmiHeader.biSize = sizeof v->bmi.bmiHeader;
    v->bmi.bmiHeader.biPlanes = 1;
    v->bmi.bmiHeader.biBitCount = 32;
    v->bmi.bmiHeader.biCompression = BI_RGB;
    v->page = CreateWindowExW(0, PAGE_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent,
                              NULL, inst, NULL);
    SetWindowLongPtrW(v->page, GWLP_USERDATA, (LONG_PTR)v);

    v->label_view = mk(v, L"STATIC", L"View", SS_LEFT, IDV_LABEL_VIEW, ui);
    v->view = mk(v, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_VIEW, ui);
    for (i = 0; i < MSXVID_V_COUNT; i++) {
        wchar_t *w = wide(msxvid_view_name(i));
        SendMessageW(v->view, CB_ADDSTRING, 0, (LPARAM)w);
        free(w);
    }
    v->label_src = mk(v, L"STATIC", L"Source", SS_LEFT, IDV_LABEL_SRC, ui);
    v->source = mk(v, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_SOURCE, ui);
    for (i = 0; i < MSXVID_SRC_COUNT; i++) {
        wchar_t *w = wide(msxvid_source_name(i));
        SendMessageW(v->source, CB_ADDSTRING, 0, (LPARAM)w);
        free(w);
    }
    v->label_bank = mk(v, L"STATIC", L"Bank", SS_LEFT, IDV_LABEL_BANK, ui);
    v->bank = mk(v, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_BANK, ui);
    SendMessageW(v->bank, CB_ADDSTRING, 0, (LPARAM)L"0");
    SendMessageW(v->bank, CB_ADDSTRING, 0, (LPARAM)L"1");
    SendMessageW(v->bank, CB_ADDSTRING, 0, (LPARAM)L"2");
    v->label_zoom = mk(v, L"STATIC", L"Zoom", SS_LEFT, IDV_LABEL_ZOOM, ui);
    v->zoom = mk(v, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_ZOOM, ui);
    zv = msxvid_zooms(&n);
    for (i = 0; i < n && i < MAX_ZOOMS; i++) {
        wchar_t lbl[16];
        v->zoom_val[v->nzoom++] = zv[i];
        if (zv[i] == 0) wcscpy(lbl, L"Fit");
        else swprintf(lbl, 16, L"%d\u00d7", zv[i]);
        SendMessageW(v->zoom, CB_ADDSTRING, 0, (LPARAM)lbl);
    }
    v->brk = mk(v, L"BUTTON", L"Break on writes", BS_PUSHBUTTON | WS_TABSTOP, IDV_BREAK, ui);
    od = msxvid_overlays(&n);
    for (i = 0; i < n && i < MAX_OVERLAYS; i++) {
        wchar_t *w = wide(od[i].label);
        v->ov[i] = mk(v, L"BUTTON", w, BS_AUTOCHECKBOX | WS_TABSTOP, IDV_OV0 + i, ui);
        v->ov_bit[i] = od[i].bit;
        v->nov++;
        free(w);
    }
    v->status = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_STATUS, mono);
    v->pic = CreateWindowExW(WS_EX_CLIENTEDGE, PIC_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL, 0, 0, 10, 10,
                             v->page, (HMENU)(INT_PTR)IDV_PIC, inst, NULL);
    SetWindowLongPtrW(v->pic, GWLP_USERDATA, (LONG_PTR)v);
    v->note = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDV_NOTE, ui);

    v->sec = mk(v, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP, IDV_SEC, ui);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (i = 0; i < MSXVID_S_COUNT; i++) {
        wchar_t *w = wide(msxvid_section_name(i));
        item.pszText = w;
        SendMessageW(v->sec, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&item);
        free(w);
    }
    SetWindowPos(v->sec, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   /* under the section controls */
    v->pixel = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_PIXEL, mono);
    v->unpin = mk(v, L"BUTTON", L"Unpin", BS_PUSHBUTTON | WS_TABSTOP, IDV_UNPIN, ui);
    v->regs = mk_list(v, IDV_REGS, regcols, 5);
    v->lines = mk_list(v, IDV_LINES, linecols, 10);
    v->cols = CreateWindowExW(0, COL_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, v->page,
                              (HMENU)(INT_PTR)IDV_COLS, inst, NULL);
    SetWindowLongPtrW(v->cols, GWLP_USERDATA, (LONG_PTR)v);
    v->beam = mk(v, PROGRESS_CLASSW, L"", PBS_SMOOTH, IDV_BEAM, NULL);
    SendMessageW(v->beam, PBM_SETRANGE32, 0, 1000);
    v->timing = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_TIMING, mono);
    v->vram_title = mk(v, L"STATIC", L"Physical VRAM (Enter writes; addr [bytes...])", SS_LEFT | SS_NOPREFIX, IDV_VRAM_TITLE, ui);
    v->vram_addr = mk(v, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, IDV_VRAM_ADDR, mono);
    SendMessageW(v->vram_addr, EM_SETCUEBANNER, TRUE, (LPARAM)L"1800: 41 42");
    g_vram_proc = (WNDPROC)SetWindowLongPtrW(v->vram_addr, GWLP_WNDPROC, (LONG_PTR)vram_edit_proc);
    v->vram_prev = mk(v, L"BUTTON", L"<", BS_PUSHBUTTON | WS_TABSTOP, IDV_VRAM_PREV, ui);
    v->vram_next = mk(v, L"BUTTON", L">", BS_PUSHBUTTON | WS_TABSTOP, IDV_VRAM_NEXT, ui);
    v->vram_note = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDV_VRAM_NOTE, ui);
    v->vram_view = mk(v, L"EDIT", L"", WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                      IDV_VRAM_VIEW, mono);

    /* start from the controller's state (the MSX_DEBUGGER_VIDEO_* variables) */
    v->updating = 1;
    SendMessageW(v->view, CB_SETCURSEL, (WPARAM)msxvid_tab_view(v->tab), 0);
    SendMessageW(v->source, CB_SETCURSEL, (WPARAM)msxvid_tab_source(v->tab), 0);
    SendMessageW(v->bank, CB_SETCURSEL, (WPARAM)msxvid_tab_bank(v->tab), 0);
    enable_choosers(v);
    for (i = 0; i < v->nzoom; i++)
        if (v->zoom_val[i] == msxvid_tab_zoom(v->tab)) SendMessageW(v->zoom, CB_SETCURSEL, (WPARAM)i, 0);
    for (i = 0; i < v->nov; i++)
        SendMessageW(v->ov[i], BM_SETCHECK, (msxvid_tab_overlays(v->tab) & v->ov_bit[i]) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(v->sec, TCM_SETCURSEL, (WPARAM)msxvid_tab_env_section(v->tab), 0);
    v->updating = 0;
    show_section(v);
    return v->page;
}

void dbg_video_place(HWND page, int x, int y, int w, int h)
{
    MoveWindow(page, x, y, w > 1 ? w : 1, h > 1 ? h : 1, TRUE);
}

void dbg_video_fonts(HWND page, HFONT ui, HFONT mono, int dpi)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(page, GWLP_USERDATA);
    if (!v) return;
    v->ui = ui;
    v->mono = mono;
    v->dpi = dpi > 0 ? dpi : 96;
    layout(v);
    InvalidateRect(v->page, NULL, TRUE);
}
