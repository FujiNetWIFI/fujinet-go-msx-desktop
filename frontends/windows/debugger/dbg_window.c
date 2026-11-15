/*
 * Debugger window (Win32) over the Z80 / VDP debugger engine, via
 * core/include/msxdebug.h. Twelve tabs, the family's: Console (the engine's
 * prompt), Disassembly, Memory (bytes through the slot map, with Go to, a
 * search and a Write bar), Breakpoints (Execute / Read / Write / Access / In /
 * Out, expression conditions), Watches, Cart & I/O, Trace, History, Video (the
 * shared controller in core/include/msxvidtab.h, drawn by dbg_video.c; the old
 * VDP panes are its Decoded sources and its VRAM section), Sound (PSG, SCC,
 * MSX-MUSIC, MSX-AUDIO), Symbols and FujiNet. An editable register strip sits
 * under every tab (Enter applies, only while stopped). Built from plain common
 * controls, with the disassembly and the Video picture drawn by hand.
 *
 * The engine only engages while this window is showing: showing it attaches
 * (which stops the machine, as on every sibling) and hiding it (the close
 * box, F12, or the main window's F12 / View menu item, which toggles it)
 * detaches, which lets the machine run on at full speed. The toolbar carries
 * a single Stop/Run button, the stepping controls (Step, Step Over, Step Out,
 * Run to Cursor, Frame = run to the next VDP interrupt), symbol loading, the
 * save menu and Pop Out; F5 F7 F8 Shift+F8 Shift+F7 F9 F12 work wherever the
 * focus is inside the window (msx_debugger_pretranslate).
 *
 * Each tab's controls are children of a container of their own, so Pop Out
 * can peel the tab off into a window (SetParent) and Dock put it back.
 *
 * A 100 ms timer polls the engine (msxdebug_generation and the stop
 * snapshot, never a stop callback, which fires off the UI thread) and
 * refreshes the tabs showing, and only those, when something changed; while
 * the machine runs it refreshes the live values of those tabs twice a second.
 * Nothing is greyed while the machine runs: a click in the disassembly
 * selects a row, a register edit says "stop the machine first" in the status
 * line. After a step the window waits a few milliseconds for the stop to
 * settle so the status does not flash "Running".
 *
 * Environment hooks, for captures and tests: MSX_DEBUGGER_TAB (a tab number,
 * or the start of a tab's name or label, in any case; the old "cpu", "vdp"
 * and "io" still work), MSX_WATCH ("x,y": watches), MSX_WATCH_BREAK (a
 * write-when-changed breakpoint on a range), MSX_MEM_GOTO (show a range in
 * Memory), MSX_HISTORY (record from the start), MSX_POPOUT (a tab, as for
 * MSX_DEBUGGER_TAB) and the Video tab's MSX_DEBUGGER_VIDEO_* (read by
 * msxvid_tab_new).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accent.h"
#include "dbg_video.h"
#include "msxdebug.h"
#include "msxmemtools.h"

#include "resource.h"
#include "../help/help_window.h"

#define DISASM_MAX 96        /* rows fetched at most; the view shows what fits */
#define MEM_ROWS 256         /* 4K of memory, 16 bytes a row */
#define MEM_BYTES (MEM_ROWS * 16)
#define MAX_BPS 128
#define HISTORY_MAX 64
#define TRACE_MAX 512        /* FujiNet packets shown */
#define COMMENT_COL 66       /* where a disassembly line's comment starts, in characters */
#define CONSOLE_KEEP (256 * 1024)
#define NOTICE_MS 4000       /* how long a notice holds the status line */
#define HIST_SHOWN 400       /* lines on the Trace and History tabs */
#define HIST_COPY 4000       /* entries read to find them */
#define SYM_MAX 4096         /* symbols listed at most */

/* Sizes at 96 dpi; S() scales them to the window's. */
#define GUTTER_W 18          /* the disassembly's breakpoint column */
#define MONO_PX 13           /* the monospace font */

/* The icon's blue (core/include/msxsession.h); the PC line and the stopped
 * Run button wear it, with the ink on it. */
#define ACCENT_RGB MSX_ACCENT_COLORREF
#define INK_RGB MSX_ACCENT_INK_COLORREF

#define WM_DBG_ACCEPT  (WM_APP + 2)   /* wp: control id -- Enter in an edit */
#define WM_DBG_TAB     (WM_APP + 3)   /* Tab in the console: complete */
#define WM_DBG_HISTORY (WM_APP + 4)   /* wp: 0 Up (older), 1 Down (newer) in the console */
#define WM_DBG_SYNC    (WM_APP + 5)   /* resync once a list's notification is over */
#define WM_DBG_GOTO    (WM_APP + 6)   /* wp: an address to show in Memory (a symbol was clicked) */

#define TIMER_REFRESH 1

#define DBG_CLASS "FujiNetGoMSXDebugger"
#define DISASM_CLASS "FujiNetGoMSXDisasm"
#define PAGE_CLASS "FujiNetGoMSXDbgPage"
#define POP_CLASS "FujiNetGoMSXDbgPop"

enum {
    TAB_CONSOLE, TAB_DISASM, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_CART, TAB_TRACE, TAB_HISTORY,
    TAB_VIDEO, TAB_SOUND, TAB_SYMBOLS, TAB_FUJINET, TAB_COUNT
};

/* The tabs, in order: the names MSX_DEBUGGER_TAB takes, and their labels
 * (which it takes too). */
static const struct { const char *name, *label; } k_tabs[TAB_COUNT] = {
    { "console", "Console" }, { "disassembly", "Disassembly" }, { "memory", "Memory" },
    { "breakpoints", "Breakpoints" }, { "watches", "Watches" }, { "cart", "Cart & I/O" },
    { "trace", "Trace" }, { "history", "History" }, { "video", "Video" }, { "sound", "Sound" },
    { "symbols", "Symbols" }, { "fujinet", "FujiNet" },
};

/* The tabs before there were twelve: what they became. */
static const struct { const char *name; int tab; } k_tab_aliases[] = {
    { "cpu", TAB_MEMORY }, { "registers", TAB_MEMORY }, { "vdp", TAB_VIDEO }, { "io", TAB_CART },
    { "i/o", TAB_CART }, { "slots", TAB_CART },
};

#define NREGS 15            /* the editable registers */
#define NCHECKS 8           /* six flags and the two interrupt flip-flops */
#define NSAVE 4
#define NBPTYPES 6

enum {
    IDC_RUN = 1000, IDC_STEP, IDC_OVER, IDC_OUT, IDC_RUN_TO, IDC_FRAME, IDC_SYMBOLS, IDC_SAVE,
    IDC_STATUS, IDC_TABS, IDC_TOGGLE_BP, IDC_HELP_BTN, IDC_POPOUT, IDC_DOCK,
    IDC_CON_OUT, IDC_CON_PROMPT, IDC_CON_IN,
    IDC_REG0, IDC_REG_LAST = IDC_REG0 + NREGS - 1,
    IDC_CHK0, IDC_CHK_LAST = IDC_CHK0 + NCHECKS - 1,
    IDC_MEM_VIEW, IDC_MEM_GOTO, IDC_MEM_BACK, IDC_MEM_ON, IDC_MEM_WRITE,
    IDC_SRCH_KIND, IDC_SRCH_ENTRY, IDC_SRCH_NEXT, IDC_SRCH_PREV,
    IDC_FOLLOW_PC, IDC_JUMP, IDC_DISASM,
    IDC_BP_LIST, IDC_BP_REMOVE, IDC_BP_CLEAR, IDC_BP_HITS, IDC_BP_TYPE, IDC_BP_START, IDC_BP_END,
    IDC_BP_COND, IDC_BP_ADD,
    IDC_WT_ENTRY, IDC_WT_ADD, IDC_WT_LIST, IDC_WT_MEM, IDC_WT_BRK, IDC_WT_REMOVE,
    IDC_CART_TEXT, IDC_IO_TEXT, IDC_SOUND_TEXT,
    IDC_H_REC0, IDC_H_REC1, IDC_H_FILTER0, IDC_H_FILTER1, IDC_H_RANGE0, IDC_H_RANGE1,
    IDC_H_CLEAR0, IDC_H_CLEAR1, IDC_H_NOTE0, IDC_H_NOTE1, IDC_H_TEXT0, IDC_H_TEXT1,
    IDC_SYM_FILTER, IDC_SYM_LIST, IDC_SYM_COUNT,
    IDC_FUJI_NOTE, IDC_FUJI_CLEAR, IDC_FUJI_TEXT, IDC_FUJI_STATE,
    IDC_SAVE_KIND0, IDC_SAVE_KIND_LAST = IDC_SAVE_KIND0 + NSAVE - 1,
    IDC_LABEL_FIRST
};

typedef struct {
    HWND hwnd, tabs;
    HWND run_btn, step_btn, over_btn, out_btn, run_to_btn, frame_btn, sym_btn, save_btn, popout_btn, help_btn, status;

    /* a container per tab: its controls are its children, so it can be peeled off into a window */
    HWND pages[TAB_COUNT];
    HWND popped[TAB_COUNT];     /* the window a tab lives in, or NULL */
    int tab_page[TAB_COUNT];    /* the tab at each tab-control index */
    int ntabs;
    HWND new_parent;            /* where child() puts the next control; NULL: the window */

    /* the register strip, under every tab */
    HWND reg_label[NREGS], reg[NREGS], chk[NCHECKS], cpu_hint, halt_label;

    /* console */
    HWND con_out, con_prompt, con_in;
    char history[HISTORY_MAX][256];
    int nhistory, history_pos;
    uint64_t con_serial;        /* the stop the console last reported */

    /* memory */
    HWND mem_title, mem_view, goto_label, mem_goto, mem_back, mem_on, write_label, mem_write;
    HWND srch_label, srch_kind, srch_entry, srch_next, srch_prev, srch_note;
    int mem_top;                /* the dump's first address */
    int mem_hl;                 /* a range is marked */
    int mem_hl_lo, mem_hl_hi;

    /* disassembly */
    HWND follow_pc, jump_label, jump, disasm_hint, disasm;
    int disasm_top;             /* address of the first disassembly line */
    int selected;               /* the selected line's address, or -1 */
    msxdebug_line lines[DISASM_MAX];
    int line_count, pc_line;

    /* breakpoints */
    HWND bp_hint, bp_remove, bp_clear, bp_hits, bp_list, bp_empty;
    HWND bp_type, bp_start, bp_end, bp_cond, bp_add, bp_msg, bp_stop;
    msxdebug_breakpoint bps[MAX_BPS];
    int nbps;
    char bp_sig[MAX_BPS * 200]; /* what the list shows, to rebuild on change */
    int bp_sig_ok;

    /* watches */
    HWND wt_label, wt_entry, wt_add, wt_list, wt_mem, wt_brk, wt_remove;

    /* cart & I/O, sound */
    HWND cart_text, io_text, sound_text;
    int cart_ticks;

    /* trace (0, live) and history (1, when stopped) */
    HWND h_rec[2], h_filter[2], h_range[2], h_clear[2], h_note[2], h_text[2];
    uint64_t h_seen[2];
    int h_ticks;

    /* video */
    HWND video;

    /* symbols */
    HWND sym_filter_label, sym_filter, sym_count, sym_list;
    uint16_t sym_addr[SYM_MAX]; /* the address of each line shown, for a click */
    int sym_lines;
    char *sym_text;             /* what the list shows, to rebuild on change */
    char sym_filter_text[64];
    int sym_ok;

    /* fujinet */
    HWND fuji_title, fuji_note, fuji_clear, fuji_text, fuji_state_title, fuji_state;
    uint64_t fuji_last;
    int fuji_count, fuji_ticks;

    HWND views[40];             /* the read-only text views, drawn on the window colour */
    int nviews;
    HWND dim[96];               /* captions and hints, drawn grey */
    int ndim, nlabels;

    HFONT ui, ui_bold, mono;
    int dpi;
    int mono_w, mono_h, line_h;
    HACCEL accel;
    HBRUSH accent, bp_on, bp_off;

    msxsession *session;
    msxdebug *dbg;

    int page;
    unsigned seen_gen;
    uint64_t seen_serial;
    int was_stopped;
    int running_ticks;
    int updating;               /* a refresh is setting controls */

    char notice[200];           /* a message holding the status line a moment */
    DWORD notice_until;
} debugger;

static debugger *g_dbg;

/* The editable registers, in the order the strip lays them out. */
static const char *const k_reg_names[NREGS] = { "AF", "BC", "DE", "HL", "IX", "IY", "SP", "PC",
                                                "AF'", "BC'", "DE'", "HL'", "I", "R", "IM" };
static const int k_reg_ids[NREGS] = { MSX_REG_AF, MSX_REG_BC, MSX_REG_DE, MSX_REG_HL, MSX_REG_IX,
                                      MSX_REG_IY, MSX_REG_SP, MSX_REG_PC, MSX_REG_AF2, MSX_REG_BC2,
                                      MSX_REG_DE2, MSX_REG_HL2, MSX_REG_I, MSX_REG_R, MSX_REG_IM };

/* The check boxes: the flags in F, then the interrupt flip-flops. */
static const char *const k_chk_names[NCHECKS] = { "S", "Z", "H", "PV", "N", "C", "IFF1", "IFF2" };
static const int k_chk_bits[NCHECKS] = { 0x80, 0x40, 0x10, 0x04, 0x02, 0x01, 0, 0 };   /* in F */

static const char *const k_save_kinds[NSAVE] = { "dis", "mem", "vram", "regs" };
static const char *const k_save_titles[NSAVE] = { "Disassembly ($0000-$FFFF)...", "Memory (the 64K the CPU sees)...",
                                                  "VRAM...", "Registers (text)..." };

static const char *const k_bp_types[NBPTYPES] = { "Execute", "Read", "Write", "Access", "Port in", "Port out" };
static const int k_bp_type_ids[NBPTYPES] = { MSXDEBUG_BP_EXEC, MSXDEBUG_BP_READ, MSXDEBUG_BP_WRITE,
                                             MSXDEBUG_BP_ACCESS, MSXDEBUG_BP_IN, MSXDEBUG_BP_OUT };

/* ---- helpers ---------------------------------------------------------------- */

static void edit_text(HWND h, char *out, int outsz) { GetWindowTextA(h, out, outsz); }

static void wide(const char *utf8, wchar_t *out, int outn)
{
    if (outn <= 0) return;
    out[0] = 0;
    if (utf8 && MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, outn) <= 0) out[0] = 0;
    out[outn - 1] = 0;
}

/* A label set only when it changed (no flicker on the live refresh). */
static void set_label(HWND h, const char *utf8)
{
    wchar_t cur[512], w[512];
    wide(utf8, w, 512);
    GetWindowTextW(h, cur, 512);
    if (wcscmp(cur, w) != 0) SetWindowTextW(h, w);
}

/* EDIT controls want CR LF and draw control characters as boxes: engine
 * text (UTF-8) as the wide text a view shows. */
static wchar_t *wide_view_text(const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = malloc(n * 2 + 1);
    wchar_t *w = NULL;
    int wn;
    if (!buf) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[o++] = '\r'; buf[o++] = '\n'; }
        else if (c >= 0x20 || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    wn = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    if (wn > 0 && (w = malloc((size_t)wn * sizeof *w)) != NULL)
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, w, wn);
    free(buf);
    return w;
}

static size_t addf(char *s, size_t len, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (len >= cap) return len;
    va_start(ap, fmt);
    n = vsnprintf(s + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) return len;
    return len + (size_t)n < cap ? len + (size_t)n : cap - 1;
}

/* Blanks off both ends, in place. */
static char *strip(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

/* $hex, 0xhex, hexh, #dec, or bare hex, as the console reads numbers. */
static int parse_num(const char *text, long *out)
{
    char buf[64];
    const char *p;
    char *end;
    long v;
    int base = 16;

    snprintf(buf, sizeof buf, "%s", text);
    p = buf;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '$') p++;
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    else if (*p == '#') { p++; base = 10; }
    if (!*p) return 0;
    v = strtol(p, &end, base);
    if (base == 16 && (*end == 'h' || *end == 'H')) end++;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (end == p || *end) return 0;
    *out = v;
    return 1;
}

/* A label, or a number; -1 when it is neither. */
static int resolve_addr(debugger *d, const char *text)
{
    long v;
    int addr = -1;
    const char *p = text;
    while (*p == ' ') p++;
    if (!*p) return -1;
    if (*p != '$' && *p != '#' && !(p[0] == '0' && (p[1] == 'x' || p[1] == 'X')))
        addr = msxdebug_label_address(d->dbg, p);
    if (addr < 0 && parse_num(p, &v) && v >= 0 && v <= 0xffff) addr = (int)v;
    if (addr < 0) {
        char err[96];
        if (msxdebug_eval(d->dbg, p, &v, err, (int)sizeof err) == 0 && v >= 0 && v <= 0xffff) addr = (int)v;
    }
    return addr;
}

static const char *base_name(const char *path)
{
    const char *a = strrchr(path, '\\'), *b = strrchr(path, '/');
    if (b && (!a || b > a)) a = b;
    return a ? a + 1 : path;
}


/* ---- scale and fonts --------------------------------------------------------- */

static int system_dpi(void)
{
    HDC dc = GetDC(NULL);
    int v = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return v > 0 ? v : 96;
}

/* The window's DPI (Windows 10 1607+), else the system's. */
static int window_dpi(HWND hwnd)
{
    typedef UINT (WINAPI *dpi_fn)(HWND);
    static dpi_fn fn;
    static int looked;
    if (!looked) {
        HMODULE user = GetModuleHandleA("user32.dll");
        fn = user ? (dpi_fn)(void (*)(void))GetProcAddress(user, "GetDpiForWindow") : NULL;
        looked = 1;
    }
    if (fn && hwnd) {
        const UINT v = fn(hwnd);
        if (v) return (int)v;
    }
    return system_dpi();
}

static int S(const debugger *d, int v) { return MulDiv(v, d->dpi, 96); }

/* The message font at dpi (system metrics come at the system DPI), at its
 * own size (px 0) or px pixels at 96 dpi; face NULL keeps the message
 * font's, otherwise a fixed-pitch face (Consolas). */
static HFONT make_font(int dpi, int px, int weight, const char *face)
{
    NONCLIENTMETRICSA ncm;
    LOGFONTA lf;
    ZeroMemory(&ncm, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0))
        lf = ncm.lfMessageFont;
    else
        GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof lf, &lf);
    if (px > 0) lf.lfHeight = -MulDiv(px, dpi, 96);
    else lf.lfHeight = -MulDiv(abs(lf.lfHeight ? lf.lfHeight : 12), dpi, system_dpi());
    lf.lfWidth = 0;
    lf.lfWeight = weight;
    if (face) {
        snprintf(lf.lfFaceName, sizeof lf.lfFaceName, "%s", face);
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    }
    return CreateFontIndirectA(&lf);
}

static BOOL CALLBACK refont_child(HWND h, LPARAM lp)
{
    const HFONT *map = (const HFONT *)lp;     /* old ui, bold, mono; new ui, bold, mono */
    const HFONT f = (HFONT)SendMessageA(h, WM_GETFONT, 0, 0);
    int i;
    for (i = 0; i < 3; i++)
        if (f && f == map[i]) SendMessageA(h, WM_SETFONT, (WPARAM)map[i + 3], TRUE);
    return TRUE;
}

/* The message font and Consolas at the window's DPI; the controls already
 * made take the new ones. */
static void make_fonts(debugger *d)
{
    HFONT old[3] = { d->ui, d->ui_bold, d->mono };
    HDC dc;
    TEXTMETRICA tm;
    HGDIOBJ prev;
    int i;

    d->ui = make_font(d->dpi, 0, FW_NORMAL, NULL);
    d->ui_bold = make_font(d->dpi, 0, FW_BOLD, NULL);
    d->mono = make_font(d->dpi, MONO_PX, FW_NORMAL, "Consolas");

    /* the monospace cell, for the hand-drawn views */
    dc = GetDC(d->hwnd);
    prev = SelectObject(dc, d->mono);
    d->mono_w = S(d, 7);
    d->mono_h = S(d, 15);
    if (GetTextMetricsA(dc, &tm)) {
        d->mono_w = tm.tmAveCharWidth;
        d->mono_h = tm.tmHeight;
    }
    d->line_h = d->mono_h + S(d, 2);
    SelectObject(dc, prev);
    ReleaseDC(d->hwnd, dc);

    if (old[0]) {
        HFONT map[6] = { old[0], old[1], old[2], d->ui, d->ui_bold, d->mono };
        EnumChildWindows(d->hwnd, refont_child, (LPARAM)map);
    }
    for (i = 0; i < 3; i++)
        if (old[i]) DeleteObject(old[i]);
}

static int text_w(debugger *d, HFONT f, const char *text)
{
    HDC dc = GetDC(d->hwnd);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32A(dc, text, (int)strlen(text), &sz);
    SelectObject(dc, old);
    ReleaseDC(d->hwnd, dc);
    return sz.cx;
}

static int btn_w(debugger *d, const char *text)
{
    const int w = text_w(d, d->ui, text) + S(d, 24);
    return w < S(d, 64) ? S(d, 64) : w;
}

static int in_list(HWND h, const HWND *list, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (list[i] == h) return 1;
    return 0;
}

/* ---- the text views ------------------------------------------------------------ */

/* The rows an EDIT view shows. */
static int edit_rows(debugger *d, HWND h)
{
    RECT c;
    GetClientRect(h, &c);
    return d->mono_h > 0 && c.bottom > d->mono_h ? c.bottom / d->mono_h : 1;
}

/* Whether a log's reader is at its end (it then follows new lines). */
static int edit_at_end(debugger *d, HWND h)
{
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    const int count = (int)SendMessageW(h, EM_GETLINECOUNT, 0, 0);
    return first + edit_rows(d, h) >= count;
}

static void edit_to_end(HWND h)
{
    SendMessageW(h, EM_LINESCROLL, 0, SendMessageW(h, EM_GETLINECOUNT, 0, 0));
}

static void edit_scroll_to(HWND h, int first)
{
    SendMessageW(h, EM_LINESCROLL, 0, first - (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0));
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void set_text_at(HWND h, const char *text, int follow)
{
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    wchar_t *w = wide_view_text(text);
    if (!w) return;
    SendMessageW(h, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(h, w);
    if (follow) edit_to_end(h);
    else edit_scroll_to(h, first);
    SendMessageW(h, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(h, NULL, TRUE);
    free(w);
}

static void refresh_status(debugger *d);

static int notice_showing(debugger *d)
{
    return d->notice[0] && (LONG)(GetTickCount() - d->notice_until) < 0;
}

/* The other frontends' toasts: the message holds the status line a moment. */
static void toast(debugger *d, const char *msg)
{
    snprintf(d->notice, sizeof d->notice, "%s", msg);
    d->notice_until = GetTickCount() + NOTICE_MS;
    refresh_status(d);
}

/* ---- the console ---------------------------------------------------------------- */

/* Long sessions keep the last CONSOLE_KEEP characters, whole lines. */
static void console_trim(debugger *d)
{
    const int len = GetWindowTextLengthW(d->con_out);
    wchar_t *all, *nl;
    int cut;
    if (len <= CONSOLE_KEEP) return;
    all = malloc(((size_t)len + 1) * sizeof *all);
    if (!all) return;
    GetWindowTextW(d->con_out, all, len + 1);
    cut = len - CONSOLE_KEEP * 3 / 4;
    nl = wcschr(all + cut, L'\n');
    if (nl) cut = (int)(nl - all) + 1;
    free(all);
    SendMessageW(d->con_out, EM_SETSEL, 0, (LPARAM)cut);
    SendMessageW(d->con_out, EM_REPLACESEL, FALSE, (LPARAM)L"");
}

/* Onto the console's transcript, which follows its end only while the
 * reader is there. */
static void console_append(debugger *d, const char *text)
{
    const int follow = edit_at_end(d, d->con_out);
    const int first = (int)SendMessageW(d->con_out, EM_GETFIRSTVISIBLELINE, 0, 0);
    wchar_t *w = wide_view_text(text);
    int len;
    if (!w) return;
    len = GetWindowTextLengthW(d->con_out);
    SendMessageW(d->con_out, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(d->con_out, EM_REPLACESEL, FALSE, (LPARAM)w);
    free(w);
    console_trim(d);
    if (follow) edit_to_end(d->con_out);
    else edit_scroll_to(d->con_out, first);
}

/* Command output, ended with a newline. */
static void console_output(debugger *d, const char *text)
{
    const size_t n = strlen(text);
    if (!n) return;
    console_append(d, text);
    if (text[n - 1] != '\n') console_append(d, "\n");
}

/* Messages from Load Symbols and Save land in the console and, since it may
 * not be the tab showing, on the status line. */
static void say(debugger *d, const char *msg)
{
    console_output(d, msg);
    toast(d, msg);
}


/* ---- refreshers -------------------------------------------------------------- */

static int page_visible(debugger *d, int page);

static void refresh_status(debugger *d)
{
    msxdebug_snapshot snap;
    char text[260];
    const int stopped = msxdebug_stop_snapshot(d->dbg, &snap) != 0;

    if (notice_showing(d)) {
        snprintf(text, sizeof text, "%s", d->notice);
    } else if (stopped) {
        snprintf(text, sizeof text, "Stopped: %s", snap.text);
    } else {
        snprintf(text, sizeof text, "Running");
    }
    set_label(d->status, text);
    set_label(d->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    InvalidateRect(d->run_btn, NULL, FALSE);
    InvalidateRect(d->status, NULL, TRUE);
}

/* The register strip: the machine's values (the stale ones while it runs, a
 * box being typed in is left alone). */
static void refresh_cpu(debugger *d)
{
    msxdebug_cpu c;
    char buf[32];
    int i;
    msxdebug_cpu_get(d->dbg, &c);
    {
        const int v[NREGS] = { c.af, c.bc, c.de, c.hl, c.ix, c.iy, c.sp, c.pc,
                               c.af2, c.bc2, c.de2, c.hl2, c.i, c.r, c.im };
        for (i = 0; i < NREGS; i++) {
            if (GetFocus() == d->reg[i]) continue;   /* do not fight the user's typing */
            /* the register pairs as four digits, I and R as two, IM as it is */
            snprintf(buf, sizeof buf, i < 12 ? "%04X" : i < 14 ? "%02X" : "%d", v[i] & 0xFFFF);
            set_label(d->reg[i], buf);
        }
    }
    {
        const int on[NCHECKS] = { c.sf, c.zf, c.hf, c.pf, c.nf, c.cf, c.iff1, c.iff2 };
        d->updating = 1;
        for (i = 0; i < NCHECKS; i++)
            SendMessageA(d->chk[i], BM_SETCHECK, on[i] ? BST_CHECKED : BST_UNCHECKED, 0);
        d->updating = 0;
    }
    set_label(d->halt_label, c.halted ? "HALT" : "");
}

/* Where a byte's two hex digits start in a Memory row: "AAAA  " then three
 * characters a byte, one more after the eighth. */
static int mem_hex_col(int col)
{
    return 6 + col * 3 + (col >= 8 ? 1 : 0);
}

static void refresh_mem(debugger *d)
{
    static uint8_t mem[MEM_BYTES];
    static char t[(MEM_ROWS + 1) * 140];
    static int rowoff[MEM_ROWS + 1];    /* each row's first character in the control (CR LF counted) */
    msxdebug_region map[16];
    char region[80], last_region[80];
    size_t len = 0;
    int row, col, nmap;

    memset(mem, 0, sizeof mem);
    msxdebug_read(d->dbg, (uint16_t)d->mem_top, mem, MEM_BYTES);
    nmap = msxdebug_memory_map(d->dbg, map, 16);
    if (nmap < 0) nmap = 0;
    last_region[0] = '\0';
    len = addf(t, len, sizeof t, "      ");
    for (col = 0; col < 16; col++)
        len = addf(t, len, sizeof t, col == 8 ? " %X  " : "%X  ", col);
    len = addf(t, len, sizeof t, "\n");
    for (row = 0; row < MEM_ROWS; row++) {
        const uint8_t *r = mem + row * 16;
        const int addr = d->mem_top + row * 16;
        rowoff[row] = (int)len + row + 1;
        len = addf(t, len, sizeof t, "%04X  ", addr);
        for (col = 0; col < 16; col++)
            len = addf(t, len, sizeof t, col == 8 ? " %02X " : "%02X ", r[col]);
        len = addf(t, len, sizeof t, " ");
        for (col = 0; col < 16; col++)
            len = addf(t, len, sizeof t, "%c", r[col] >= 0x20 && r[col] < 0x7F ? r[col] : '.');
        /* the slot's device, on the first row of each run */
        region[0] = '\0';
        msxmem_region_name(map, nmap, (uint16_t)addr, region, (int)sizeof region);
        if (region[0] && strcmp(region, last_region) != 0) len = addf(t, len, sizeof t, "  ; %s", region);
        snprintf(last_region, sizeof last_region, "%s", region);
        len = addf(t, len, sizeof t, "\n");
    }
    set_text_at(d->mem_view, t, 0);

    /* the bytes Go to, a search hit or a watch found stay selected across refreshes */
    if (d->mem_hl && d->mem_hl_hi >= d->mem_top && d->mem_hl_lo < d->mem_top + MEM_BYTES) {
        const int first = d->mem_hl_lo > d->mem_top ? d->mem_hl_lo : d->mem_top;
        const int last = d->mem_hl_hi < d->mem_top + MEM_BYTES - 1 ? d->mem_hl_hi : d->mem_top + MEM_BYTES - 1;
        const int first_row = (first - d->mem_top) / 16, last_row = (last - d->mem_top) / 16;
        const int top_line = (int)SendMessageW(d->mem_view, EM_GETFIRSTVISIBLELINE, 0, 0);
        SendMessageW(d->mem_view, EM_SETSEL, (WPARAM)(rowoff[first_row] + mem_hex_col((first - d->mem_top) % 16)),
                     (LPARAM)(rowoff[last_row] + mem_hex_col((last - d->mem_top) % 16) + 2));
        edit_scroll_to(d->mem_view, top_line);
    }
}

/* Shows bytes lo..hi in the Memory tab, selected, the page starting just above them. */
static void show_in_memory(debugger *d, int low, int high)
{
    char shown[16];
    int top;
    if (low < 0) low = 0;
    if (low > 0xFFFF) low = 0xFFFF;
    if (high < low) high = low;
    if (high > 0xFFFF) high = 0xFFFF;
    d->mem_hl = 1;
    d->mem_hl_lo = low;
    d->mem_hl_hi = high;
    d->mem_top = (low > 0x100 ? low - 0x100 : 0) & ~0xF;
    if (d->mem_top > 0x10000 - MEM_BYTES) d->mem_top = 0x10000 - MEM_BYTES;
    snprintf(shown, sizeof shown, "%04X", low);
    SetWindowTextA(d->mem_goto, shown);
    refresh_mem(d);
    /* the row 30% of the way down */
    top = 1 + (low - d->mem_top) / 16 - edit_rows(d, d->mem_view) * 3 / 10;
    edit_scroll_to(d->mem_view, top > 0 ? top : 0);
}

/* The disassembly rows the view has room for. */
static int disasm_rows(debugger *d)
{
    RECT c;
    int rows;
    if (!d->disasm || d->line_h <= 0) return 1;
    GetClientRect(d->disasm, &c);
    rows = (c.bottom - c.top) / d->line_h;
    if (rows < 1) rows = 1;
    if (rows > DISASM_MAX) rows = DISASM_MAX;
    return rows;
}

static int row_address(debugger *d, int addr, int rows)
{
    const int a = msxdebug_row_address(d->dbg, (uint16_t)addr, rows);
    return a < 0 ? addr : a & 0xFFFF;
}

static void refresh_disasm(debugger *d)
{
    int pc_line = -1;
    const int rows = disasm_rows(d);
    const int follow = SendMessageA(d->follow_pc, BM_GETCHECK, 0, 0) == BST_CHECKED;

    if (follow) {
        /* the PC a third of the way down, so what led to it shows too */
        msxdebug_cpu c;
        msxdebug_cpu_get(d->dbg, &c);
        d->disasm_top = row_address(d, c.pc & 0xFFFF, -(rows / 3));
    }
    d->line_count = msxdebug_disassemble(d->dbg, (uint16_t)d->disasm_top, d->lines, rows, &pc_line);
    if (d->line_count < 0) d->line_count = 0;
    d->pc_line = pc_line;
    InvalidateRect(d->disasm, NULL, FALSE);
}

static const char *bp_kind(int type, char *buf)
{
    buf[0] = '\0';
    if (type == MSXDEBUG_BP_ACCESS) return strcpy(buf, "access");
    if (type & MSXDEBUG_BP_EXEC) strcat(buf, "exec");
    if (type & MSXDEBUG_BP_READ) strcat(buf, buf[0] ? "/read" : "read");
    if (type & MSXDEBUG_BP_WRITE) strcat(buf, buf[0] ? "/write" : "write");
    if (type & MSXDEBUG_BP_IN) strcat(buf, buf[0] ? "/in" : "in");
    if (type & MSXDEBUG_BP_OUT) strcat(buf, buf[0] ? "/out" : "out");
    return buf;
}

static void lv_set(HWND lv, int row, int col, const char *text)
{
    wchar_t w[256];
    LVITEMW it;
    wide(text, w, 256);
    memset(&it, 0, sizeof it);
    it.iSubItem = col;
    it.pszText = w;
    SendMessageW(lv, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
}

/* The list, or (empty) the hint in its place. */
static void show_bp_list(debugger *d)
{
    const int on = page_visible(d, TAB_BREAKS);
    ShowWindow(d->bp_list, on && d->nbps > 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(d->bp_empty, on && d->nbps == 0 ? SW_SHOW : SW_HIDE);
}

/* Rebuilt only when something in it changed (each step refreshes), keeping
 * the selected breakpoint selected. */
static void refresh_bps(debugger *d)
{
    static char sig[sizeof d->bp_sig];
    size_t len = 0;
    int i, sel_id = -1;
    const int sel = (int)SendMessageA(d->bp_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);

    if (sel >= 0 && sel < d->nbps) sel_id = d->bps[sel].id;
    d->nbps = msxdebug_breakpoint_list(d->dbg, d->bps, MAX_BPS);
    if (d->nbps < 0) d->nbps = 0;
    sig[0] = '\0';
    for (i = 0; i < d->nbps; i++)
        len = addf(sig, len, sizeof sig, "%d %d %u %d %u %u %s\n", d->bps[i].id, d->bps[i].enabled,
                   d->bps[i].hits, d->bps[i].type, d->bps[i].start, d->bps[i].end, d->bps[i].condition);
    if (d->bp_sig_ok && strcmp(sig, d->bp_sig) == 0) { show_bp_list(d); return; }
    memcpy(d->bp_sig, sig, sizeof sig);
    d->bp_sig_ok = 1;

    d->updating = 1;
    SendMessageA(d->bp_list, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; i < d->nbps; i++) {
        const msxdebug_breakpoint *b = &d->bps[i];
        const int port = (b->type & (MSXDEBUG_BP_IN | MSXDEBUG_BP_OUT)) != 0;
        LVITEMA it;
        char id[16], kind[40], range[32], hits[24];
        snprintf(id, sizeof id, "#%d", b->id);
        if (b->start == b->end) snprintf(range, sizeof range, port ? "$%02X" : "$%04X", b->start);
        else snprintf(range, sizeof range, port ? "$%02X-$%02X" : "$%04X-$%04X", b->start, b->end);
        snprintf(hits, sizeof hits, "%u", b->hits);
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = id;
        SendMessageA(d->bp_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
        lv_set(d->bp_list, i, 1, bp_kind(b->type, kind));
        lv_set(d->bp_list, i, 2, range);
        lv_set(d->bp_list, i, 3, b->condition);
        lv_set(d->bp_list, i, 4, hits);
        memset(&it, 0, sizeof it);
        it.stateMask = LVIS_STATEIMAGEMASK | LVIS_SELECTED | LVIS_FOCUSED;
        it.state = INDEXTOSTATEIMAGEMASK(b->enabled ? 2 : 1);
        if (b->id == sel_id) it.state |= LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageA(d->bp_list, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&it);
    }
    d->updating = 0;
    show_bp_list(d);
}

/* A view's text, replaced only when it changed (an unchanged one keeps its
 * selection and where the reader scrolled it to). */
static void set_view(HWND h, const char *text)
{
    wchar_t *w = wide_view_text(text), *cur;
    int n;
    if (!w) return;
    n = GetWindowTextLengthW(h);
    cur = malloc(((size_t)n + 2) * sizeof *cur);
    if (cur) {
        GetWindowTextW(h, cur, n + 1);
        if (wcscmp(cur, w) == 0) {
            free(cur);
            free(w);
            return;
        }
        free(cur);
    }
    free(w);
    set_text_at(h, text, 0);
}

/* Why the machine stopped, in words, under the breakpoint list. */
static void refresh_bp_stop(debugger *d)
{
    msxdebug_snapshot snap;
    char line[260];
    if (msxdebug_stop_snapshot(d->dbg, &snap)) snprintf(line, sizeof line, "Stopped: %s", snap.text);
    else snprintf(line, sizeof line, "Running.");
    set_label(d->bp_stop, line);
}

/* Rebuilt only when a line changed, keeping the selection. */
static void refresh_watches(debugger *d)
{
    static char last[MSXWATCH_MAX * 220];
    static char now[MSXWATCH_MAX * 220];
    const int n = msxwatch_count();
    const int keep = (int)SendMessageA(d->wt_list, LB_GETCURSEL, 0, 0);
    size_t len = 0;
    int i;

    now[0] = '\0';
    for (i = 0; i < n; i++) {
        char line[200];
        line[0] = '\0';
        msxwatch_format(d->dbg, i, line, (int)sizeof line);
        len = addf(now, len, sizeof now, "%s\n", line);
    }
    if (strcmp(now, last) == 0 && (int)SendMessageA(d->wt_list, LB_GETCOUNT, 0, 0) == n) return;
    memcpy(last, now, sizeof last);
    SendMessageA(d->wt_list, WM_SETREDRAW, FALSE, 0);
    SendMessageA(d->wt_list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < n; i++) {
        char line[200];
        line[0] = '\0';
        msxwatch_format(d->dbg, i, line, (int)sizeof line);
        SendMessageA(d->wt_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (keep >= 0 && keep < n) SendMessageA(d->wt_list, LB_SETCURSEL, (WPARAM)keep, 0);
    SendMessageA(d->wt_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(d->wt_list, NULL, TRUE);
}

/* "slot 3-2" or "slot 1" */
static void slot_name(int slot, int sub, char *dst, size_t cap)
{
    if (sub >= 0) snprintf(dst, cap, "slot %d-%d", slot, sub);
    else snprintf(dst, cap, "slot %d", slot);
}

/* The machine and its devices, and the memory map as the slots are selected now. */
static void refresh_cart(debugger *d)
{
    static char t[8192];
    msxdebug_cart cart;
    msxdebug_io ports;
    msxdebug_region map[16];
    size_t len = 0;
    int i, n;
    const char *p;

    msxdebug_cart_info(d->dbg, &cart);
    len = addf(t, len, sizeof t, "Machine   %s (%s)\n", cart.machine[0] ? cart.machine : "(none)", cart.type);
    if (cart.z80_hz) len = addf(t, len, sizeof t, "CPU       %d Hz\n", cart.z80_hz);
    len = addf(t, len, sizeof t, "FujiNet   %s\n",
               cart.fujinet_external ? (cart.fujinet_link ? "external, cartridge connected" : "external, cartridge not connected")
               : cart.fujinet ? (cart.fujinet_link ? "cartridge in a slot, link up" : "cartridge in a slot, link down")
                              : "no cartridge");
    len = addf(t, len, sizeof t, "Chips     %s%s%s%s%s\n", cart.scc ? "SCC " : "", cart.music ? "MSX-MUSIC " : "",
               cart.audio ? "MSX-AUDIO " : "", cart.mapper ? "memory mapper " : "",
               (cart.scc || cart.music || cart.audio || cart.mapper) ? "" : "none");
    len = addf(t, len, sizeof t, "Devices   ");
    for (p = cart.devices; *p; p++) {
        if (p[0] == ',' && p[1] == ' ') {
            len = addf(t, len, sizeof t, "\n          ");
            p++;
        } else {
            len = addf(t, len, sizeof t, "%c", *p);
        }
    }
    len = addf(t, len, sizeof t, "\n\nMemory map (as the slots are selected now)\n");
    n = msxdebug_memory_map(d->dbg, map, 16);
    if (n <= 0) len = addf(t, len, sizeof t, "  (no machine)\n");
    for (i = 0; i < n; i++)
        len = addf(t, len, sizeof t, "  %04X-%04X  %c%c  %s\n", map[i].lo, map[i].hi, map[i].readable ? 'r' : '-',
                   map[i].writable ? 'w' : '-', map[i].name);
    if (msxdebug_io_get(d->dbg, &ports) && ports.valid) {
        len = addf(t, len, sizeof t, "\nCPU pages\n");
        for (i = 0; i < 4; i++) {
            char nm[24];
            slot_name(ports.page_slot[i], ports.page_subslot[i], nm, sizeof nm);
            len = addf(t, len, sizeof t, "  $%04X  %-10s %s\n", i * 0x4000, nm, ports.page_device[i]);
        }
    }
    set_view(d->cart_text, t);
}

/* The slot and mapper registers, the PSG, and what the keyboard and joystick ports read. */
static void refresh_io(debugger *d)
{
    static char t[16384];
    msxdebug_io ports;
    size_t len = 0;
    int i;

    t[0] = '\0';
    msxdebug_io_state(d->dbg, t, (int)sizeof t);
    len = strlen(t);
    if (msxdebug_io_get(d->dbg, &ports) && ports.valid) {
        len = addf(t, len, sizeof t, "\nKeyboard matrix (a 0 bit is a pressed key)\n");
        for (i = 0; i < ports.keymatrix_rows && i < 16; i++)
            len = addf(t, len, sizeof t, "  row %2d  %02X\n", i, ports.keymatrix[i]);
        len = addf(t, len, sizeof t, "Joystick ports  A $%02X  B $%02X (a 1 bit is not pressed)\n", ports.joyport[0],
                   ports.joyport[1]);
        if (ports.rom_mappers > 0) len = addf(t, len, sizeof t, "\nROM mappers (the block at $0000 $2000 ... $E000)\n");
        for (i = 0; i < ports.rom_mappers && i < 6; i++) {
            int block;
            len = addf(t, len, sizeof t, "  %-24s", ports.rom[i].device);
            for (block = 0; block < 8; block++) {
                if (ports.rom[i].block[block] == 255) len = addf(t, len, sizeof t, " --");
                else len = addf(t, len, sizeof t, " %02X", ports.rom[i].block[block]);
            }
            len = addf(t, len, sizeof t, "\n");
        }
        if (ports.slotmap[0]) len = addf(t, len, sizeof t, "\nopenMSX slotmap\n%s\n", ports.slotmap);
    }
    set_view(d->io_text, t);
}

/* The PSG and the SCC, MSX-MUSIC and MSX-AUDIO chips the machine has, straight out of the chips (stopped only). */
static void refresh_sound(debugger *d)
{
    static char t[24576];
    static msxdebug_sound snd;
    size_t len = 0;
    int voice, index;

    if (!msxdebug_is_stopped(d->dbg)) {
        set_view(d->sound_text, "Stop the machine to read the sound chips.");
        return;
    }
    t[0] = '\0';
    memset(&snd, 0, sizeof snd);
    msxdebug_sound_get(d->dbg, &snd);
    if (snd.psg.present) {
        const msxdebug_psg *psg = &snd.psg;
        len = addf(t, len, sizeof t, "PSG (AY-3-8910)\n  regs");
        for (index = 0; index < 16; index++) len = addf(t, len, sizeof t, " %02X", psg->reg[index]);
        len = addf(t, len, sizeof t, "\n");
        for (voice = 0; voice < 3; voice++)
            len = addf(t, len, sizeof t, "  %c  period $%03X  %8.1f Hz  tone %-3s  noise %-3s  level %2d%s\n", 'A' + voice,
                       psg->period[voice], psg->freq[voice], psg->tone_on[voice] ? "on" : "off", psg->noise_on[voice] ? "on" : "off",
                       psg->level[voice], psg->envelope[voice] ? " (envelope)" : "");
        len = addf(t, len, sizeof t, "  noise period %d   envelope period $%04X  shape $%X   mixer $%02X (a set bit is OFF)\n\n",
                   psg->noise_period, psg->env_period, psg->env_shape, psg->reg[7]);
    } else {
        len = addf(t, len, sizeof t, "PSG: not found\n\n");
    }
    if (snd.scc.present) {
        const msxdebug_scc *scc = &snd.scc;
        len = addf(t, len, sizeof t, "SCC  (%s)\n", scc->name);
        for (voice = 0; voice < 5; voice++) {
            len = addf(t, len, sizeof t, "  %d  period $%03X  %8.1f Hz  volume %2d  %s   wave", voice + 1, scc->period[voice],
                       scc->freq[voice], scc->volume[voice], scc->enabled[voice] ? "on " : "off");
            for (index = 0; index < 32; index++) len = addf(t, len, sizeof t, " %02X", scc->wave[voice][index]);
            len = addf(t, len, sizeof t, "\n");
        }
        len = addf(t, len, sizeof t, "\n");
    }
    if (snd.opll.present) {
        const msxdebug_opll *opll = &snd.opll;
        len = addf(t, len, sizeof t, "MSX-MUSIC  (%s)%s\n", opll->name, opll->rhythm ? "  rhythm mode" : "");
        for (voice = 0; voice < 9; voice++)
            len = addf(t, len, sizeof t, "  %d  F-number $%03X  block %d  %8.1f Hz  key %-3s  instrument %2d  volume %2d\n", voice + 1,
                       opll->fnum[voice], opll->block[voice], opll->freq[voice], opll->key_on[voice] ? "on" : "off", opll->instrument[voice], opll->volume[voice]);
        len = addf(t, len, sizeof t, "  regs");
        for (index = 0; index < 0x39; index++) len = addf(t, len, sizeof t, "%s%02X", index % 16 ? " " : "\n    ", opll->reg[index]);
        len = addf(t, len, sizeof t, "\n\n");
    }
    if (snd.y8950.present) {
        const msxdebug_y8950 *audio = &snd.y8950;
        len = addf(t, len, sizeof t, "MSX-AUDIO  (%s)\n  regs", audio->name);
        for (index = 0; index < 256; index++) len = addf(t, len, sizeof t, "%s%02X", index % 16 ? " " : "\n    ", audio->reg[index]);
        len = addf(t, len, sizeof t, "\n");
    }
    set_view(d->sound_text, t);
}

/* Trace (0: live, the ring as it fills) and History (1: what ran before the
 * machine stopped, while it is stopped) read the same ring: the newest
 * HIST_SHOWN entries that pass the filter, oldest first. */
static void hist_refresh(debugger *d, int idx, int force)
{
    static msxdebug_trace buf[HIST_COPY];
    static char text[HIST_SHOWN * 260];
    const int live = idx == 0;
    const int stopped = msxdebug_is_stopped(d->dbg) != 0;
    const int on = live ? msxdebug_trace_enabled(d->dbg) : msxdebug_history_enabled(d->dbg);
    const uint64_t count = live ? msxdebug_trace_total(d->dbg) : msxdebug_history_count(d->dbg);
    uint16_t low = 0, high = 0xFFFF;
    int filter = MSXDEBUG_TRACE_ALL, n, i, shown = 0, hit[HIST_SHOWN];
    size_t len = 0;
    char note[120];

    SendMessageA(d->h_rec[idx], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    InvalidateRect(d->h_rec[idx], NULL, FALSE);
    if (!force && count == d->h_seen[idx] && (live || stopped)) return;
    d->h_seen[idx] = count;
    if (!live && !stopped) {
        set_label(d->h_note[idx], "Stop the machine to read its history.");
        return;
    }
    switch ((int)SendMessageA(d->h_filter[idx], CB_GETCURSEL, 0, 0)) {
    case 1: filter = MSXDEBUG_TRACE_BRANCHES; break;
    case 2: {
        char r[64];
        GetWindowTextA(d->h_range[idx], r, sizeof r);
        if (msxsym_parse_range(d->dbg, r, &low, &high)) filter = MSXDEBUG_TRACE_RANGE;
        break;
    }
    default: break;
    }
    if (live) {
        n = msxdebug_trace_read(d->dbg, buf, HIST_COPY);      /* newest first */
        for (i = 0; i < n && shown < HIST_SHOWN; i++)
            if (msxdebug_trace_match(&buf[i], filter, low, high)) hit[shown++] = i;
        text[0] = '\0';
        if (!on && n == 0) snprintf(text, sizeof text, "The trace is off. Press Record, then let the machine run.");
        for (i = shown - 1; i >= 0 && len + 280 < sizeof text; i--) {
            char line[256];
            msxdebug_trace_format(&buf[hit[i]], line, (int)sizeof line);
            len = addf(text, len, sizeof text, "%s\n", line);
        }
    } else {
        n = msxdebug_history_copy(d->dbg, buf, HIST_COPY);    /* oldest first */
        for (i = n - 1; i >= 0 && shown < HIST_SHOWN; i--)
            if (msxdebug_history_match(&buf[i], filter, low, high)) hit[shown++] = i;
        text[0] = '\0';
        if (!on && n == 0) snprintf(text, sizeof text, "History is off. Press Record, then let the machine run.");
        for (i = shown - 1; i >= 0 && len + 280 < sizeof text; i--) {
            char line[256];
            msxdebug_history_format(&buf[hit[i]], line, (int)sizeof line);
            len = addf(text, len, sizeof text, "%s\n", line);
        }
    }
    set_text_at(d->h_text[idx], text, 1);
    snprintf(note, sizeof note, "%s%llu instructions recorded, last %d shown", on ? "Recording. " : "",
             (unsigned long long)count, shown);
    set_label(d->h_note[idx], note);
}

/* The symbols that pass the filter, one a line with its address kept for a
 * click; rebuilt only when the text would change. */
static void refresh_symbols(debugger *d)
{
    static int idx[SYM_MAX];
    char filter[64], count[64];
    char *text;
    size_t len = 0, cap = (size_t)SYM_MAX * 96;
    int n, i, shown = 0;

    GetWindowTextA(d->sym_filter, filter, sizeof filter);
    n = msxdebug_symbols_filter(d->dbg, filter, idx, SYM_MAX);
    if (n < 0) n = 0;
    text = malloc(cap);
    if (!text) return;
    text[0] = '\0';
    for (i = 0; i < n; i++) {
        uint16_t addr;
        char name[80];
        if (!msxdebug_symbols_get(d->dbg, idx[i], &addr, name, (int)sizeof name)) continue;
        len = addf(text, len, cap, "%04X  %s\n", addr, name);
        d->sym_addr[shown++] = addr;
    }
    d->sym_lines = shown;
    if (d->sym_ok && d->sym_text && strcmp(text, d->sym_text) == 0) {
        free(text);
        return;
    }
    free(d->sym_text);
    d->sym_text = text;
    d->sym_ok = 1;
    snprintf(d->sym_filter_text, sizeof d->sym_filter_text, "%s", filter);
    set_text_at(d->sym_list, text, 0);
    snprintf(count, sizeof count, "%d symbol%s%s", shown, shown == 1 ? "" : "s", filter[0] ? " (filtered)" : "");
    set_label(d->sym_count, count);
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
static void refresh_fujinet(debugger *d, int force)
{
    static msx_fuji_packet p[TRACE_MAX];
    static char t[TRACE_MAX * 320 + 128];   /* an entry is at most ~300 */
    static char state[2048];
    char note[200];
    size_t len = 0;
    int n = msxdebug_fujinet_trace(d->dbg, p, TRACE_MAX), i, follow;

    if (msxsession_fujinet_external(d->session))
        snprintf(note, sizeof note, "External FujiNet %s, cartridge %s.",
                 msxsession_fujinet_endpoint(d->session),
                 msxsession_fujinet_link(d->session) ? "connected" : "not connected");
    else if (!msxsession_fujinet_running(d->session))
        snprintf(note, sizeof note, "FujiNet is off (disabled, or its runtime did not start).");
    else
        snprintf(note, sizeof note, "FujiNet is on, cartridge %s.",
                 msxsession_fujinet_link(d->session) ? "connected" : "not connected");
    set_label(d->fuji_note, note);
    state[0] = '\0';
    msxdebug_fujinet_state(d->dbg, state, (int)sizeof state);
    set_text_at(d->fuji_state, state, 0);
    if (n < 0) n = 0;

    if (!force && n == d->fuji_count && (n == 0 || p[n - 1].serial == d->fuji_last)) return;
    follow = force || d->fuji_count < 0 || edit_at_end(d, d->fuji_text);
    d->fuji_count = n;
    d->fuji_last = n > 0 ? p[n - 1].serial : 0;
    t[0] = '\0';
    for (i = 0; i < n; i++) {
        int k;
        len = addf(t, len, sizeof t, "%s%s", i ? "\n" : "", p[i].text);
        if (p[i].repeat > 1) len = addf(t, len, sizeof t, " \xc3\x97%u", (unsigned)p[i].repeat);
        len = addf(t, len, sizeof t, "\n     ");
        for (k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            len = addf(t, len, sizeof t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) len = addf(t, len, sizeof t, "\xe2\x80\xa6");
    }
    if (n == 0) len = addf(t, len, sizeof t, "(no FujiBus packets since the trace was cleared)");
    set_text_at(d->fuji_text, t, follow);
}


/* One tab brought up to date (force: the live ones too, the Video tab redrawn). */
static void refresh_page(debugger *d, int page, int force)
{
    switch (page) {
    case TAB_MEMORY: refresh_mem(d); break;
    case TAB_DISASM: refresh_disasm(d); break;
    case TAB_BREAKS: refresh_bps(d); refresh_bp_stop(d); break;
    case TAB_WATCHES: refresh_watches(d); break;
    case TAB_CART: refresh_cart(d); refresh_io(d); break;
    case TAB_TRACE: hist_refresh(d, 0, force); break;
    case TAB_HISTORY: hist_refresh(d, 1, force); break;
    case TAB_VIDEO: if (d->video) dbg_video_refresh(d->video, force); break;
    case TAB_SOUND: refresh_sound(d); break;
    case TAB_SYMBOLS: refresh_symbols(d); break;
    case TAB_FUJINET: refresh_fujinet(d, 0); break;
    default: break;
    }
}

/* The tabs on screen: the one the tab control shows, and any in a window of its own. */
static void refresh_shown(debugger *d, int force)
{
    int p;
    for (p = 0; p < TAB_COUNT; p++)
        if (page_visible(d, p)) refresh_page(d, p, force);
}

/* At a new stop the console says why, as the other frontends' do. */
static void console_stop(debugger *d, const msxdebug_snapshot *snap)
{
    char line[220];
    if (!snap->paused || snap->serial == d->con_serial) return;
    d->con_serial = snap->serial;
    snprintf(line, sizeof line, "\xe2\x80\x94 %s", snap->text);
    console_output(d, line);
}

/* After anything that may have moved the engine: the status, the register
 * strip and the tabs showing follow it. */
static void resync(debugger *d)
{
    msxdebug_snapshot snap;
    const int stopped = msxdebug_stop_snapshot(d->dbg, &snap) != 0;
    console_stop(d, &snap);
    d->was_stopped = stopped;
    d->seen_serial = snap.serial;
    d->seen_gen = msxdebug_generation(d->dbg);
    d->running_ticks = 0;
    refresh_status(d);
    refresh_cpu(d);
    refresh_shown(d, 1);
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
static void settle_and_sync(debugger *d, int expect_stop)
{
    int i;
    d->notice[0] = '\0';        /* the machine moved: its new state replaces a notice */
    for (i = 0; expect_stop && i < 40 && !msxdebug_is_stopped(d->dbg); i++)
        Sleep(1);
    resync(d);
}

static void tick(debugger *d)
{
    msxdebug_snapshot snap;
    unsigned gen;
    int stopped;
    if (!IsWindowVisible(d->hwnd)) return;
    if (d->notice[0] && !notice_showing(d)) {
        d->notice[0] = '\0';
        refresh_status(d);
    }
    gen = msxdebug_generation(d->dbg);
    stopped = msxdebug_stop_snapshot(d->dbg, &snap) != 0;
    if (gen != d->seen_gen || stopped != d->was_stopped || snap.serial != d->seen_serial) {
        if (stopped != d->was_stopped) d->notice[0] = '\0';   /* a stop (or a go) says so at once */
        resync(d);
        return;
    }
    /* the pictures move while the machine runs */
    if (page_visible(d, TAB_VIDEO) && d->video) dbg_video_refresh(d->video, 0);
    if (!stopped && ++d->running_ticks >= 5) {
        /* running: the live values of the tabs showing, twice a second */
        d->running_ticks = 0;
        refresh_status(d);
        refresh_cpu(d);
        refresh_shown(d, 0);
    } else if (stopped && page_visible(d, TAB_FUJINET) && ++d->fuji_ticks >= 5) {
        d->fuji_ticks = 0;
        refresh_fujinet(d, 0);
    }
}

/* ---- actions ------------------------------------------------------------------ */

static void on_run(debugger *d)
{
    const int stopped = msxdebug_is_stopped(d->dbg);
    if (stopped) msxdebug_resume(d->dbg);
    else msxdebug_stop(d->dbg);
    settle_and_sync(d, !stopped);
}

static void on_run_to(debugger *d)
{
    if (d->selected < 0) {
        toast(d, "Select a line in the Disassembly to run to");
        return;
    }
    msxdebug_run_to(d->dbg, (uint16_t)d->selected);
    settle_and_sync(d, 1);
}

static void toggle_breakpoint(debugger *d, int addr)
{
    msxdebug_breakpoint_toggle(d->dbg, (uint16_t)addr);
    resync(d);
}

/* F9: the breakpoint on the selected disassembly line. */
static void on_toggle_bp(debugger *d)
{
    if (d->selected < 0) {
        toast(d, "Select a line in the Disassembly first");
        return;
    }
    toggle_breakpoint(d, d->selected);
}

static void on_console(debugger *d)
{
    static char out[65536];
    char buf[256], line[300], *cmd;
    const int was = msxdebug_is_stopped(d->dbg);

    edit_text(d->con_in, buf, sizeof buf);
    cmd = strip(buf);
    if (!*cmd) return;
    /* history: the newest last, no repeats of the one before */
    if (d->nhistory == 0 || strcmp(d->history[d->nhistory - 1], cmd) != 0) {
        if (d->nhistory == HISTORY_MAX) {
            memmove(d->history[0], d->history[1], sizeof d->history[0] * (HISTORY_MAX - 1));
            d->nhistory--;
        }
        snprintf(d->history[d->nhistory++], sizeof d->history[0], "%s", cmd);
    }
    d->history_pos = d->nhistory;
    snprintf(line, sizeof line, "> %s\n", cmd);
    edit_to_end(d->con_out);     /* a command brings the reader back down */
    console_append(d, line);

    out[0] = '\0';
    msxdebug_command(d->dbg, cmd, out, sizeof out);
    SetWindowTextA(d->con_in, "");
    console_output(d, out);
    settle_and_sync(d, was);
}

/* Up and Down walk the commands already run; past the newest is an empty
 * line again. */
static void history_step(debugger *d, int newer)
{
    int len;
    if (d->nhistory == 0) return;
    d->history_pos += newer ? 1 : -1;
    if (d->history_pos < 0) d->history_pos = 0;
    if (d->history_pos >= d->nhistory) {
        d->history_pos = d->nhistory;
        SetWindowTextA(d->con_in, "");
        return;
    }
    SetWindowTextA(d->con_in, d->history[d->history_pos]);
    len = GetWindowTextLengthA(d->con_in);
    SendMessageA(d->con_in, EM_SETSEL, (WPARAM)len, (LPARAM)len);
}

/* Tab: the engine completes the end of the line; one answer takes its
 * place, several are listed (and the part they share is typed). */
static void complete_console(debugger *d)
{
    char text[256], comps[4096], common[256] = "", line[256];
    char *sp, *cur, *nl;
    int n, first = 1;
    edit_text(d->con_in, text, sizeof text);
    n = msxdebug_completions(d->dbg, text, comps, sizeof comps);
    if (n <= 0) return;
    for (cur = comps; *cur; cur = nl ? nl + 1 : cur + strlen(cur)) {
        int k = 0;
        nl = strchr(cur, '\n');
        snprintf(line, sizeof line, "%.*s", nl ? (int)(nl - cur) : (int)strlen(cur), cur);
        if (!line[0]) continue;
        if (first) {
            snprintf(common, sizeof common, "%s", line);
            first = 0;
        } else {
            while (common[k] && line[k] && common[k] == line[k]) k++;
            common[k] = '\0';
        }
    }
    if (n > 1) console_append(d, comps);
    sp = strrchr(text, ' ');
    if ((int)strlen(common) < (int)strlen(sp ? sp + 1 : text))
        snprintf(common, sizeof common, "%s", sp ? sp + 1 : text);   /* never shorter than typed */
    if (sp) sp[1] = '\0'; else text[0] = '\0';
    {
        char joined[600];
        snprintf(joined, sizeof joined, "%s%s%s", text, common, n == 1 ? " " : "");
        SetWindowTextA(d->con_in, joined);
        SendMessageA(d->con_in, EM_SETSEL, (WPARAM)strlen(joined), (LPARAM)strlen(joined));
    }
}


/* A value typed into a register box: hex ("1F", "$1F", "0x1F"), "#31" decimal,
 * or anything the engine's expressions take (a label, "hl + 2", "[$C000]"). */
static int parse_value(debugger *d, const char *text, long *out, char *err, int errsz)
{
    char buf[128], *p;
    snprintf(buf, sizeof buf, "%s", text);
    p = strip(buf);
    if (!*p) {
        snprintf(err, (size_t)errsz, "Type a value");
        return 0;
    }
    if (parse_num(p, out)) return 1;
    return msxdebug_eval(d->dbg, p, out, err, errsz) == 0;
}

static int *reg_field(msxdebug_cpu *c, int id)
{
    switch (id) {
    case MSX_REG_AF: return &c->af;
    case MSX_REG_BC: return &c->bc;
    case MSX_REG_DE: return &c->de;
    case MSX_REG_HL: return &c->hl;
    case MSX_REG_IX: return &c->ix;
    case MSX_REG_IY: return &c->iy;
    case MSX_REG_SP: return &c->sp;
    case MSX_REG_PC: return &c->pc;
    case MSX_REG_AF2: return &c->af2;
    case MSX_REG_BC2: return &c->bc2;
    case MSX_REG_DE2: return &c->de2;
    case MSX_REG_HL2: return &c->hl2;
    case MSX_REG_I: return &c->i;
    case MSX_REG_R: return &c->r;
    case MSX_REG_IM: return &c->im;
    default: return NULL;
    }
}

/* Writes the strip's CPU back (every register in one go); the reason it was
 * refused goes to the status line. */
static void regs_write(debugger *d, const msxdebug_cpu *c)
{
    char err[160];
    err[0] = '\0';
    if (!msxdebug_regs_set(d->dbg, c, err, sizeof err)) toast(d, err[0] ? err : "That register could not be set");
}

/* A register box (Enter). Refused, with a notice, unless stopped. */
static void on_reg(debugger *d, int i)
{
    char buf[64], err[160];
    long v;
    msxdebug_cpu c;
    int *field;
    edit_text(d->reg[i], buf, sizeof buf);
    err[0] = '\0';
    if (!msxdebug_is_stopped(d->dbg)) {
        toast(d, "Stop the machine first to change a register");
    } else if (!parse_value(d, buf, &v, err, sizeof err)) {
        toast(d, err[0] ? err : "Not a number (hex, # decimal, or an expression)");
    } else {
        msxdebug_cpu_get(d->dbg, &c);
        field = reg_field(&c, k_reg_ids[i]);
        if (field) *field = (int)v;
        regs_write(d, &c);
    }
    SetFocus(d->hwnd);
    resync(d);
}

static void on_check(debugger *d, int i)
{
    msxdebug_cpu c;
    const int on = SendMessageA(d->chk[i], BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (d->updating) return;
    if (!msxdebug_is_stopped(d->dbg)) {
        toast(d, "Stop the machine first to change a flag");
        refresh_cpu(d);     /* the box goes back to the CPU's */
        return;
    }
    msxdebug_cpu_get(d->dbg, &c);
    if (i == 6) c.iff1 = on;
    else if (i == 7) c.iff2 = on;
    else c.af = on ? (c.af | k_chk_bits[i]) : (c.af & ~k_chk_bits[i]);
    regs_write(d, &c);
    resync(d);
}

/* Go to: the dump from just above an address, label, expression or range, its
 * bytes selected and scrolled to. */
static void on_mem_goto(debugger *d)
{
    char buf[64];
    uint16_t low, high;
    edit_text(d->mem_goto, buf, sizeof buf);
    if (!msxsym_parse_range(d->dbg, buf, &low, &high)) {
        d->mem_hl = 0;
        refresh_mem(d);
        toast(d, "Not an address, label or range");
        return;
    }
    show_in_memory(d, low, high);
}

/* The dump a page of MEM_BYTES on or back. */
static void on_mem_page(debugger *d, int dir)
{
    d->mem_top += dir * MEM_BYTES;
    if (d->mem_top < 0) d->mem_top = 0;
    if (d->mem_top > 0x10000 - MEM_BYTES) d->mem_top = 0x10000 - MEM_BYTES;
    refresh_mem(d);
}

/* "address value [value...]": bytes into memory from the address on. */
static void on_mem_write(debugger *d)
{
    char buf[256], *tok, *save = NULL, msg[96];
    int addr, n = 0;
    edit_text(d->mem_write, buf, sizeof buf);
    tok = strtok_s(buf, " \t,", &save);
    if (!tok) return;
    addr = resolve_addr(d, tok);
    if (addr < 0) {
        toast(d, "Not an address or label");
        return;
    }
    while ((tok = strtok_s(NULL, " \t,", &save)) != NULL) {
        long v;
        if (!parse_num(tok, &v) || v < 0 || v > 0xFF) {
            toast(d, "A value is a byte (00-FF)");
            return;
        }
        msxdebug_write(d->dbg, (uint16_t)(addr + n), (uint8_t)v);
        n++;
    }
    if (!n) {
        toast(d, "Type: address value [value...]");
        return;
    }
    snprintf(msg, sizeof msg, "Wrote %d byte%s at $%04X", n, n == 1 ? "" : "s", addr);
    toast(d, msg);
    resync(d);
}

static void on_jump(debugger *d)
{
    char buf[64];
    int addr;
    edit_text(d->jump, buf, sizeof buf);
    addr = resolve_addr(d, buf);
    if (addr < 0) {
        toast(d, "Not an address or label");
        return;
    }
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
    d->disasm_top = addr;
    d->selected = addr;
    refresh_disasm(d);
}


/* The selected breakpoint row's index, or -1. */
static int bp_selected(debugger *d)
{
    const int i = (int)SendMessageA(d->bp_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    return i >= 0 && i < d->nbps ? i : -1;
}

static void on_bp_remove(debugger *d)
{
    const int i = bp_selected(d);
    if (i < 0) {
        set_label(d->bp_msg, "Select a breakpoint to remove");
        return;
    }
    set_label(d->bp_msg, "");
    msxdebug_breakpoint_remove(d->dbg, d->bps[i].id);
    resync(d);
}

static void on_bp_add(debugger *d)
{
    char a[64], b[64], cond[256], err[160];
    int start, end, sel, id, port;
    edit_text(d->bp_start, a, sizeof a);
    edit_text(d->bp_end, b, sizeof b);
    edit_text(d->bp_cond, cond, sizeof cond);
    sel = (int)SendMessageA(d->bp_type, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= NBPTYPES) sel = 0;
    port = (k_bp_type_ids[sel] & (MSXDEBUG_BP_IN | MSXDEBUG_BP_OUT)) != 0;
    start = resolve_addr(d, strip(a));
    if (start < 0) {
        set_label(d->bp_msg, port ? "The port is not a number" : "The start is not an address, label or expression");
        return;
    }
    end = strip(b)[0] ? resolve_addr(d, strip(b)) : start;
    if (end < 0) {
        set_label(d->bp_msg, "The end is not an address, label or expression");
        return;
    }
    if (end < start) end = start;
    if (port && end > 0xFF) {
        set_label(d->bp_msg, "An I/O port is $00-$FF");
        return;
    }
    err[0] = '\0';
    id = msxdebug_breakpoint_add(d->dbg, k_bp_type_ids[sel], (uint16_t)start, (uint16_t)end,
                                 strip(cond), err, sizeof err);
    if (id < 0) {
        set_label(d->bp_msg, err[0] ? err : "That breakpoint could not be set");
        return;
    }
    set_label(d->bp_msg, "");
    SetWindowTextA(d->bp_start, "");
    SetWindowTextA(d->bp_end, "");
    SetWindowTextA(d->bp_cond, "");
    resync(d);
}

/* The type decides what the boxes take. */
static void on_bp_type(debugger *d)
{
    const int sel = (int)SendMessageA(d->bp_type, CB_GETCURSEL, 0, 0);
    const int port = sel >= 4;
    SendMessageW(d->bp_start, EM_SETCUEBANNER, TRUE, (LPARAM)(port ? L"port, e.g. 98" : L"4010 / label / expression"));
    SendMessageW(d->bp_end, EM_SETCUEBANNER, TRUE, (LPARAM)(port ? L"(last port)" : L"(end)"));
}

/* ---- watches and the memory search ------------------------------------------- */

static void add_watch(debugger *d)
{
    char text[128];
    edit_text(d->wt_entry, text, sizeof text);
    if (msxwatch_add(strip(text)) < 0) {
        toast(d, "Type an address, label, expression or a range (c000-c0ff)");
        return;
    }
    SetWindowTextA(d->wt_entry, "");
    refresh_watches(d);
}

static int watch_range(debugger *d, uint16_t *low, uint16_t *high)
{
    const int sel = (int)SendMessageA(d->wt_list, LB_GETCURSEL, 0, 0);
    return sel >= 0 && msxwatch_range(d->dbg, sel, low, high);
}

/* Break on a write that changes the watched bytes. */
static void watch_break(debugger *d, uint16_t low, uint16_t high)
{
    char err[160];
    err[0] = '\0';
    if (msxdebug_breakpoint_add(d->dbg, MSXDEBUG_BP_WRITE, low, high, "value != old", err, sizeof err) >= 0)
        toast(d, "Breaking when it changes");
    else
        toast(d, err[0] ? err : "That breakpoint could not be set");
    resync(d);
}

static void do_search(debugger *d, int backward)
{
    char text[200];
    uint8_t pat[MSXSEARCH_MAX], mask;
    uint16_t found;
    int count, flags, r;

    edit_text(d->srch_entry, text, sizeof text);
    if (!msxsearch_parse((int)SendMessageA(d->srch_kind, CB_GETCURSEL, 0, 0), text, pat, &count, &mask, &flags)) {
        set_label(d->srch_note, "Type hex bytes (3E 00) or some text.");
        return;
    }
    if (!msxdebug_is_stopped(d->dbg)) {
        set_label(d->srch_note, "Stop the machine to search its memory.");
        return;
    }
    r = msxdebug_search_next(d->dbg, pat, count, mask, flags,
                             (uint16_t)(d->mem_hl ? (backward ? d->mem_hl_lo : d->mem_hl_lo + 1) : d->mem_top),
                             backward, &found);
    if (r > 0) {
        char note[40];
        snprintf(note, sizeof note, "Found at $%04X", found);
        set_label(d->srch_note, note);
        show_in_memory(d, found, found + count - 1);
    } else {
        set_label(d->srch_note, r < 0 ? "Stop the machine to search its memory." : "Not found.");
    }
}

static int pick_file(debugger *d, int save, const char *title, const char *filter, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = d->hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    if (save) {
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        return GetSaveFileNameA(&ofn) ? 1 : 0;
    }
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static void on_symbols(debugger *d)
{
    char path[MAX_PATH], msg[512];
    if (!pick_file(d, 0, "Load symbols",
                   "Symbol files (*.sym;*.map;*.noi;*.txt)\0*.sym;*.map;*.noi;*.txt\0All files\0*.*\0\0",
                   path, sizeof path))
        return;
    msg[0] = '\0';
    if (msxdebug_load_symbols(d->dbg, path, msg, sizeof msg) < 0 && !msg[0])
        snprintf(msg, sizeof msg, "No symbols found in %s", base_name(path));
    say(d, msg);
    resync(d);
}

static void on_save(debugger *d, int which)
{
    char path[MAX_PATH], msg[512], title[64];
    snprintf(title, sizeof title, "Save %s", k_save_titles[which]);
    if (!pick_file(d, 1, title, "All files\0*.*\0\0", path, sizeof path)) return;
    msg[0] = '\0';
    msxdebug_save(d->dbg, k_save_kinds[which], path, msg, sizeof msg);
    if (!msg[0]) snprintf(msg, sizeof msg, "Saved %s", base_name(path));
    say(d, msg);
}

static void save_menu(debugger *d)
{
    HMENU m = CreatePopupMenu();
    RECT r;
    int i;
    for (i = 0; i < NSAVE; i++) AppendMenuA(m, MF_STRING, (UINT_PTR)(IDC_SAVE_KIND0 + i), k_save_titles[i]);
    GetWindowRect(d->save_btn, &r);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, d->hwnd, NULL);
    DestroyMenu(m);
}

static void on_accept(debugger *d, int id)
{
    if (id >= IDC_REG0 && id <= IDC_REG_LAST) {
        on_reg(d, id - IDC_REG0);
        return;
    }
    switch (id) {
    case IDC_CON_IN: on_console(d); break;
    case IDC_MEM_GOTO: on_mem_goto(d); break;
    case IDC_MEM_WRITE: on_mem_write(d); break;
    case IDC_SRCH_ENTRY: do_search(d, 0); break;
    case IDC_JUMP: on_jump(d); break;
    case IDC_BP_START:
    case IDC_BP_END:
    case IDC_BP_COND: on_bp_add(d); break;
    case IDC_WT_ENTRY: add_watch(d); break;
    case IDC_SYM_FILTER: refresh_symbols(d); break;
    case IDC_H_RANGE0: hist_refresh(d, 0, 1); break;
    case IDC_H_RANGE1: hist_refresh(d, 1, 1); break;
    default: break;
    }
}

/* ---- the disassembly -------------------------------------------------------------- */

/* Drawn by hand, one row per line: the breakpoint gutter (a red dot when the
 * breakpoint is enabled, grey when disabled), address, bytes, label,
 * instruction, and the comment (a jump target's label) in grey. The PC's
 * line is filled with the accent, the selected one with the selection
 * colour. */
static void paint_disasm(debugger *d, HWND hwnd, HDC dc)
{
    RECT c;
    int i;
    const int gutter = S(d, GUTTER_W);
    GetClientRect(hwnd, &c);
    FillRect(dc, &c, GetSysColorBrush(COLOR_WINDOW));
    SelectObject(dc, d->mono);
    SetBkMode(dc, TRANSPARENT);
    if (d->line_count == 0) {
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        TextOutA(dc, gutter, 0, "(no disassembly)", 16);
        return;
    }
    for (i = 0; i < d->line_count && i * d->line_h < c.bottom; i++) {
        const msxdebug_line *l = &d->lines[i];
        const int y = i * d->line_h, ty = y + (d->line_h - d->mono_h) / 2;
        const int is_pc = l->is_pc, sel = l->address == d->selected;
        COLORREF fg = GetSysColor(COLOR_WINDOWTEXT);
        RECT row;
        char text[240];
        int len;
        SetRect(&row, 0, y, c.right, y + d->line_h);
        if (is_pc) {
            FillRect(dc, &row, d->accent);
            fg = INK_RGB;
        } else if (sel) {
            FillRect(dc, &row, GetSysColorBrush(COLOR_HIGHLIGHT));
            fg = GetSysColor(COLOR_HIGHLIGHTTEXT);
        }
        if (l->has_breakpoint) {
            const int size = d->line_h - S(d, 6) > 4 ? d->line_h - S(d, 6) : 4;
            HGDIOBJ oldb = SelectObject(dc, l->breakpoint_enabled ? d->bp_on : d->bp_off);
            HGDIOBJ oldp = SelectObject(dc, is_pc ? GetStockObject(WHITE_PEN) : GetStockObject(NULL_PEN));
            Ellipse(dc, S(d, 4), y + (d->line_h - size) / 2, S(d, 4) + size + 1, y + (d->line_h - size) / 2 + size + 1);
            SelectObject(dc, oldp);
            SelectObject(dc, oldb);
        }
        len = snprintf(text, sizeof text, "%04X  %-12.12s %-18.18s %s", l->address, l->bytes, l->label, l->disasm);
        if (len > (int)sizeof text - 1) len = (int)sizeof text - 1;
        if (len < 0) len = 0;
        SetTextColor(dc, fg);
        TextOutA(dc, gutter, ty, text, len);
        if (l->comment[0]) {
            len = snprintf(text, sizeof text, "; %s", l->comment);
            if (len > (int)sizeof text - 1) len = (int)sizeof text - 1;
            if (!is_pc && !sel) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            TextOutA(dc, gutter + d->mono_w * COMMENT_COL, ty, text, len);
        }
    }
}

/* A click selects a line (it works while the machine runs, too); a click on
 * the gutter, or a double-click, toggles its breakpoint. The wheel and the
 * keys browse by instructions (and turn Follow PC off). */
static LRESULT CALLBACK disasm_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (!d) return DefWindowProcA(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right > 0 ? c.right : 1, c.bottom > 0 ? c.bottom : 1);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, d->mono);
        paint_disasm(d, hwnd, mem);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        paint_disasm(d, hwnd, (HDC)wp);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        /* more or fewer rows fit now */
        if (d->disasm == hwnd && page_visible(d, TAB_DISASM)) refresh_disasm(d);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const int line = d->line_h > 0 ? GET_Y_LPARAM(lp) / d->line_h : -1;
        const int gutter = GET_X_LPARAM(lp) < S(d, GUTTER_W);
        SetFocus(hwnd);
        if (line < 0 || line >= d->line_count) return 0;
        d->selected = d->lines[line].address;
        if (gutter ? msg == WM_LBUTTONDOWN : msg == WM_LBUTTONDBLCLK)
            toggle_breakpoint(d, d->lines[line].address);
        else
            InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int rows = -delta * 3 / WHEEL_DELTA;
        if (!rows) rows = delta > 0 ? -1 : 1;
        SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
        d->disasm_top = row_address(d, d->disasm_top, rows);
        refresh_disasm(d);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_PRIOR || wp == VK_NEXT || wp == VK_UP || wp == VK_DOWN) {
            const int half = disasm_rows(d) / 2 > 0 ? disasm_rows(d) / 2 : 1;
            const int rows = wp == VK_PRIOR ? -half : wp == VK_NEXT ? half : wp == VK_UP ? -1 : 1;
            SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
            d->disasm_top = row_address(d, d->disasm_top, rows);
            refresh_disasm(d);
            return 0;
        }
        break;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- edit subclassing ------------------------------------------------------------- */

static WNDPROC g_edit_proc;
static WNDPROC g_sym_proc;

/* Enter in a single-line field means "apply this value"; Tab in the console
 * completes and Up/Down recall. None may beep its way through the default
 * handler. The notifications go to the debugger window, whatever window the
 * field's tab is in. */
static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const int id = (int)GetWindowLongPtrA(hwnd, GWLP_ID);
    HWND main = g_dbg ? g_dbg->hwnd : GetAncestor(hwnd, GA_ROOT);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageA(main, WM_DBG_ACCEPT, (WPARAM)id, 0);
        return 0;
    }
    if (msg == WM_CHAR && (wp == '\r' || wp == '\n')) return 0;
    if (id == IDC_CON_IN) {
        if (msg == WM_KEYDOWN && wp == VK_TAB) {
            PostMessageA(main, WM_DBG_TAB, 0, 0);
            return 0;
        }
        if (msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN)) {
            PostMessageA(main, WM_DBG_HISTORY, wp == VK_DOWN, 0);
            return 0;
        }
        if (msg == WM_CHAR && wp == VK_TAB) return 0;
        if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    }
    return CallWindowProcA(g_edit_proc, hwnd, msg, wp, lp);
}

/* A click on a symbol's line shows its address in Memory. The lines come
 * from a filtered list whose indices move, so the address of each line was
 * kept when the list was built (sym_addr); what is clicked is a line. */
static LRESULT CALLBACK sym_list_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_LBUTTONDOWN && g_dbg) {
        const int line = (int)HIWORD(SendMessageA(hwnd, EM_CHARFROMPOS, 0, lp));
        if (line >= 0 && line < g_dbg->sym_lines)
            PostMessageA(g_dbg->hwnd, WM_DBG_GOTO, (WPARAM)g_dbg->sym_addr[line], 0);
    }
    return CallWindowProcA(g_sym_proc, hwnd, msg, wp, lp);
}

/* ---- construction ----------------------------------------------------------------- */

static HWND child_ex(debugger *d, DWORD ex, const char *cls, const char *text, DWORD style, int id, HFONT font)
{
    HWND h = CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                             d->new_parent ? d->new_parent : d->hwnd, (HMENU)(INT_PTR)id,
                             (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE), NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

static HWND child(debugger *d, const char *cls, const char *text, DWORD style, int id, HFONT font)
{
    return child_ex(d, 0, cls, text, style, id, font);
}

/* A read-only monospace text view. */
static HWND view(debugger *d, int id, DWORD extra)
{
    HWND h = child(d, "EDIT", "", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | extra,
                   id, d->mono);
    SendMessageA(h, EM_SETLIMITTEXT, 0, 0);
    if (d->nviews < (int)(sizeof d->views / sizeof d->views[0])) d->views[d->nviews++] = h;
    return h;
}

static HWND field(debugger *d, int id, const wchar_t *cue)
{
    HWND h = child(d, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, id, d->mono);
    g_edit_proc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)edit_subclass);
    if (cue) SendMessageW(h, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
    return h;
}

static HWND label(debugger *d, const char *text, HFONT font, DWORD style)
{
    return child(d, "STATIC", text, style | SS_NOPREFIX, IDC_LABEL_FIRST + d->nlabels++, font);
}

/* A caption or hint, drawn grey. */
static HWND dim_label(debugger *d, const char *text, DWORD style)
{
    HWND h = label(d, text, d->ui, style);
    if (d->ndim < (int)(sizeof d->dim / sizeof d->dim[0])) d->dim[d->ndim++] = h;
    return h;
}

static HWND button(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, id, d->ui);
}

static HWND checkbox(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, id, d->ui);
}

static HWND combo(debugger *d, int id)
{
    return child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id, d->ui);
}

/* A list box of lines in the monospace font. */
static HWND list_box(debugger *d, int id)
{
    return child_ex(d, WS_EX_CLIENTEDGE, "LISTBOX", "", WS_VSCROLL | WS_HSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                    id, d->mono);
}

static void size_columns(debugger *d)
{
    static const int widths[5] = { 70, 90, 150, 380, 80 };
    int i;
    for (i = 0; i < 5; i++)
        SendMessageA(d->bp_list, LVM_SETCOLUMNWIDTH, (WPARAM)i, S(d, widths[i]));
}

/* A tab control reads '&' as a mnemonic, so the label's own is doubled. */
static const char *tab_text(int page, char *buf, size_t cap)
{
    const char *in = k_tabs[page].label;
    size_t o = 0;
    for (; *in && o + 3 < cap; in++) {
        buf[o++] = *in;
        if (*in == '&') buf[o++] = '&';
    }
    buf[o] = '\0';
    return buf;
}

static void build_controls(debugger *d)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE);
    TCITEMA item;
    int i;

    d->tabs = child(d, WC_TABCONTROLA, "", WS_CLIPSIBLINGS | WS_TABSTOP, IDC_TABS, d->ui);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (i = 0; i < TAB_COUNT; i++) {
        char text[40];
        item.pszText = (char *)tab_text(i, text, sizeof text);
        SendMessageA(d->tabs, TCM_INSERTITEMA, (WPARAM)i, (LPARAM)&item);
        d->tab_page[i] = i;
    }
    d->ntabs = TAB_COUNT;
    for (i = 0; i < TAB_COUNT; i++) {       /* a container per tab: its controls are its children */
        d->pages[i] = CreateWindowExA(0, PAGE_CLASS, "", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, d->hwnd,
                                   NULL, inst, NULL);
        SetWindowLongPtrA(d->pages[i], GWLP_USERDATA, (LONG_PTR)d->hwnd);
    }

    /* The toolbar. Run/Stop is owner-drawn so it can wear the accent colour
     * while the machine is stopped. */
    d->run_btn = child(d, "BUTTON", "Stop (F5)", BS_OWNERDRAW | WS_TABSTOP, IDC_RUN, d->ui);
    d->step_btn = button(d, "Step (F7)", IDC_STEP);
    d->over_btn = button(d, "Step Over (F8)", IDC_OVER);
    d->out_btn = button(d, "Step Out (Shift+F8)", IDC_OUT);
    d->run_to_btn = button(d, "Run to Cursor", IDC_RUN_TO);
    d->frame_btn = button(d, "Frame (Shift+F7)", IDC_FRAME);
    d->sym_btn = button(d, "Symbols...", IDC_SYMBOLS);
    d->save_btn = button(d, "Save...", IDC_SAVE);
    d->popout_btn = button(d, "Pop Out", IDC_POPOUT);
    d->help_btn = button(d, "Help", IDC_HELP_BTN);
    d->status = child(d, "STATIC", "", SS_RIGHT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_STATUS, d->ui);
    if (d->ndim < (int)(sizeof d->dim / sizeof d->dim[0])) d->dim[d->ndim++] = d->status;

    /* The register strip, under every tab. */
    for (i = 0; i < NREGS; i++) {
        d->reg_label[i] = dim_label(d, k_reg_names[i], SS_RIGHT);
        d->reg[i] = field(d, IDC_REG0 + i, NULL);
        SendMessageA(d->reg[i], EM_SETLIMITTEXT, 8, 0);
    }
    for (i = 0; i < NCHECKS; i++) d->chk[i] = checkbox(d, k_chk_names[i], IDC_CHK0 + i);
    d->halt_label = label(d, "", d->ui_bold, SS_LEFT);
    d->cpu_hint = dim_label(d, "Enter sets a register (hex, # decimal or an expression); they change only while the machine is stopped.",
                            SS_LEFT | SS_ENDELLIPSIS);

    /* Console */
    d->new_parent = d->pages[TAB_CONSOLE];
    d->con_out = view(d, IDC_CON_OUT, 0);
    SetWindowTextA(d->con_out, "MSX debugger console. Type 'help' for every command; Tab completes, Up/Down "
                               "recall. Anything else is a Tcl command for openMSX's own console.\r\n");
    d->con_prompt = child(d, "STATIC", ">", SS_LEFT, IDC_CON_PROMPT, d->mono);
    d->con_in = field(d, IDC_CON_IN, L"help, step, over, break 4010, bpa c000 if value == $ff, mem c000, watch, video, ...");

    /* Disassembly */
    d->new_parent = d->pages[TAB_DISASM];
    d->follow_pc = checkbox(d, "Follow PC", IDC_FOLLOW_PC);
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_CHECKED, 0);
    d->jump_label = dim_label(d, "Jump to", SS_LEFT);
    d->jump = field(d, IDC_JUMP, L"4010 or a label");
    d->disasm_hint = dim_label(d, "Click a line to select it (Run to Cursor); click the gutter, double-click or "
                                  "press F9 for a breakpoint; scroll to browse",
                               SS_LEFT | SS_ENDELLIPSIS);
    d->disasm = CreateWindowExA(WS_EX_CLIENTEDGE, DISASM_CLASS, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10,
                                d->pages[TAB_DISASM], (HMENU)(INT_PTR)IDC_DISASM, inst, NULL);

    /* Memory: Go to, Write (poke), the search, the dump */
    d->new_parent = d->pages[TAB_MEMORY];
    d->goto_label = dim_label(d, "Go to", SS_LEFT);
    d->mem_goto = field(d, IDC_MEM_GOTO, L"c000 / label / c000-c0ff");
    d->mem_back = button(d, "Back 4K", IDC_MEM_BACK);
    d->mem_on = button(d, "On 4K", IDC_MEM_ON);
    d->write_label = dim_label(d, "Poke", SS_LEFT);
    d->mem_write = field(d, IDC_MEM_WRITE, L"address value [value...]");
    d->srch_label = dim_label(d, "Search", SS_LEFT);
    d->srch_kind = combo(d, IDC_SRCH_KIND);
    SendMessageA(d->srch_kind, CB_ADDSTRING, 0, (LPARAM)"Hex bytes");
    SendMessageA(d->srch_kind, CB_ADDSTRING, 0, (LPARAM)"Text");
    SendMessageA(d->srch_kind, CB_SETCURSEL, 0, 0);
    d->srch_entry = field(d, IDC_SRCH_ENTRY, L"3E 00 / ABC");
    d->srch_next = button(d, "Find Next", IDC_SRCH_NEXT);
    d->srch_prev = button(d, "Find Previous", IDC_SRCH_PREV);
    d->srch_note = dim_label(d, "", SS_LEFT | SS_ENDELLIPSIS);
    d->mem_view = view(d, IDC_MEM_VIEW, WS_HSCROLL | ES_NOHIDESEL);

    /* Breakpoints */
    d->new_parent = d->pages[TAB_BREAKS];
    d->bp_hint = dim_label(d, "Click a disassembly line's gutter to set an execute breakpoint; the box enables it.",
                           SS_LEFT | SS_ENDELLIPSIS);
    d->bp_remove = button(d, "Remove", IDC_BP_REMOVE);
    d->bp_hits = button(d, "Clear Hits", IDC_BP_HITS);
    d->bp_clear = button(d, "Clear All", IDC_BP_CLEAR);
    d->bp_list = child_ex(d, WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
                          WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | LVS_NOSORTHEADER,
                          IDC_BP_LIST, d->mono);
    SendMessageA(d->bp_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    {
        static const char *const cols[5] = { "#", "Type", "Range", "Condition", "Hits" };
        LVCOLUMNA col;
        memset(&col, 0, sizeof col);
        col.mask = LVCF_TEXT;
        for (i = 0; i < 5; i++) {
            col.pszText = (char *)cols[i];
            SendMessageA(d->bp_list, LVM_INSERTCOLUMNA, (WPARAM)i, (LPARAM)&col);
        }
        size_columns(d);
    }
    d->bp_empty = dim_label(d, "No breakpoints. Click the gutter of a disassembly line, or add one below.", SS_CENTER);
    d->bp_type = combo(d, IDC_BP_TYPE);
    for (i = 0; i < NBPTYPES; i++) SendMessageA(d->bp_type, CB_ADDSTRING, 0, (LPARAM)k_bp_types[i]);
    SendMessageA(d->bp_type, CB_SETCURSEL, 0, 0);
    d->bp_start = field(d, IDC_BP_START, NULL);
    d->bp_end = field(d, IDC_BP_END, NULL);
    d->bp_cond = field(d, IDC_BP_COND, L"condition, e.g. value != old  (value, old, addr, a, hl, [addr], or Tcl)");
    d->bp_add = button(d, "Add", IDC_BP_ADD);
    d->bp_msg = label(d, "", d->ui, SS_LEFT | SS_ENDELLIPSIS);
    d->bp_stop = dim_label(d, "", SS_LEFT | SS_ENDELLIPSIS);
    on_bp_type(d);

    /* Watches */
    d->new_parent = d->pages[TAB_WATCHES];
    d->wt_label = dim_label(d, "Watch", SS_LEFT);
    d->wt_entry = field(d, IDC_WT_ENTRY, L"an address, label, expression or range (score, c000-c0ff, [hl])");
    d->wt_add = button(d, "Add", IDC_WT_ADD);
    d->wt_list = list_box(d, IDC_WT_LIST);
    d->wt_mem = button(d, "Show in Memory", IDC_WT_MEM);
    d->wt_brk = button(d, "Break on Change", IDC_WT_BRK);
    d->wt_remove = button(d, "Remove", IDC_WT_REMOVE);

    /* Cart & I/O */
    d->new_parent = d->pages[TAB_CART];
    d->cart_text = view(d, IDC_CART_TEXT, WS_HSCROLL);
    d->io_text = view(d, IDC_IO_TEXT, WS_HSCROLL);

    /* Trace (0, live) and History (1, when stopped) share one ring */
    for (i = 0; i < 2; i++) {
        d->new_parent = d->pages[TAB_TRACE + i];
        d->h_rec[i] = child(d, "BUTTON", "Record", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, IDC_H_REC0 + i, d->ui);
        d->h_filter[i] = combo(d, IDC_H_FILTER0 + i);
        SendMessageA(d->h_filter[i], CB_ADDSTRING, 0, (LPARAM)"All instructions");
        SendMessageA(d->h_filter[i], CB_ADDSTRING, 0, (LPARAM)"Branches only");
        SendMessageA(d->h_filter[i], CB_ADDSTRING, 0, (LPARAM)"In range");
        SendMessageA(d->h_filter[i], CB_SETCURSEL, 0, 0);
        d->h_range[i] = field(d, IDC_H_RANGE0 + i, L"range, e.g. 4000-40ff");
        d->h_clear[i] = button(d, "Clear", IDC_H_CLEAR0 + i);
        d->h_note[i] = dim_label(d, "", SS_RIGHT | SS_ENDELLIPSIS);
        d->h_text[i] = view(d, IDC_H_TEXT0 + i, WS_HSCROLL);
        d->h_seen[i] = (uint64_t)-1;
    }

    /* Video: the shared controller's tab, one child holding all its controls */
    d->new_parent = NULL;
    d->video = dbg_video_create(d->pages[TAB_VIDEO], d->dbg, d->ui, d->mono, d->dpi);

    /* Sound */
    d->new_parent = d->pages[TAB_SOUND];
    d->sound_text = view(d, IDC_SOUND_TEXT, WS_HSCROLL);

    /* Symbols */
    d->new_parent = d->pages[TAB_SYMBOLS];
    d->sym_filter_label = dim_label(d, "Filter", SS_LEFT);
    d->sym_filter = field(d, IDC_SYM_FILTER, L"part of a name");
    d->sym_count = dim_label(d, "", SS_LEFT);
    d->sym_list = view(d, IDC_SYM_LIST, WS_HSCROLL);
    g_sym_proc = (WNDPROC)SetWindowLongPtrA(d->sym_list, GWLP_WNDPROC, (LONG_PTR)sym_list_proc);

    /* FujiNet: the note and the log are UTF-8 (see wide_view_text) */
    d->new_parent = d->pages[TAB_FUJINET];
    d->fuji_title = label(d, "FujiBus packets (SLIP-decoded)", d->ui_bold, SS_LEFT);
    d->fuji_note = dim_label(d, "", SS_LEFT | SS_ENDELLIPSIS);
    d->fuji_clear = button(d, "Clear", IDC_FUJI_CLEAR);
    d->fuji_text = view(d, IDC_FUJI_TEXT, WS_HSCROLL);
    d->fuji_state_title = label(d, "FujiNet cartridge", d->ui_bold, SS_LEFT);
    d->fuji_state = view(d, IDC_FUJI_STATE, WS_HSCROLL);

    /* The tab control was made first, so it sits below the pages' containers
     * already; with WS_CLIPSIBLINGS and the bottom of the z-order a repaint of
     * it never paints its body over a page. */
    d->new_parent = NULL;
    SetWindowPos(d->tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/* Help's contexts (msxhelp.h), one per tab */
static const char *const k_tab_contexts[TAB_COUNT] = {
    "debugger/console", "debugger/disassembly", "debugger/memory", "debugger/breakpoints", "debugger/watches",
    "debugger/cart", "debugger/trace", "debugger/history", "debugger/video", "debugger/sound",
    "debugger/symbols", "debugger/fujinet",
};

/* On screen: the tab the tab control shows, or one living in a window of its own. */
static int page_visible(debugger *d, int page)
{
    return d->popped[page] != NULL || d->page == page;
}

static void show_page(debugger *d, int page)
{
    int p;
    d->page = page;
    /* Help, F1 and Ctrl+Shift+H open the tab's section */
    if (page >= 0 && page < TAB_COUNT) msx_help_set_context(d->hwnd, k_tab_contexts[page], 1);
    for (p = 0; p < TAB_COUNT; p++)     /* a tab's controls follow its container */
        ShowWindow(d->pages[p], d->popped[p] || p == page ? SW_SHOW : SW_HIDE);
    show_bp_list(d);
}

/* The tab control's index for a tab, or -1 when it is in a window of its own. */
static int tab_index(debugger *d, int page)
{
    int i;
    for (i = 0; i < d->ntabs; i++)
        if (d->tab_page[i] == page) return i;
    return -1;
}

static void place(HWND h, int x, int y, int w, int hgt);
static void layout(debugger *d);

/* Brings a tab to the front: its window if it has one, else the tab control's. */
static void goto_page(debugger *d, int page)
{
    const int i = tab_index(d, page);
    if (d->popped[page]) {
        SetForegroundWindow(d->popped[page]);
        return;
    }
    if (i >= 0) {
        SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)i, 0);
        show_page(d, page);
        layout(d);
        refresh_page(d, page, 1);
    }
}

/* ---- peeling a tab off into a window of its own, and putting it back ----------- */

static void dock_page(debugger *d, int page)
{
    TCITEMA item;
    char shown[40];
    HWND pop = d->popped[page];
    int pos = 0, p, i;

    if (!pop) return;
    d->popped[page] = NULL;
    SetParent(d->pages[page], d->hwnd);
    for (p = 0; p < page; p++)          /* back where it was in the order */
        if (!d->popped[p]) pos++;
    for (i = d->ntabs; i > pos; i--) d->tab_page[i] = d->tab_page[i - 1];
    d->tab_page[pos] = page;
    d->ntabs++;
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    item.pszText = (char *)tab_text(page, shown, sizeof shown);
    SendMessageA(d->tabs, TCM_INSERTITEMA, (WPARAM)pos, (LPARAM)&item);
    DestroyWindow(pop);
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)pos, 0);
    show_page(d, page);
    layout(d);
    refresh_page(d, page, 1);
}

static void pop_out(debugger *d, int page)
{
    HWND pop;
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE);
    char title[96];
    int idx, i, next;

    if (page < 0 || page >= TAB_COUNT || d->popped[page] || d->ntabs <= 1) return;   /* one tab always stays */
    idx = tab_index(d, page);
    if (idx < 0) return;
    SendMessageA(d->tabs, TCM_DELETEITEM, (WPARAM)idx, 0);
    for (i = idx; i < d->ntabs - 1; i++) d->tab_page[i] = d->tab_page[i + 1];
    d->ntabs--;

    snprintf(title, sizeof title, "MSX Debugger - %s", k_tabs[page].label);
    pop = CreateWindowExA(0, POP_CLASS, title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                          S(d, 900), S(d, 620), d->hwnd, NULL, inst, NULL);
    SetWindowLongPtrA(pop, GWLP_USERDATA, page);
    msx_help_set_context(pop, k_tab_contexts[page], 1);       /* F1 in the window opens its tab's section */
    {
        HWND dock = CreateWindowExA(0, "BUTTON", "Dock", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, S(d, 4),
                                    S(d, 4), S(d, 70), S(d, 26), pop, (HMENU)(INT_PTR)IDC_DOCK, inst, NULL);
        SendMessageA(dock, WM_SETFONT, (WPARAM)d->ui, TRUE);
    }
    SetParent(d->pages[page], pop);
    d->popped[page] = pop;
    if (page == d->page) {              /* the tab in front left: the one after it comes forward */
        next = idx < d->ntabs ? idx : d->ntabs - 1;
        SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)next, 0);
        show_page(d, d->tab_page[next]);
    } else {                            /* another tab left: the one in front stays */
        SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)tab_index(d, d->page), 0);
        show_page(d, d->page);
    }
    layout(d);
    ShowWindow(pop, SW_SHOW);
    refresh_page(d, page, 1);
    refresh_page(d, d->page, 1);
}

/* A tab's container: its controls' notifications go to the debugger window,
 * whichever window the container is in. */
static LRESULT CALLBACK page_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND main = (HWND)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_DRAWITEM:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (main) return SendMessageA(main, msg, wp, lp);
        break;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* The window a peeled-off tab lives in. */
static LRESULT CALLBACK pop_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const int page = (int)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_SIZE:
        if (g_dbg) layout(g_dbg);
        return 0;
    case WM_COMMAND:
        if (g_dbg && LOWORD(wp) == IDC_DOCK) dock_page(g_dbg, page);
        return 0;
    case WM_CLOSE:
        if (g_dbg) dock_page(g_dbg, page);
        else DestroyWindow(hwnd);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = MulDiv(420, g_dbg && g_dbg->dpi ? g_dbg->dpi : 96, 96);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = MulDiv(300, g_dbg && g_dbg->dpi ? g_dbg->dpi : 96, 96);
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- layout ----------------------------------------------------------------------- */

static void place(HWND h, int x, int y, int w, int hgt)
{
    MoveWindow(h, x, y, w > 1 ? w : 1, hgt > 1 ? hgt : 1, TRUE);
}

/* The controls of each tab inside its container. */
static void layout_pages(debugger *d)
{
    const int bh = S(d, 26), eh = S(d, 24), lh = S(d, 18), gap = S(d, 6);
    RECT r;
    int p, w, h, x, y, i, bw;

    for (p = 0; p < TAB_COUNT; p++) {
        if (!d->pages[p]) continue;
        GetClientRect(d->pages[p], &r);
        w = r.right;
        h = r.bottom;
        if (w < 40 || h < 40) continue;
        switch (p) {
        case TAB_CONSOLE:
            bw = d->mono_w + S(d, 6);
            place(d->con_out, 0, 0, w, h - eh - S(d, 8));
            place(d->con_prompt, 0, h - eh + (eh - d->mono_h) / 2, bw, d->mono_h);
            place(d->con_in, bw + S(d, 2), h - eh, w - bw - S(d, 2), eh);
            break;
        case TAB_DISASM:
            bw = text_w(d, d->ui, "Follow PC") + S(d, 24);
            place(d->follow_pc, 0, S(d, 1), bw, S(d, 22));
            x = bw + S(d, 12);
            bw = text_w(d, d->ui, "Jump to") + S(d, 6);
            place(d->jump_label, x, (eh - lh) / 2, bw, lh);
            x += bw;
            place(d->jump, x, 0, S(d, 150), eh);
            x += S(d, 162);
            place(d->disasm_hint, x, (eh - lh) / 2, w - x, lh);
            place(d->disasm, 0, eh + S(d, 6), w, h - eh - S(d, 6));
            break;
        case TAB_MEMORY:
            x = 0;
            bw = text_w(d, d->ui, "Go to") + S(d, 6);
            place(d->goto_label, x, (eh - lh) / 2, bw, lh);
            x += bw;
            place(d->mem_goto, x, 0, S(d, 190), eh);
            x += S(d, 196);
            bw = btn_w(d, "Back 4K");
            place(d->mem_back, x, -S(d, 1), bw, bh);
            x += bw + S(d, 4);
            bw = btn_w(d, "On 4K");
            place(d->mem_on, x, -S(d, 1), bw, bh);
            x += bw + S(d, 24);
            bw = text_w(d, d->ui, "Poke") + S(d, 6);
            place(d->write_label, x, (eh - lh) / 2, bw, lh);
            x += bw;
            place(d->mem_write, x, 0, w - x > S(d, 120) ? (w - x < S(d, 300) ? w - x : S(d, 300)) : S(d, 120), eh);
            y = bh + gap;
            x = 0;
            bw = text_w(d, d->ui, "Search") + S(d, 6);
            place(d->srch_label, x, y + (eh - lh) / 2, bw, lh);
            x += bw;
            place(d->srch_kind, x, y, S(d, 110), S(d, 200));
            x += S(d, 116);
            place(d->srch_entry, x, y, S(d, 230), eh);
            x += S(d, 236);
            bw = btn_w(d, "Find Next");
            place(d->srch_next, x, y - S(d, 1), bw, bh);
            x += bw + S(d, 4);
            bw = btn_w(d, "Find Previous");
            place(d->srch_prev, x, y - S(d, 1), bw, bh);
            x += bw + S(d, 12);
            place(d->srch_note, x, y + (eh - lh) / 2, w - x, lh);
            y += bh + gap;
            place(d->mem_view, 0, y, w, h - y);
            break;
        case TAB_BREAKS: {
            const int stop_y = h - lh, msg_y = stop_y - lh - S(d, 2), add_y = msg_y - S(d, 8) - eh;
            const int list_y = bh + gap;
            int cw = btn_w(d, "Clear All"), hw = btn_w(d, "Clear Hits"), rw = btn_w(d, "Remove");
            place(d->bp_clear, w - cw, 0, cw, bh);
            place(d->bp_hits, w - cw - gap - hw, 0, hw, bh);
            place(d->bp_remove, w - cw - hw - 2 * gap - rw, 0, rw, bh);
            place(d->bp_hint, 0, (bh - lh) / 2, w - cw - hw - rw - 4 * gap, lh);
            place(d->bp_list, 0, list_y, w, add_y - gap - list_y);
            place(d->bp_empty, 0, list_y + S(d, 12), w, lh);
            x = 0;
            place(d->bp_type, x, add_y, S(d, 110), S(d, 240));
            x += S(d, 116);
            place(d->bp_start, x, add_y, S(d, 140), eh);
            x += S(d, 146);
            place(d->bp_end, x, add_y, S(d, 110), eh);
            x += S(d, 116);
            bw = btn_w(d, "Add");
            place(d->bp_cond, x, add_y, w - x - bw - gap, eh);
            place(d->bp_add, w - bw, add_y - S(d, 1), bw, bh);
            place(d->bp_msg, 0, msg_y, w, lh);
            place(d->bp_stop, 0, stop_y, w, lh);
            break;
        }
        case TAB_WATCHES: {
            const int bottom = h - bh;
            bw = text_w(d, d->ui, "Watch") + S(d, 6);
            place(d->wt_label, 0, (eh - lh) / 2, bw, lh);
            i = btn_w(d, "Add");
            place(d->wt_entry, bw, 0, w - bw - i - gap, eh);
            place(d->wt_add, w - i, -S(d, 1), i, bh);
            place(d->wt_list, 0, bh + gap, w, bottom - bh - 2 * gap);
            x = 0;
            bw = btn_w(d, "Show in Memory");
            place(d->wt_mem, x, bottom, bw, bh);
            x += bw + gap;
            bw = btn_w(d, "Break on Change");
            place(d->wt_brk, x, bottom, bw, bh);
            x += bw + gap;
            bw = btn_w(d, "Remove");
            place(d->wt_remove, x, bottom, bw, bh);
            break;
        }
        case TAB_CART: {
            const int left = (w - gap) * 2 / 5;
            place(d->cart_text, 0, 0, left, h);
            place(d->io_text, left + gap, 0, w - left - gap, h);
            break;
        }
        case TAB_TRACE:
        case TAB_HISTORY:
            i = p - TAB_TRACE;
            x = 0;
            bw = btn_w(d, "Record");
            place(d->h_rec[i], x, 0, bw, bh);
            x += bw + gap;
            place(d->h_filter[i], x, 1, S(d, 150), S(d, 200));
            x += S(d, 156);
            place(d->h_range[i], x, 1, S(d, 190), eh);
            x += S(d, 196);
            bw = btn_w(d, "Clear");
            place(d->h_clear[i], x, 0, bw, bh);
            x += bw + gap;
            place(d->h_note[i], x, (bh - lh) / 2, w - x, lh);
            place(d->h_text[i], 0, bh + gap, w, h - bh - gap);
            break;
        case TAB_VIDEO:
            if (d->video) dbg_video_place(d->video, 0, 0, w, h);
            break;
        case TAB_SOUND:
            place(d->sound_text, 0, 0, w, h);
            break;
        case TAB_SYMBOLS:
            bw = text_w(d, d->ui, "Filter") + S(d, 6);
            place(d->sym_filter_label, 0, (eh - lh) / 2, bw, lh);
            place(d->sym_filter, bw, 0, S(d, 240), eh);
            place(d->sym_count, bw + S(d, 250), (eh - lh) / 2, S(d, 240), lh);
            place(d->sym_list, 0, eh + gap, w, h - eh - gap);
            break;
        case TAB_FUJINET: {
            /* the packets left (a note and Clear over them), the cartridge's link and counts right */
            const int rw = w / 3 > S(d, 340) ? S(d, 340) : w / 3 > S(d, 220) ? w / 3 : S(d, 220);
            const int lw = w - rw - S(d, 12);
            const int cw = btn_w(d, "Clear");
            place(d->fuji_title, 0, 0, lw, lh);
            place(d->fuji_clear, lw - cw, lh + S(d, 4), cw, bh);
            place(d->fuji_note, 0, lh + S(d, 4) + (bh - lh) / 2, lw - cw - gap, lh);
            place(d->fuji_text, 0, lh + bh + S(d, 10), lw, h - lh - bh - S(d, 10));
            place(d->fuji_state_title, lw + S(d, 12), 0, rw, lh);
            place(d->fuji_state, lw + S(d, 12), lh + S(d, 4), rw, h - lh - S(d, 4));
            break;
        }
        default: break;
        }
    }
}

/* The register strip's height: three rows. */
static int strip_height(const debugger *d)
{
    return 3 * (S(d, 24) + S(d, 5)) + S(d, 6);
}

static void layout(debugger *d)
{
    RECT client, page;
    int x, y, w, i, px, py, pw, ph, strip_y, tab_y;
    const int bh = S(d, 26), eh = S(d, 24), lh = S(d, 18), gap = S(d, 6);

    if (!d->tabs) return;
    GetClientRect(d->hwnd, &client);

    /* The toolbar: stepping, then symbols, save and pop out, the status after
     * them. Too narrow for the status beside them, it goes on a line of its own. */
    {
        HWND bar[9] = { d->run_btn, d->step_btn, d->over_btn, d->out_btn, d->run_to_btn, d->frame_btn,
                        d->sym_btn, d->save_btn, d->popout_btn };
        const char *btext[9] = { "Stop (F5)", "Step (F7)", "Step Over (F8)", "Step Out (Shift+F8)",
                                 "Run to Cursor", "Frame (Shift+F7)", "Symbols...", "Save...", "Pop Out" };
        const int hw = btn_w(d, "Help");
        const int sright = client.right - hw - gap;
        x = S(d, 8);
        for (i = 0; i < 9; i++) {
            w = btn_w(d, btext[i]);
            place(bar[i], x, S(d, 8), w, bh);
            x += w + gap + (i == 5 ? S(d, 14) : 0);
        }
        y = S(d, 8) + bh + S(d, 8);
        place(d->help_btn, client.right - S(d, 8) - hw, S(d, 8), hw, bh);
        if (sright - S(d, 8) - x >= S(d, 220)) {
            place(d->status, x, S(d, 8) + (bh - lh) / 2, sright - S(d, 8) - x, lh);
        } else {
            place(d->status, S(d, 8), y - S(d, 2), client.right - S(d, 16), lh);
            y += lh + S(d, 4);
        }
        tab_y = y;
    }

    /* The register strip, under every tab: AF BC DE HL IX IY SP PC / the
     * shadow set, I, R, IM and HALT / the flags and flip-flops and a hint. */
    strip_y = client.bottom - strip_height(d);
    {
        const int rw = d->mono_w * 6 + S(d, 8), cell = (client.right - S(d, 16) + S(d, 8)) / 8;
        const int lw = text_w(d, d->ui, "WWW") + S(d, 2);
        const int row_h = eh + S(d, 5);
        px = S(d, 12);
        for (i = 0; i < NREGS; i++) {
            const int row = i / 8, col = i % 8;
            const int row_top = strip_y + S(d, 3) + row * row_h;
            place(d->reg_label[i], px + col * cell, row_top + (eh - lh) / 2, lw, lh);
            place(d->reg[i], px + col * cell + lw + S(d, 4), row_top, rw, eh);
        }
        place(d->halt_label, px + 7 * cell + lw + S(d, 4), strip_y + S(d, 3) + row_h + (eh - lh) / 2, rw, lh);
        x = px;
        y = strip_y + S(d, 3) + 2 * row_h;
        for (i = 0; i < NCHECKS; i++) {
            w = text_w(d, d->ui, k_chk_names[i]) + S(d, 28);
            place(d->chk[i], x, y + S(d, 1), w, S(d, 22));
            x += w + S(d, 6);
        }
        place(d->cpu_hint, x + S(d, 16), y + (eh - lh) / 2, client.right - x - S(d, 16) - px, lh);
    }

    /* The tab control is the strip alone: the pages sit on the window's own
     * background, which their captions are drawn on. */
    SetRect(&page, 0, 0, S(d, 400), S(d, 400));
    SendMessageA(d->tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    place(d->tabs, S(d, 8), tab_y, client.right - S(d, 16), page.top);
    px = S(d, 12);
    py = tab_y + page.top + S(d, 6);
    pw = client.right - 2 * px;
    ph = strip_y - S(d, 6) - py;
    if (pw < S(d, 300)) pw = S(d, 300);
    if (ph < S(d, 160)) ph = S(d, 160);

    /* each tab's container: in the tab control's page, or filling the window it was peeled off into */
    for (i = 0; i < TAB_COUNT; i++) {
        if (d->popped[i]) {
            RECT pop_rect;
            GetClientRect(d->popped[i], &pop_rect);
            MoveWindow(d->pages[i], S(d, 6), S(d, 34), pop_rect.right > S(d, 12) ? pop_rect.right - S(d, 12) : 1,
                       pop_rect.bottom > S(d, 40) ? pop_rect.bottom - S(d, 40) : 1, TRUE);
        } else {
            MoveWindow(d->pages[i], px, py, pw, ph, TRUE);
        }
    }
    layout_pages(d);

    /* everything again: what the controls left behind where they were */
    RedrawWindow(d->hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* ---- the window ------------------------------------------------------------------- */

static void hide_window(debugger *d)
{
    KillTimer(d->hwnd, TIMER_REFRESH);
    msxdebug_trace_enable(d->dbg, 0);        /* the ring costs the CPU its slow path: only while a window wants it */
    msxdebug_history_enable(d->dbg, 0);
    msxdebug_detach(d->dbg);     /* lets the machine run */
    ShowWindow(d->hwnd, SW_HIDE);
}

static void attach_and_refresh(debugger *d)
{
    msxdebug_attach(d->dbg);     /* stops the machine */
    d->was_stopped = 0;
    d->seen_gen = msxdebug_generation(d->dbg);
    SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);
    settle_and_sync(d, 1);
}

static void draw_run_button(debugger *d, const DRAWITEMSTRUCT *di)
{
    char text[32];
    const int stopped = msxdebug_is_stopped(d->dbg);
    RECT r = di->rcItem;
    if (stopped) {
        FillRect(di->hDC, &r, d->accent);
        FrameRect(di->hDC, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(di->hDC, INK_RGB);
    } else {
        DrawFrameControl(di->hDC, &r, DFC_BUTTON, DFCS_BUTTONPUSH | ((di->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0));
        SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
    }
    SetBkMode(di->hDC, TRANSPARENT);
    GetWindowTextA(di->hwndItem, text, sizeof text);
    SelectObject(di->hDC, d->ui);
    DrawTextA(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (di->itemState & ODS_FOCUS) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(di->hDC, &r);
    }
}


static void select_tab(debugger *d, int page)
{
    goto_page(d, page);
}

static LRESULT on_notify(debugger *d, NMHDR *nh)
{
    if (nh->hwndFrom == d->tabs && nh->code == (UINT)TCN_SELCHANGE) {
        const int idx = (int)SendMessageA(d->tabs, TCM_GETCURSEL, 0, 0);
        if (idx >= 0 && idx < d->ntabs) {
            show_page(d, d->tab_page[idx]);
            refresh_page(d, d->tab_page[idx], 1);
        }
        return 0;
    }
    if (nh->code == (UINT)NM_CUSTOMDRAW && (nh->hwndFrom == d->h_rec[0] || nh->hwndFrom == d->h_rec[1]))
        return msx_accent_customdraw((const NMCUSTOMDRAW *)nh, SendMessageA(nh->hwndFrom, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (nh->hwndFrom != d->bp_list) return 0;
    if (nh->code == (UINT)LVN_ITEMCHANGED) {
        const NMLISTVIEW *nm = (const NMLISTVIEW *)nh;
        /* a row's box: its breakpoint on or off (the list follows after) */
        if (!d->updating && nm->iItem >= 0 && nm->iItem < d->nbps && (nm->uChanged & LVIF_STATE) &&
            ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK) && (nm->uOldState & LVIS_STATEIMAGEMASK)) {
            const int on = ((nm->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
            if (on != (d->bps[nm->iItem].enabled != 0)) {
                msxdebug_breakpoint_enable(d->dbg, d->bps[nm->iItem].id, on);
                PostMessageA(d->hwnd, WM_DBG_SYNC, 0, 0);
            }
        }
    } else if (nh->code == (UINT)LVN_KEYDOWN) {
        /* after the list's own handling: removing rebuilds it */
        if (((const NMLVKEYDOWN *)nh)->wVKey == VK_DELETE)
            PostMessageA(d->hwnd, WM_COMMAND, MAKEWPARAM(IDC_BP_REMOVE, BN_CLICKED), 0);
    } else if (nh->code == (UINT)NM_DBLCLK) {
        /* an address breakpoint's row opens the disassembly there */
        const int i = bp_selected(d);
        if (i >= 0 && !(d->bps[i].type & (MSXDEBUG_BP_IN | MSXDEBUG_BP_OUT))) {
            char a[16];
            snprintf(a, sizeof a, "$%04X", d->bps[i].start);
            select_tab(d, TAB_DISASM);
            SetWindowTextA(d->jump, a);
            on_jump(d);
        }
    }
    return 0;
}

static LRESULT CALLBACK dbg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (msg == WM_GETMINMAXINFO) {
        const int dpi = d && d->dpi ? d->dpi : 96;
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = MulDiv(980, dpi, 96);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = MulDiv(600, dpi, 96);
        return 0;
    }
    if (!d || d->hwnd != hwnd) return DefWindowProcA(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE:
        layout(d);
        return 0;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        d->dpi = HIWORD(wp);
        make_fonts(d);
        size_columns(d);
        if (d->video) dbg_video_fonts(d->video, d->ui, d->mono, d->dpi);
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout(d);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_REFRESH) tick(d);
        return 0;
    case WM_DBG_ACCEPT:
        on_accept(d, (int)wp);
        return 0;
    case WM_DBG_TAB:
        complete_console(d);
        return 0;
    case WM_DBG_HISTORY:
        history_step(d, (int)wp);
        return 0;
    case WM_DBG_SYNC:
        resync(d);
        return 0;
    case WM_DBG_GOTO:
        goto_page(d, TAB_MEMORY);
        show_in_memory(d, (int)wp, (int)wp);
        return 0;
    case WM_NOTIFY:
        return on_notify(d, (NMHDR *)lp);
    case WM_DRAWITEM:
        if (wp == IDC_RUN) {
            draw_run_button(d, (const DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HWND h = (HWND)lp;
        HDC dc = (HDC)wp;
        if (in_list(h, d->views, d->nviews)) {
            /* read-only views on the window colour, as text views are */
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        if (h == d->bp_msg) {
            SetTextColor(dc, RGB(0xc0, 0x1c, 0x28));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if (h == d->status && notice_showing(d)) {
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if (in_list(h, d->dim, d->ndim)) {
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        /* the disassembly scrolls under the pointer, focused or not */
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r;
        GetWindowRect(d->disasm, &r);
        if (page_visible(d, TAB_DISASM) && PtInRect(&r, p)) return SendMessageA(d->disasm, msg, wp, lp);
        break;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id >= IDC_CHK0 && id <= IDC_CHK_LAST && HIWORD(wp) == BN_CLICKED) {
            on_check(d, id - IDC_CHK0);
            return 0;
        }
        if (id >= IDC_SAVE_KIND0 && id <= IDC_SAVE_KIND_LAST) {
            on_save(d, id - IDC_SAVE_KIND0);
            return 0;
        }
        switch (id) {
        case IDC_RUN: on_run(d); return 0;
        case IDC_STEP: msxdebug_step(d->dbg); settle_and_sync(d, 1); return 0;
        case IDC_OVER: msxdebug_step_over(d->dbg); settle_and_sync(d, 1); return 0;
        case IDC_OUT: msxdebug_step_out(d->dbg); settle_and_sync(d, 1); return 0;
        case IDC_FRAME: msxdebug_frame(d->dbg); settle_and_sync(d, 1); return 0;
        case IDC_RUN_TO: on_run_to(d); return 0;
        case IDC_TOGGLE_BP: on_toggle_bp(d); return 0;
        case IDC_HELP_BTN: msx_help_for(d->hwnd); return 0;
        case IDC_SYMBOLS: on_symbols(d); return 0;
        case IDC_SAVE: save_menu(d); return 0;
        case IDC_POPOUT: pop_out(d, d->page); return 0;
        case IDC_MEM_BACK: on_mem_page(d, -1); return 0;
        case IDC_MEM_ON: on_mem_page(d, 1); return 0;
        case IDC_SRCH_NEXT: do_search(d, 0); return 0;
        case IDC_SRCH_PREV: do_search(d, 1); return 0;
        case IDC_FOLLOW_PC: refresh_disasm(d); return 0;
        case IDC_BP_TYPE:
            if (HIWORD(wp) == CBN_SELCHANGE) on_bp_type(d);
            return 0;
        case IDC_BP_ADD: on_bp_add(d); return 0;
        case IDC_BP_REMOVE: on_bp_remove(d); return 0;
        case IDC_BP_HITS:
            msxdebug_breakpoint_clear_hits(d->dbg);
            resync(d);
            return 0;
        case IDC_BP_CLEAR:
            msxdebug_breakpoint_clear(d->dbg);
            set_label(d->bp_msg, "");
            resync(d);
            return 0;
        case IDC_SYM_FILTER:
            if (HIWORD(wp) == EN_CHANGE) refresh_symbols(d);       /* the list follows the typing */
            return 0;
        case IDC_WT_ADD: add_watch(d); return 0;
        case IDC_WT_MEM: {
            uint16_t low, high;
            if (watch_range(d, &low, &high)) {
                goto_page(d, TAB_MEMORY);
                show_in_memory(d, low, high);
            } else {
                toast(d, "Select a watch that resolves to an address");
            }
            return 0;
        }
        case IDC_WT_BRK: {
            uint16_t low, high;
            if (watch_range(d, &low, &high)) watch_break(d, low, high);
            else toast(d, "Select a watch that resolves to an address");
            return 0;
        }
        case IDC_WT_REMOVE: {
            const int sel = (int)SendMessageA(d->wt_list, LB_GETCURSEL, 0, 0);
            if (sel >= 0) {
                msxwatch_remove(sel);
                refresh_watches(d);
            }
            return 0;
        }
        case IDC_H_REC0:
            msxdebug_trace_enable(d->dbg, SendMessageA((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED);
            hist_refresh(d, 0, 1);
            return 0;
        case IDC_H_REC1:
            msxdebug_history_enable(d->dbg, SendMessageA((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED);
            hist_refresh(d, 1, 1);
            return 0;
        case IDC_H_CLEAR0:
            msxdebug_trace_clear(d->dbg);
            hist_refresh(d, 0, 1);
            return 0;
        case IDC_H_CLEAR1:
            msxdebug_history_clear(d->dbg);
            hist_refresh(d, 1, 1);
            return 0;
        case IDC_H_FILTER0:
        case IDC_H_FILTER1:
            if (HIWORD(wp) == CBN_SELCHANGE) hist_refresh(d, id == IDC_H_FILTER1, 1);
            return 0;
        case IDC_FUJI_CLEAR:
            msxdebug_fujinet_trace_clear(d->dbg);
            refresh_fujinet(d, 1);
            return 0;
        case IDCANCEL: hide_window(d); return 0;
        default: break;
        }
        break;
    }
    case WM_CLOSE:
        /* Hide and let the machine run; F12 brings it back, stopped. */
        hide_window(d);
        return 0;
    case WM_DESTROY: {
        int p;
        KillTimer(hwnd, TIMER_REFRESH);
        for (p = 0; p < TAB_COUNT; p++)     /* a peeled-off tab's window goes with the debugger */
            if (d->popped[p]) {
                HWND pop = d->popped[p];
                d->popped[p] = NULL;
                DestroyWindow(pop);
            }
        if (msxdebug_is_attached(d->dbg)) msxdebug_detach(d->dbg);
        DeleteObject(d->ui);
        DeleteObject(d->ui_bold);
        DeleteObject(d->mono);
        DeleteObject(d->accent);
        DeleteObject(d->bp_on);
        DeleteObject(d->bp_off);
        DestroyAcceleratorTable(d->accel);
        free(d->sym_text);
        free(d);
        g_dbg = NULL;
        return 0;
    }
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- entry points ----------------------------------------------------------------- */

static void register_class(HINSTANCE inst, const char *name, WNDPROC proc, HBRUSH background, UINT style)
{
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = style;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = background;
    wc.lpszClassName = name;
    if (proc == dbg_proc) wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
    RegisterClassExA(&wc);
}

/* A tab named by an environment variable: a number, or a tab's name or label
 * (any case), or the start of one, or of a word of a label ("memory",
 * "i/o"...), or one of the old names. -1 if unset or unknown. */
static int tab_from_env_var(const char *var)
{
    const char *tab = getenv(var);
    char *end;
    long n;
    size_t len;
    int i, pass;
    if (!tab || !*tab) return -1;
    for (i = 0; i < (int)(sizeof k_tab_aliases / sizeof k_tab_aliases[0]); i++)
        if (_stricmp(tab, k_tab_aliases[i].name) == 0) return k_tab_aliases[i].tab;
    n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (n >= 0 && n < TAB_COUNT) ? (int)n : -1;
    len = strlen(tab);
    for (pass = 0; pass < 3; pass++)
        for (i = 0; i < TAB_COUNT; i++) {
            if (pass == 0 && _strnicmp(tab, k_tabs[i].name, len) == 0) return i;
            if (pass == 1 && _strnicmp(tab, k_tabs[i].label, len) == 0) return i;
            if (pass == 2) {
                const char *p = k_tabs[i].label;
                while ((p = strchr(p, ' ')) != NULL)
                    if (_strnicmp(tab, ++p, len) == 0) return i;
            }
        }
    return -1;
}

/* For captures and tests: MSX_HISTORY records from the start, MSX_WATCH="x,y"
 * adds watches, MSX_WATCH_BREAK="x" a write-when-changed breakpoint on it,
 * MSX_MEM_GOTO="x" shows it in Memory, MSX_POPOUT=<tab> peels a tab off. */
static void apply_env_hooks(debugger *d)
{
    const char *hist = getenv("MSX_HISTORY"), *watch_text = getenv("MSX_WATCH"), *break_text = getenv("MSX_WATCH_BREAK"),
               *goto_text = getenv("MSX_MEM_GOTO");
    uint16_t low, high;
    int pop;

    if (hist && *hist && *hist != '0') msxdebug_history_enable(d->dbg, 1);
    if (watch_text && *watch_text) {
        char list[512], *item, *save = NULL;
        snprintf(list, sizeof list, "%s", watch_text);
        for (item = strtok_s(list, ",", &save); item; item = strtok_s(NULL, ",", &save)) {
            item = strip(item);
            if (*item) msxwatch_add(item);
        }
    }
    if (break_text && *break_text && msxsym_parse_range(d->dbg, break_text, &low, &high)) {
        char err[160];
        msxdebug_breakpoint_add(d->dbg, MSXDEBUG_BP_WRITE, low, high, "value != old", err, sizeof err);
    }
    if (goto_text && *goto_text && msxsym_parse_range(d->dbg, goto_text, &low, &high)) show_in_memory(d, low, high);
    pop = tab_from_env_var("MSX_POPOUT");
    if (pop >= 0) pop_out(d, pop);
    resync(d);
}

void msx_debugger_show(HWND parent, msxsession *session)
{
    static int registered;
    INITCOMMONCONTROLSEX icc;
    HINSTANCE inst;
    debugger *d;
    ACCEL accels[7];
    int tab;

    if (g_dbg) {
        d = g_dbg;
        ShowWindow(d->hwnd, IsIconic(d->hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(d->hwnd);
        if (!msxdebug_is_attached(d->dbg) || !IsWindowVisible(d->hwnd) || !msxdebug_is_stopped(d->dbg))
            attach_and_refresh(d);
        else
            resync(d);
        return;
    }

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);
    inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);
    if (!inst) inst = GetModuleHandleA(NULL);
    if (!registered) {
        register_class(inst, DBG_CLASS, dbg_proc, (HBRUSH)(COLOR_BTNFACE + 1), CS_HREDRAW | CS_VREDRAW);
        register_class(inst, DISASM_CLASS, disasm_proc, NULL, CS_DBLCLKS);
        register_class(inst, PAGE_CLASS, page_proc, (HBRUSH)(COLOR_BTNFACE + 1), 0);
        register_class(inst, POP_CLASS, pop_proc, (HBRUSH)(COLOR_BTNFACE + 1), 0);
        registered = 1;
    }

    d = calloc(1, sizeof *d);
    if (!d) return;
    d->session = session;
    d->dbg = msxsession_debugger(session);
    if (!d->dbg) {
        free(d);
        MessageBoxA(parent, "The debugger could not be created for this session.", "Debugger Unavailable",
                    MB_ICONERROR);
        return;
    }
    d->selected = -1;
    d->pc_line = -1;
    d->fuji_count = -1;
    d->h_seen[0] = d->h_seen[1] = (uint64_t)-1;
    g_dbg = d;

    d->dpi = window_dpi(parent);
    /* owned by the main window: over it, and gone with it */
    d->hwnd = CreateWindowExA(0, DBG_CLASS, "Debugger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                              CW_USEDEFAULT, S(d, 1250), S(d, 860), parent, NULL, inst, NULL);
    if (!d->hwnd) {
        free(d);
        g_dbg = NULL;
        return;
    }
    if (window_dpi(d->hwnd) != d->dpi) {
        /* placed on a monitor of another DPI than the parent's */
        d->dpi = window_dpi(d->hwnd);
        SetWindowPos(d->hwnd, NULL, 0, 0, S(d, 1250), S(d, 860), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    make_fonts(d);
    d->accent = CreateSolidBrush(ACCENT_RGB);
    d->bp_on = CreateSolidBrush(RGB(0xe0, 0x1b, 0x24));
    d->bp_off = CreateSolidBrush(RGB(0x9a, 0x99, 0x96));

    build_controls(d);

    /* F5/F7/F8/Shift+F8/Shift+F7/F9/F12 work wherever the focus is in the
     * window. */
    accels[0].fVirt = FVIRTKEY;          accels[0].key = VK_F5;  accels[0].cmd = IDC_RUN;
    accels[1].fVirt = FVIRTKEY;          accels[1].key = VK_F7;  accels[1].cmd = IDC_STEP;
    accels[2].fVirt = FVIRTKEY;          accels[2].key = VK_F8;  accels[2].cmd = IDC_OVER;
    accels[3].fVirt = FVIRTKEY | FSHIFT; accels[3].key = VK_F8;  accels[3].cmd = IDC_OUT;
    accels[4].fVirt = FVIRTKEY | FSHIFT; accels[4].key = VK_F7;  accels[4].cmd = IDC_FRAME;
    accels[5].fVirt = FVIRTKEY;          accels[5].key = VK_F9;  accels[5].cmd = IDC_TOGGLE_BP;
    accels[6].fVirt = FVIRTKEY;          accels[6].key = VK_F12; accels[6].cmd = IDCANCEL;
    d->accel = CreateAcceleratorTableA(accels, 7);

    tab = tab_from_env_var("MSX_DEBUGGER_TAB");
    d->page = tab >= 0 ? tab : TAB_CONSOLE;
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)d->page, 0);
    show_page(d, d->page);
    layout(d);

    ShowWindow(d->hwnd, SW_SHOW);
    if (d->page == TAB_CONSOLE) SetFocus(d->con_in);
    attach_and_refresh(d);
    apply_env_hooks(d);
}

int msx_debugger_visible(void)
{
    return g_dbg && IsWindowVisible(g_dbg->hwnd);
}

void msx_debugger_toggle(HWND parent, msxsession *session)
{
    if (msx_debugger_visible()) hide_window(g_dbg);
    else msx_debugger_show(parent, session);
}

int msx_debugger_pretranslate(MSG *msg)
{
    int p, mine;
    if (!g_dbg || !g_dbg->accel) return 0;
    mine = msg->hwnd == g_dbg->hwnd || IsChild(g_dbg->hwnd, msg->hwnd);
    for (p = 0; !mine && p < TAB_COUNT; p++)     /* a peeled-off tab's window too */
        if (g_dbg->popped[p] && (msg->hwnd == g_dbg->popped[p] || IsChild(g_dbg->popped[p], msg->hwnd))) mine = 1;
    if (!mine) return 0;
    /* F12 closes without waiting for the accelerator (Windows keeps F12 for
     * debuggers); its key-up goes with it */
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_F12) {
        hide_window(g_dbg);
        return 1;
    }
    return TranslateAcceleratorA(g_dbg->hwnd, g_dbg->accel, msg) ? 1 : 0;
}
