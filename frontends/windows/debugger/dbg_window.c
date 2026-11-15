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
#define SLOW_DEBOUNCE_MS 400 /* the engine's memory map and I/O reads cost ~0.8 s each: never more often than this */

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

    /* cart & I/O, sound; the slow reads behind them are cached (see cache_due) */
    HWND cart_text, io_text, sound_text;
    int cart_ticks;
    msxdebug_cart cart_info;
    msxdebug_io cart_ports;
    char cart_state[8192];          /* msxdebug_io_state text */
    int cart_valid, cart_dirty;
    uint64_t cart_serial;
    DWORD cart_stamp;
    msxdebug_region mem_map[16];
    int mem_map_count, mem_map_valid, mem_map_dirty;
    uint64_t mem_map_serial;
    DWORD mem_map_stamp;

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

    HFONT ui_font, ui_bold, mono;
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

static void edit_text(HWND hwnd, char *out, int outsz) { GetWindowTextA(hwnd, out, outsz); }

static void wide(const char *utf8, wchar_t *out, int outn)
{
    if (outn <= 0) return;
    out[0] = 0;
    if (utf8 && MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, outn) <= 0) out[0] = 0;
    out[outn - 1] = 0;
}

/* A label set only when it changed (no flicker on the live refresh). */
static void set_label(HWND hwnd, const char *utf8)
{
    wchar_t cur[512], wtext[512];
    wide(utf8, wtext, 512);
    GetWindowTextW(hwnd, cur, 512);
    if (wcscmp(cur, wtext) != 0) SetWindowTextW(hwnd, wtext);
}

/* EDIT controls want CR LF and draw control characters as boxes: engine
 * text (UTF-8) as the wide text a view shows. */
static wchar_t *wide_view_text(const char *text)
{
    size_t num = strlen(text), idx, outbuf = 0;
    char *buf = malloc(num * 2 + 1);
    wchar_t *wtext = NULL;
    int wn;
    if (!buf) return NULL;
    for (idx = 0; idx < num; idx++) {
        unsigned char cur = (unsigned char)text[idx];
        if (cur == '\n') { buf[outbuf++] = '\r'; buf[outbuf++] = '\n'; }
        else if (cur >= 0x20 || cur == '\t') buf[outbuf++] = (char)cur;
    }
    buf[outbuf] = '\0';
    wn = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    if (wn > 0 && (wtext = malloc((size_t)wn * sizeof *wtext)) != NULL)
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, wtext, wn);
    free(buf);
    return wtext;
}

static size_t addf(char *s, size_t len, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int num;
    if (len >= cap) return len;
    va_start(ap, fmt);
    num = vsnprintf(s + len, cap - len, fmt, ap);
    va_end(ap);
    if (num < 0) return len;
    return len + (size_t)num < cap ? len + (size_t)num : cap - 1;
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
    const char *ptr;
    char *end;
    long val;
    int base = 16;

    snprintf(buf, sizeof buf, "%s", text);
    ptr = buf;
    while (*ptr == ' ' || *ptr == '\t') ptr++;
    if (*ptr == '$') ptr++;
    else if (ptr[0] == '0' && (ptr[1] == 'x' || ptr[1] == 'X')) ptr += 2;
    else if (*ptr == '#') { ptr++; base = 10; }
    if (!*ptr) return 0;
    val = strtol(ptr, &end, base);
    if (base == 16 && (*end == 'h' || *end == 'H')) end++;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (end == ptr || *end) return 0;
    *out = val;
    return 1;
}

/* A label, or a number; -1 when it is neither. */
static int resolve_addr(debugger *dbgwin, const char *text)
{
    long val;
    int addr = -1;
    const char *ptr = text;
    while (*ptr == ' ') ptr++;
    if (!*ptr) return -1;
    if (*ptr != '$' && *ptr != '#' && !(ptr[0] == '0' && (ptr[1] == 'x' || ptr[1] == 'X')))
        addr = msxdebug_label_address(dbgwin->dbg, ptr);
    if (addr < 0 && parse_num(ptr, &val) && val >= 0 && val <= 0xffff) addr = (int)val;
    if (addr < 0) {
        char err[96];
        if (msxdebug_eval(dbgwin->dbg, ptr, &val, err, (int)sizeof err) == 0 && val >= 0 && val <= 0xffff) addr = (int)val;
    }
    return addr;
}

static const char *base_name(const char *path)
{
    const char *start_text = strrchr(path, '\\'), *end_text = strrchr(path, '/');
    if (end_text && (!start_text || end_text > start_text)) start_text = end_text;
    return start_text ? start_text + 1 : path;
}


/* ---- scale and fonts --------------------------------------------------------- */

static int system_dpi(void)
{
    HDC dc = GetDC(NULL);
    int val = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return val > 0 ? val : 96;
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
        const UINT val = fn(hwnd);
        if (val) return (int)val;
    }
    return system_dpi();
}

static int scaled(const debugger *dbgwin, int val) { return MulDiv(val, dbgwin->dpi, 96); }

/* The message font at dpi (system metrics come at the system DPI), at its
 * own size (px 0) or px pixels at 96 dpi; face NULL keeps the message
 * font's, otherwise a fixed-pitch face (Consolas). */
static HFONT make_font(int dpi, int page_x, int weight, const char *face)
{
    NONCLIENTMETRICSA ncm;
    LOGFONTA lf;
    ZeroMemory(&ncm, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0))
        lf = ncm.lfMessageFont;
    else
        GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof lf, &lf);
    if (page_x > 0) lf.lfHeight = -MulDiv(page_x, dpi, 96);
    else lf.lfHeight = -MulDiv(abs(lf.lfHeight ? lf.lfHeight : 12), dpi, system_dpi());
    lf.lfWidth = 0;
    lf.lfWeight = weight;
    if (face) {
        snprintf(lf.lfFaceName, sizeof lf.lfFaceName, "%s", face);
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    }
    return CreateFontIndirectA(&lf);
}

static BOOL CALLBACK refont_child(HWND hwnd, LPARAM lparam)
{
    const HFONT *map = (const HFONT *)lparam;     /* old ui, bold, mono; new ui, bold, mono */
    const HFONT f = (HFONT)SendMessageA(hwnd, WM_GETFONT, 0, 0);
    int idx;
    for (idx = 0; idx < 3; idx++)
        if (f && f == map[idx]) SendMessageA(hwnd, WM_SETFONT, (WPARAM)map[idx + 3], TRUE);
    return TRUE;
}

/* The message font and Consolas at the window's DPI; the controls already
 * made take the new ones. */
static void make_fonts(debugger *dbgwin)
{
    HFONT old[3] = { dbgwin->ui_font, dbgwin->ui_bold, dbgwin->mono };
    HDC dc;
    TEXTMETRICA tm;
    HGDIOBJ prev;
    int idx;

    dbgwin->ui_font = make_font(dbgwin->dpi, 0, FW_NORMAL, NULL);
    dbgwin->ui_bold = make_font(dbgwin->dpi, 0, FW_BOLD, NULL);
    dbgwin->mono = make_font(dbgwin->dpi, MONO_PX, FW_NORMAL, "Consolas");

    /* the monospace cell, for the hand-drawn views */
    dc = GetDC(dbgwin->hwnd);
    prev = SelectObject(dc, dbgwin->mono);
    dbgwin->mono_w = scaled(dbgwin, 7);
    dbgwin->mono_h = scaled(dbgwin, 15);
    if (GetTextMetricsA(dc, &tm)) {
        dbgwin->mono_w = tm.tmAveCharWidth;
        dbgwin->mono_h = tm.tmHeight;
    }
    dbgwin->line_h = dbgwin->mono_h + scaled(dbgwin, 2);
    SelectObject(dc, prev);
    ReleaseDC(dbgwin->hwnd, dc);

    if (old[0]) {
        HFONT map[6] = { old[0], old[1], old[2], dbgwin->ui_font, dbgwin->ui_bold, dbgwin->mono };
        EnumChildWindows(dbgwin->hwnd, refont_child, (LPARAM)map);
    }
    for (idx = 0; idx < 3; idx++)
        if (old[idx]) DeleteObject(old[idx]);
}

static int text_w(debugger *dbgwin, HFONT f, const char *text)
{
    HDC dc = GetDC(dbgwin->hwnd);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32A(dc, text, (int)strlen(text), &sz);
    SelectObject(dc, old);
    ReleaseDC(dbgwin->hwnd, dc);
    return sz.cx;
}

static int btn_w(debugger *dbgwin, const char *text)
{
    const int wtext = text_w(dbgwin, dbgwin->ui_font, text) + scaled(dbgwin, 24);
    return wtext < scaled(dbgwin, 64) ? scaled(dbgwin, 64) : wtext;
}

static int in_list(HWND hwnd, const HWND *list, int num)
{
    int idx;
    for (idx = 0; idx < num; idx++)
        if (list[idx] == hwnd) return 1;
    return 0;
}

/* ---- the text views ------------------------------------------------------------ */

/* The rows an EDIT view shows. */
static int edit_rows(debugger *dbgwin, HWND hwnd)
{
    RECT cur;
    GetClientRect(hwnd, &cur);
    return dbgwin->mono_h > 0 && cur.bottom > dbgwin->mono_h ? cur.bottom / dbgwin->mono_h : 1;
}

/* Whether a log's reader is at its end (it then follows new lines). */
static int edit_at_end(debugger *dbgwin, HWND hwnd)
{
    const int first = (int)SendMessageW(hwnd, EM_GETFIRSTVISIBLELINE, 0, 0);
    const int count = (int)SendMessageW(hwnd, EM_GETLINECOUNT, 0, 0);
    return first + edit_rows(dbgwin, hwnd) >= count;
}

static void edit_to_end(HWND hwnd)
{
    SendMessageW(hwnd, EM_LINESCROLL, 0, SendMessageW(hwnd, EM_GETLINECOUNT, 0, 0));
}

static void edit_scroll_to(HWND hwnd, int first)
{
    SendMessageW(hwnd, EM_LINESCROLL, 0, first - (int)SendMessageW(hwnd, EM_GETFIRSTVISIBLELINE, 0, 0));
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void set_text_at(HWND hwnd, const char *text, int follow)
{
    const int first = (int)SendMessageW(hwnd, EM_GETFIRSTVISIBLELINE, 0, 0);
    wchar_t *wtext = wide_view_text(text);
    if (!wtext) return;
    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(hwnd, wtext);
    if (follow) edit_to_end(hwnd);
    else edit_scroll_to(hwnd, first);
    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hwnd, NULL, TRUE);
    free(wtext);
}

static void refresh_status(debugger *dbgwin);

static int notice_showing(debugger *dbgwin)
{
    return dbgwin->notice[0] && (LONG)(GetTickCount() - dbgwin->notice_until) < 0;
}

/* The other frontends' toasts: the message holds the status line a moment. */
static void toast(debugger *dbgwin, const char *msg)
{
    snprintf(dbgwin->notice, sizeof dbgwin->notice, "%s", msg);
    dbgwin->notice_until = GetTickCount() + NOTICE_MS;
    refresh_status(dbgwin);
}

/* ---- the console ---------------------------------------------------------------- */

/* Long sessions keep the last CONSOLE_KEEP characters, whole lines. */
static void console_trim(debugger *dbgwin)
{
    const int len = GetWindowTextLengthW(dbgwin->con_out);
    wchar_t *all, *nl;
    int cut;
    if (len <= CONSOLE_KEEP) return;
    all = malloc(((size_t)len + 1) * sizeof *all);
    if (!all) return;
    GetWindowTextW(dbgwin->con_out, all, len + 1);
    cut = len - CONSOLE_KEEP * 3 / 4;
    nl = wcschr(all + cut, L'\n');
    if (nl) cut = (int)(nl - all) + 1;
    free(all);
    SendMessageW(dbgwin->con_out, EM_SETSEL, 0, (LPARAM)cut);
    SendMessageW(dbgwin->con_out, EM_REPLACESEL, FALSE, (LPARAM)L"");
}

/* Onto the console's transcript, which follows its end only while the
 * reader is there. */
static void console_append(debugger *dbgwin, const char *text)
{
    const int follow = edit_at_end(dbgwin, dbgwin->con_out);
    const int first = (int)SendMessageW(dbgwin->con_out, EM_GETFIRSTVISIBLELINE, 0, 0);
    wchar_t *wtext = wide_view_text(text);
    int len;
    if (!wtext) return;
    len = GetWindowTextLengthW(dbgwin->con_out);
    SendMessageW(dbgwin->con_out, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(dbgwin->con_out, EM_REPLACESEL, FALSE, (LPARAM)wtext);
    free(wtext);
    console_trim(dbgwin);
    if (follow) edit_to_end(dbgwin->con_out);
    else edit_scroll_to(dbgwin->con_out, first);
}

/* Command output, ended with a newline. */
static void console_output(debugger *dbgwin, const char *text)
{
    const size_t num = strlen(text);
    if (!num) return;
    console_append(dbgwin, text);
    if (text[num - 1] != '\n') console_append(dbgwin, "\n");
}

/* Messages from Load Symbols and Save land in the console and, since it may
 * not be the tab showing, on the status line. */
static void say(debugger *dbgwin, const char *msg)
{
    console_output(dbgwin, msg);
    toast(dbgwin, msg);
}


/* ---- refreshers -------------------------------------------------------------- */

static int page_visible(debugger *dbgwin, int page);

static void refresh_status(debugger *dbgwin)
{
    msxdebug_snapshot snap;
    char text[260];
    const int stopped = msxdebug_stop_snapshot(dbgwin->dbg, &snap) != 0;

    if (notice_showing(dbgwin)) {
        snprintf(text, sizeof text, "%s", dbgwin->notice);
    } else if (stopped) {
        snprintf(text, sizeof text, "Stopped: %s", snap.text);
    } else {
        snprintf(text, sizeof text, "Running");
    }
    set_label(dbgwin->status, text);
    set_label(dbgwin->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    InvalidateRect(dbgwin->run_btn, NULL, FALSE);
    InvalidateRect(dbgwin->status, NULL, TRUE);
}

/* The register strip: the machine's values (the stale ones while it runs, a
 * box being typed in is left alone). */
static void refresh_cpu(debugger *dbgwin)
{
    msxdebug_cpu cur;
    char buf[32];
    int idx;
    msxdebug_cpu_get(dbgwin->dbg, &cur);
    {
        const int val[NREGS] = { cur.af, cur.bc, cur.de, cur.hl, cur.ix, cur.iy, cur.sp, cur.pc,
                               cur.af2, cur.bc2, cur.de2, cur.hl2, cur.i, cur.r, cur.im };
        for (idx = 0; idx < NREGS; idx++) {
            if (GetFocus() == dbgwin->reg[idx]) continue;   /* do not fight the user's typing */
            /* the register pairs as four digits, I and R as two, IM as it is */
            snprintf(buf, sizeof buf, idx < 12 ? "%04X" : idx < 14 ? "%02X" : "%d", val[idx] & 0xFFFF);
            set_label(dbgwin->reg[idx], buf);
        }
    }
    {
        const int on[NCHECKS] = { cur.sf, cur.zf, cur.hf, cur.pf, cur.nf, cur.cf, cur.iff1, cur.iff2 };
        dbgwin->updating = 1;
        for (idx = 0; idx < NCHECKS; idx++)
            SendMessageA(dbgwin->chk[idx], BM_SETCHECK, on[idx] ? BST_CHECKED : BST_UNCHECKED, 0);
        dbgwin->updating = 0;
    }
    set_label(dbgwin->halt_label, cur.halted ? "HALT" : "");
}

/* The engine's memory map, I/O and cart reads are slow against openMSX, so
 * the window keeps what it read and asks again only for a new stop (the
 * machine cannot change while it is stopped, and a running one is shown
 * stale): the first fill always, then never while running, and never more
 * often than SLOW_DEBOUNCE_MS (a stop inside that gap marks the cache dirty
 * and the timer comes back for it). Returns 1 when the caller should read now. */
static int cache_due(debugger *dbgwin, int valid, uint64_t *serial, DWORD *stamp, int *dirty)
{
    const uint64_t now_serial = msxdebug_stop_serial(dbgwin->dbg);
    const DWORD now = GetTickCount();
    if (!valid) {
        *serial = now_serial;
        *stamp = now;
        *dirty = 0;
        return 1;
    }
    if (!msxdebug_is_stopped(dbgwin->dbg) || (now_serial == *serial && !*dirty)) return 0;
    if (now - *stamp < SLOW_DEBOUNCE_MS) {
        *dirty = 1;
        return 0;
    }
    *serial = now_serial;
    *stamp = now;
    *dirty = 0;
    return 1;
}

static int cached_memory_map(debugger *dbgwin, msxdebug_region *out, int max)
{
    int num;
    if (cache_due(dbgwin, dbgwin->mem_map_valid, &dbgwin->mem_map_serial, &dbgwin->mem_map_stamp, &dbgwin->mem_map_dirty)) {
        num = msxdebug_memory_map(dbgwin->dbg, dbgwin->mem_map, 16);
        dbgwin->mem_map_count = num > 0 ? num : 0;
        dbgwin->mem_map_valid = 1;
    }
    num = dbgwin->mem_map_count < max ? dbgwin->mem_map_count : max;
    memcpy(out, dbgwin->mem_map, (size_t)num * sizeof out[0]);
    return num;
}

/* Where a byte's two hex digits start in a Memory row: "AAAA  " then three
 * characters a byte, one more after the eighth. */
static int mem_hex_col(int col)
{
    return 6 + col * 3 + (col >= 8 ? 1 : 0);
}

static void refresh_mem(debugger *dbgwin)
{
    static uint8_t mem[MEM_BYTES];
    static char buf[(MEM_ROWS + 1) * 140];
    static int rowoff[MEM_ROWS + 1];    /* each row's first character in the control (CR LF counted) */
    msxdebug_region map[16];
    char region[80], last_region[80];
    size_t len = 0;
    int row, col, nmap;

    memset(mem, 0, sizeof mem);
    msxdebug_read(dbgwin->dbg, (uint16_t)dbgwin->mem_top, mem, MEM_BYTES);
    nmap = cached_memory_map(dbgwin, map, 16);
    if (nmap < 0) nmap = 0;
    last_region[0] = '\0';
    len = addf(buf, len, sizeof buf, "      ");
    for (col = 0; col < 16; col++)
        len = addf(buf, len, sizeof buf, col == 8 ? " %X  " : "%X  ", col);
    len = addf(buf, len, sizeof buf, "\n");
    for (row = 0; row < MEM_ROWS; row++) {
        const uint8_t *bytes = mem + row * 16;
        const int addr = dbgwin->mem_top + row * 16;
        rowoff[row] = (int)len + row + 1;
        len = addf(buf, len, sizeof buf, "%04X  ", addr);
        for (col = 0; col < 16; col++)
            len = addf(buf, len, sizeof buf, col == 8 ? " %02X " : "%02X ", bytes[col]);
        len = addf(buf, len, sizeof buf, " ");
        for (col = 0; col < 16; col++)
            len = addf(buf, len, sizeof buf, "%c", bytes[col] >= 0x20 && bytes[col] < 0x7F ? bytes[col] : '.');
        /* the slot's device, on the first row of each run */
        region[0] = '\0';
        msxmem_region_name(map, nmap, (uint16_t)addr, region, (int)sizeof region);
        if (region[0] && strcmp(region, last_region) != 0) len = addf(buf, len, sizeof buf, "  ; %s", region);
        snprintf(last_region, sizeof last_region, "%s", region);
        len = addf(buf, len, sizeof buf, "\n");
    }
    set_text_at(dbgwin->mem_view, buf, 0);

    /* the bytes Go to, a search hit or a watch found stay selected across refreshes */
    if (dbgwin->mem_hl && dbgwin->mem_hl_hi >= dbgwin->mem_top && dbgwin->mem_hl_lo < dbgwin->mem_top + MEM_BYTES) {
        const int first = dbgwin->mem_hl_lo > dbgwin->mem_top ? dbgwin->mem_hl_lo : dbgwin->mem_top;
        const int last = dbgwin->mem_hl_hi < dbgwin->mem_top + MEM_BYTES - 1 ? dbgwin->mem_hl_hi : dbgwin->mem_top + MEM_BYTES - 1;
        const int first_row = (first - dbgwin->mem_top) / 16, last_row = (last - dbgwin->mem_top) / 16;
        const int top_line = (int)SendMessageW(dbgwin->mem_view, EM_GETFIRSTVISIBLELINE, 0, 0);
        SendMessageW(dbgwin->mem_view, EM_SETSEL, (WPARAM)(rowoff[first_row] + mem_hex_col((first - dbgwin->mem_top) % 16)),
                     (LPARAM)(rowoff[last_row] + mem_hex_col((last - dbgwin->mem_top) % 16) + 2));
        edit_scroll_to(dbgwin->mem_view, top_line);
    }
}

/* Shows bytes lo..hi in the Memory tab, selected, the page starting just above them. */
static void show_in_memory(debugger *dbgwin, int low, int high)
{
    char shown[16];
    int top;
    if (low < 0) low = 0;
    if (low > 0xFFFF) low = 0xFFFF;
    if (high < low) high = low;
    if (high > 0xFFFF) high = 0xFFFF;
    dbgwin->mem_hl = 1;
    dbgwin->mem_hl_lo = low;
    dbgwin->mem_hl_hi = high;
    dbgwin->mem_top = (low > 0x100 ? low - 0x100 : 0) & ~0xF;
    if (dbgwin->mem_top > 0x10000 - MEM_BYTES) dbgwin->mem_top = 0x10000 - MEM_BYTES;
    snprintf(shown, sizeof shown, "%04X", low);
    SetWindowTextA(dbgwin->mem_goto, shown);
    refresh_mem(dbgwin);
    /* the row 30% of the way down */
    top = 1 + (low - dbgwin->mem_top) / 16 - edit_rows(dbgwin, dbgwin->mem_view) * 3 / 10;
    edit_scroll_to(dbgwin->mem_view, top > 0 ? top : 0);
}

/* The disassembly rows the view has room for. */
static int disasm_rows(debugger *dbgwin)
{
    RECT cur;
    int rows;
    if (!dbgwin->disasm || dbgwin->line_h <= 0) return 1;
    GetClientRect(dbgwin->disasm, &cur);
    rows = (cur.bottom - cur.top) / dbgwin->line_h;
    if (rows < 1) rows = 1;
    if (rows > DISASM_MAX) rows = DISASM_MAX;
    return rows;
}

static int row_address(debugger *dbgwin, int addr, int rows)
{
    const int start_text = msxdebug_row_address(dbgwin->dbg, (uint16_t)addr, rows);
    return start_text < 0 ? addr : start_text & 0xFFFF;
}

static void refresh_disasm(debugger *dbgwin)
{
    int pc_line = -1;
    const int rows = disasm_rows(dbgwin);
    const int follow = SendMessageA(dbgwin->follow_pc, BM_GETCHECK, 0, 0) == BST_CHECKED;

    if (follow) {
        /* the PC a third of the way down, so what led to it shows too */
        msxdebug_cpu cur;
        msxdebug_cpu_get(dbgwin->dbg, &cur);
        dbgwin->disasm_top = row_address(dbgwin, cur.pc & 0xFFFF, -(rows / 3));
    }
    dbgwin->line_count = msxdebug_disassemble(dbgwin->dbg, (uint16_t)dbgwin->disasm_top, dbgwin->lines, rows, &pc_line);
    if (dbgwin->line_count < 0) dbgwin->line_count = 0;
    dbgwin->pc_line = pc_line;
    InvalidateRect(dbgwin->disasm, NULL, FALSE);
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
    wchar_t wtext[256];
    LVITEMW it;
    wide(text, wtext, 256);
    memset(&it, 0, sizeof it);
    it.iSubItem = col;
    it.pszText = wtext;
    SendMessageW(lv, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
}

/* The list, or (empty) the hint in its place. */
static void show_bp_list(debugger *dbgwin)
{
    const int on = page_visible(dbgwin, TAB_BREAKS);
    ShowWindow(dbgwin->bp_list, on && dbgwin->nbps > 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(dbgwin->bp_empty, on && dbgwin->nbps == 0 ? SW_SHOW : SW_HIDE);
}

/* Rebuilt only when something in it changed (each step refreshes), keeping
 * the selected breakpoint selected. */
static void refresh_bps(debugger *dbgwin)
{
    static char sig[sizeof dbgwin->bp_sig];
    size_t len = 0;
    int idx, sel_id = -1;
    const int sel = (int)SendMessageA(dbgwin->bp_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);

    if (sel >= 0 && sel < dbgwin->nbps) sel_id = dbgwin->bps[sel].id;
    dbgwin->nbps = msxdebug_breakpoint_list(dbgwin->dbg, dbgwin->bps, MAX_BPS);
    if (dbgwin->nbps < 0) dbgwin->nbps = 0;
    sig[0] = '\0';
    for (idx = 0; idx < dbgwin->nbps; idx++)
        len = addf(sig, len, sizeof sig, "%d %d %u %d %u %u %s\n", dbgwin->bps[idx].id, dbgwin->bps[idx].enabled,
                   dbgwin->bps[idx].hits, dbgwin->bps[idx].type, dbgwin->bps[idx].start, dbgwin->bps[idx].end, dbgwin->bps[idx].condition);
    if (dbgwin->bp_sig_ok && strcmp(sig, dbgwin->bp_sig) == 0) { show_bp_list(dbgwin); return; }
    memcpy(dbgwin->bp_sig, sig, sizeof sig);
    dbgwin->bp_sig_ok = 1;

    dbgwin->updating = 1;
    SendMessageA(dbgwin->bp_list, LVM_DELETEALLITEMS, 0, 0);
    for (idx = 0; idx < dbgwin->nbps; idx++) {
        const msxdebug_breakpoint *end_text = &dbgwin->bps[idx];
        const int port = (end_text->type & (MSXDEBUG_BP_IN | MSXDEBUG_BP_OUT)) != 0;
        LVITEMA it;
        char ctl_id[16], kind[40], range[32], hits[24];
        snprintf(ctl_id, sizeof ctl_id, "#%d", end_text->id);
        if (end_text->start == end_text->end) snprintf(range, sizeof range, port ? "$%02X" : "$%04X", end_text->start);
        else snprintf(range, sizeof range, port ? "$%02X-$%02X" : "$%04X-$%04X", end_text->start, end_text->end);
        snprintf(hits, sizeof hits, "%u", end_text->hits);
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = idx;
        it.pszText = ctl_id;
        SendMessageA(dbgwin->bp_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
        lv_set(dbgwin->bp_list, idx, 1, bp_kind(end_text->type, kind));
        lv_set(dbgwin->bp_list, idx, 2, range);
        lv_set(dbgwin->bp_list, idx, 3, end_text->condition);
        lv_set(dbgwin->bp_list, idx, 4, hits);
        memset(&it, 0, sizeof it);
        it.stateMask = LVIS_STATEIMAGEMASK | LVIS_SELECTED | LVIS_FOCUSED;
        it.state = INDEXTOSTATEIMAGEMASK(end_text->enabled ? 2 : 1);
        if (end_text->id == sel_id) it.state |= LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageA(dbgwin->bp_list, LVM_SETITEMSTATE, (WPARAM)idx, (LPARAM)&it);
    }
    dbgwin->updating = 0;
    show_bp_list(dbgwin);
}

/* A view's text, replaced only when it changed (an unchanged one keeps its
 * selection and where the reader scrolled it to). */
static void set_view(HWND hwnd, const char *text)
{
    wchar_t *wtext = wide_view_text(text), *cur;
    int num;
    if (!wtext) return;
    num = GetWindowTextLengthW(hwnd);
    cur = malloc(((size_t)num + 2) * sizeof *cur);
    if (cur) {
        GetWindowTextW(hwnd, cur, num + 1);
        if (wcscmp(cur, wtext) == 0) {
            free(cur);
            free(wtext);
            return;
        }
        free(cur);
    }
    free(wtext);
    set_text_at(hwnd, text, 0);
}

/* Why the machine stopped, in words, under the breakpoint list. */
static void refresh_bp_stop(debugger *dbgwin)
{
    msxdebug_snapshot snap;
    char line[260];
    if (msxdebug_stop_snapshot(dbgwin->dbg, &snap)) snprintf(line, sizeof line, "Stopped: %s", snap.text);
    else snprintf(line, sizeof line, "Running.");
    set_label(dbgwin->bp_stop, line);
}

/* Rebuilt only when a line changed, keeping the selection. */
static void refresh_watches(debugger *dbgwin)
{
    static char last[MSXWATCH_MAX * 220];
    static char now[MSXWATCH_MAX * 220];
    const int num = msxwatch_count();
    const int keep = (int)SendMessageA(dbgwin->wt_list, LB_GETCURSEL, 0, 0);
    size_t len = 0;
    int idx;

    now[0] = '\0';
    for (idx = 0; idx < num; idx++) {
        char line[200];
        line[0] = '\0';
        msxwatch_format(dbgwin->dbg, idx, line, (int)sizeof line);
        len = addf(now, len, sizeof now, "%s\n", line);
    }
    if (strcmp(now, last) == 0 && (int)SendMessageA(dbgwin->wt_list, LB_GETCOUNT, 0, 0) == num) return;
    memcpy(last, now, sizeof last);
    SendMessageA(dbgwin->wt_list, WM_SETREDRAW, FALSE, 0);
    SendMessageA(dbgwin->wt_list, LB_RESETCONTENT, 0, 0);
    for (idx = 0; idx < num; idx++) {
        char line[200];
        line[0] = '\0';
        msxwatch_format(dbgwin->dbg, idx, line, (int)sizeof line);
        SendMessageA(dbgwin->wt_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (keep >= 0 && keep < num) SendMessageA(dbgwin->wt_list, LB_SETCURSEL, (WPARAM)keep, 0);
    SendMessageA(dbgwin->wt_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(dbgwin->wt_list, NULL, TRUE);
}

/* "slot 3-2" or "slot 1" */
static void slot_name(int slot, int sub, char *dst, size_t cap)
{
    if (sub >= 0) snprintf(dst, cap, "slot %d-%d", slot, sub);
    else snprintf(dst, cap, "slot %d", slot);
}

/* The machine and its devices, and the memory map as the slots are selected now. */
/* Cart & I/O: the engine is asked (cart info, I/O, the map) once per stop, see cache_due. */
static void cart_fetch(debugger *dbgwin)
{
    if (!cache_due(dbgwin, dbgwin->cart_valid, &dbgwin->cart_serial, &dbgwin->cart_stamp, &dbgwin->cart_dirty)) return;
    msxdebug_cart_info(dbgwin->dbg, &dbgwin->cart_info);
    if (!msxdebug_io_get(dbgwin->dbg, &dbgwin->cart_ports)) dbgwin->cart_ports.valid = 0;
    msxdebug_io_state(dbgwin->dbg, dbgwin->cart_state, (int)sizeof dbgwin->cart_state);
    dbgwin->cart_valid = 1;
    dbgwin->cart_dirty = 0;
}

static void refresh_cart(debugger *dbgwin)
{
    static char buf[8192];
    const msxdebug_cart cart = dbgwin->cart_info;
    const msxdebug_io ports = dbgwin->cart_ports;
    msxdebug_region map[16];
    size_t len = 0;
    int idx, num;
    const char *ptr;

    len = addf(buf, len, sizeof buf, "Machine   %s (%s)\n", cart.machine[0] ? cart.machine : "(none)", cart.type);
    if (cart.z80_hz) len = addf(buf, len, sizeof buf, "CPU       %d Hz\n", cart.z80_hz);
    len = addf(buf, len, sizeof buf, "FujiNet   %s\n",
               cart.fujinet_external ? (cart.fujinet_link ? "external, cartridge connected" : "external, cartridge not connected")
               : cart.fujinet ? (cart.fujinet_link ? "cartridge in a slot, link up" : "cartridge in a slot, link down")
                              : "no cartridge");
    len = addf(buf, len, sizeof buf, "Chips     %s%s%s%s%s\n", cart.scc ? "SCC " : "", cart.music ? "MSX-MUSIC " : "",
               cart.audio ? "MSX-AUDIO " : "", cart.mapper ? "memory mapper " : "",
               (cart.scc || cart.music || cart.audio || cart.mapper) ? "" : "none");
    len = addf(buf, len, sizeof buf, "Devices   ");
    for (ptr = cart.devices; *ptr; ptr++) {
        if (ptr[0] == ',' && ptr[1] == ' ') {
            len = addf(buf, len, sizeof buf, "\n          ");
            ptr++;
        } else {
            len = addf(buf, len, sizeof buf, "%c", *ptr);
        }
    }
    len = addf(buf, len, sizeof buf, "\n\nMemory map (as the slots are selected now)\n");
    num = cached_memory_map(dbgwin, map, 16);
    if (num <= 0) len = addf(buf, len, sizeof buf, "  (no machine)\n");
    for (idx = 0; idx < num; idx++)
        len = addf(buf, len, sizeof buf, "  %04X-%04X  %c%c  %s\n", map[idx].lo, map[idx].hi, map[idx].readable ? 'r' : '-',
                   map[idx].writable ? 'w' : '-', map[idx].name);
    if (ports.valid) {
        len = addf(buf, len, sizeof buf, "\nCPU pages\n");
        for (idx = 0; idx < 4; idx++) {
            char notify[24];
            slot_name(ports.page_slot[idx], ports.page_subslot[idx], notify, sizeof notify);
            len = addf(buf, len, sizeof buf, "  $%04X  %-10s %s\n", idx * 0x4000, notify, ports.page_device[idx]);
        }
    }
    set_view(dbgwin->cart_text, buf);
}

/* The slot and mapper registers, the PSG, and what the keyboard and joystick ports read. */
static void refresh_io(debugger *dbgwin)
{
    static char buf[16384];
    const msxdebug_io ports = dbgwin->cart_ports;
    size_t len = 0;
    int idx;

    snprintf(buf, sizeof buf, "%s", dbgwin->cart_state);
    len = strlen(buf);
    if (ports.valid) {
        len = addf(buf, len, sizeof buf, "\nKeyboard matrix (a 0 bit is a pressed key)\n");
        for (idx = 0; idx < ports.keymatrix_rows && idx < 16; idx++)
            len = addf(buf, len, sizeof buf, "  row %2d  %02X\n", idx, ports.keymatrix[idx]);
        len = addf(buf, len, sizeof buf, "Joystick ports  A $%02X  B $%02X (a 1 bit is not pressed)\n", ports.joyport[0],
                   ports.joyport[1]);
        if (ports.rom_mappers > 0) len = addf(buf, len, sizeof buf, "\nROM mappers (the block at $0000 $2000 ... $E000)\n");
        for (idx = 0; idx < ports.rom_mappers && idx < 6; idx++) {
            int block;
            len = addf(buf, len, sizeof buf, "  %-24s", ports.rom[idx].device);
            for (block = 0; block < 8; block++) {
                if (ports.rom[idx].block[block] == 255) len = addf(buf, len, sizeof buf, " --");
                else len = addf(buf, len, sizeof buf, " %02X", ports.rom[idx].block[block]);
            }
            len = addf(buf, len, sizeof buf, "\n");
        }
        if (ports.slotmap[0]) len = addf(buf, len, sizeof buf, "\nopenMSX slotmap\n%s\n", ports.slotmap);
    }
    set_view(dbgwin->io_text, buf);
}

/* The PSG and the SCC, MSX-MUSIC and MSX-AUDIO chips the machine has, straight out of the chips (stopped only). */
static void refresh_sound(debugger *dbgwin)
{
    static char buf[24576];
    static msxdebug_sound snd;
    size_t len = 0;
    int voice, index;

    if (!msxdebug_is_stopped(dbgwin->dbg)) {
        set_view(dbgwin->sound_text, "Stop the machine to read the sound chips.");
        return;
    }
    buf[0] = '\0';
    memset(&snd, 0, sizeof snd);
    msxdebug_sound_get(dbgwin->dbg, &snd);
    if (snd.psg.present) {
        const msxdebug_psg *psg = &snd.psg;
        len = addf(buf, len, sizeof buf, "PSG (AY-3-8910)\n  regs");
        for (index = 0; index < 16; index++) len = addf(buf, len, sizeof buf, " %02X", psg->reg[index]);
        len = addf(buf, len, sizeof buf, "\n");
        for (voice = 0; voice < 3; voice++)
            len = addf(buf, len, sizeof buf, "  %c  period $%03X  %8.1f Hz  tone %-3s  noise %-3s  level %2d%s\n", 'A' + voice,
                       psg->period[voice], psg->freq[voice], psg->tone_on[voice] ? "on" : "off", psg->noise_on[voice] ? "on" : "off",
                       psg->level[voice], psg->envelope[voice] ? " (envelope)" : "");
        len = addf(buf, len, sizeof buf, "  noise period %d   envelope period $%04X  shape $%X   mixer $%02X (a set bit is OFF)\n\n",
                   psg->noise_period, psg->env_period, psg->env_shape, psg->reg[7]);
    } else {
        len = addf(buf, len, sizeof buf, "PSG: not found\n\n");
    }
    if (snd.scc.present) {
        const msxdebug_scc *scc = &snd.scc;
        len = addf(buf, len, sizeof buf, "SCC  (%s)\n", scc->name);
        for (voice = 0; voice < 5; voice++) {
            len = addf(buf, len, sizeof buf, "  %d  period $%03X  %8.1f Hz  volume %2d  %s   wave", voice + 1, scc->period[voice],
                       scc->freq[voice], scc->volume[voice], scc->enabled[voice] ? "on " : "off");
            for (index = 0; index < 32; index++) len = addf(buf, len, sizeof buf, " %02X", scc->wave[voice][index]);
            len = addf(buf, len, sizeof buf, "\n");
        }
        len = addf(buf, len, sizeof buf, "\n");
    }
    if (snd.opll.present) {
        const msxdebug_opll *opll = &snd.opll;
        len = addf(buf, len, sizeof buf, "MSX-MUSIC  (%s)%s\n", opll->name, opll->rhythm ? "  rhythm mode" : "");
        for (voice = 0; voice < 9; voice++)
            len = addf(buf, len, sizeof buf, "  %d  F-number $%03X  block %d  %8.1f Hz  key %-3s  instrument %2d  volume %2d\n", voice + 1,
                       opll->fnum[voice], opll->block[voice], opll->freq[voice], opll->key_on[voice] ? "on" : "off", opll->instrument[voice], opll->volume[voice]);
        len = addf(buf, len, sizeof buf, "  regs");
        for (index = 0; index < 0x39; index++) len = addf(buf, len, sizeof buf, "%s%02X", index % 16 ? " " : "\n    ", opll->reg[index]);
        len = addf(buf, len, sizeof buf, "\n\n");
    }
    if (snd.y8950.present) {
        const msxdebug_y8950 *audio = &snd.y8950;
        len = addf(buf, len, sizeof buf, "MSX-AUDIO  (%s)\n  regs", audio->name);
        for (index = 0; index < 256; index++) len = addf(buf, len, sizeof buf, "%s%02X", index % 16 ? " " : "\n    ", audio->reg[index]);
        len = addf(buf, len, sizeof buf, "\n");
    }
    set_view(dbgwin->sound_text, buf);
}

/* Trace (0: live, the ring as it fills) and History (1: what ran before the
 * machine stopped, while it is stopped) read the same ring: the newest
 * HIST_SHOWN entries that pass the filter, oldest first. */
static void hist_refresh(debugger *dbgwin, int idx, int force)
{
    static msxdebug_trace buf[HIST_COPY];
    static char text[HIST_SHOWN * 260];
    const int live = idx == 0;
    const int stopped = msxdebug_is_stopped(dbgwin->dbg) != 0;
    const int on = live ? msxdebug_trace_enabled(dbgwin->dbg) : msxdebug_history_enabled(dbgwin->dbg);
    const uint64_t count = live ? msxdebug_trace_total(dbgwin->dbg) : msxdebug_history_count(dbgwin->dbg);
    uint16_t low = 0, high = 0xFFFF;
    int filter = MSXDEBUG_TRACE_ALL, num, pos, shown = 0, hit[HIST_SHOWN];
    size_t len = 0;
    char note[120];

    SendMessageA(dbgwin->h_rec[idx], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    InvalidateRect(dbgwin->h_rec[idx], NULL, FALSE);
    if (!force && count == dbgwin->h_seen[idx] && (live || stopped)) return;
    dbgwin->h_seen[idx] = count;
    if (!live && !stopped) {
        set_label(dbgwin->h_note[idx], "Stop the machine to read its history.");
        return;
    }
    switch ((int)SendMessageA(dbgwin->h_filter[idx], CB_GETCURSEL, 0, 0)) {
    case 1: filter = MSXDEBUG_TRACE_BRANCHES; break;
    case 2: {
        char row[64];
        GetWindowTextA(dbgwin->h_range[idx], row, sizeof row);
        if (msxsym_parse_range(dbgwin->dbg, row, &low, &high)) filter = MSXDEBUG_TRACE_RANGE;
        break;
    }
    default: break;
    }
    if (live) {
        num = msxdebug_trace_read(dbgwin->dbg, buf, HIST_COPY);      /* newest first */
        for (pos = 0; pos < num && shown < HIST_SHOWN; pos++)
            if (msxdebug_trace_match(&buf[pos], filter, low, high)) hit[shown++] = pos;
        text[0] = '\0';
        if (!on && num == 0) snprintf(text, sizeof text, "The trace is off. Press Record, then let the machine run.");
        for (pos = shown - 1; pos >= 0 && len + 280 < sizeof text; pos--) {
            char line[256];
            msxdebug_trace_format(&buf[hit[pos]], line, (int)sizeof line);
            len = addf(text, len, sizeof text, "%s\n", line);
        }
    } else {
        num = msxdebug_history_copy(dbgwin->dbg, buf, HIST_COPY);    /* oldest first */
        for (pos = num - 1; pos >= 0 && shown < HIST_SHOWN; pos--)
            if (msxdebug_history_match(&buf[pos], filter, low, high)) hit[shown++] = pos;
        text[0] = '\0';
        if (!on && num == 0) snprintf(text, sizeof text, "History is off. Press Record, then let the machine run.");
        for (pos = shown - 1; pos >= 0 && len + 280 < sizeof text; pos--) {
            char line[256];
            msxdebug_history_format(&buf[hit[pos]], line, (int)sizeof line);
            len = addf(text, len, sizeof text, "%s\n", line);
        }
    }
    set_text_at(dbgwin->h_text[idx], text, 1);
    snprintf(note, sizeof note, "%s%llu instructions recorded, last %d shown", on ? "Recording. " : "",
             (unsigned long long)count, shown);
    set_label(dbgwin->h_note[idx], note);
}

/* The symbols that pass the filter, one a line with its address kept for a
 * click; rebuilt only when the text would change. */
static void refresh_symbols(debugger *dbgwin)
{
    static int picked[SYM_MAX];
    char filter[64], count[64];
    char *text;
    size_t len = 0, cap = (size_t)SYM_MAX * 96;
    int num, idx, shown = 0;

    GetWindowTextA(dbgwin->sym_filter, filter, sizeof filter);
    num = msxdebug_symbols_filter(dbgwin->dbg, filter, picked, SYM_MAX);
    if (num < 0) num = 0;
    text = malloc(cap);
    if (!text) return;
    text[0] = '\0';
    for (idx = 0; idx < num; idx++) {
        uint16_t addr;
        char name[80];
        if (!msxdebug_symbols_get(dbgwin->dbg, picked[idx], &addr, name, (int)sizeof name)) continue;
        len = addf(text, len, cap, "%04X  %s\n", addr, name);
        dbgwin->sym_addr[shown++] = addr;
    }
    dbgwin->sym_lines = shown;
    if (dbgwin->sym_ok && dbgwin->sym_text && strcmp(text, dbgwin->sym_text) == 0) {
        free(text);
        return;
    }
    free(dbgwin->sym_text);
    dbgwin->sym_text = text;
    dbgwin->sym_ok = 1;
    snprintf(dbgwin->sym_filter_text, sizeof dbgwin->sym_filter_text, "%s", filter);
    set_text_at(dbgwin->sym_list, text, 0);
    snprintf(count, sizeof count, "%d symbol%s%s", shown, shown == 1 ? "" : "s", filter[0] ? " (filtered)" : "");
    set_label(dbgwin->sym_count, count);
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
static void refresh_fujinet(debugger *dbgwin, int force)
{
    static msx_fuji_packet ptr[TRACE_MAX];
    static char buf[TRACE_MAX * 320 + 128];   /* an entry is at most ~300 */
    static char state[2048];
    char note[200];
    size_t len = 0;
    int num = msxdebug_fujinet_trace(dbgwin->dbg, ptr, TRACE_MAX), idx, follow;

    if (msxsession_fujinet_external(dbgwin->session))
        snprintf(note, sizeof note, "External FujiNet %s, cartridge %s.",
                 msxsession_fujinet_endpoint(dbgwin->session),
                 msxsession_fujinet_link(dbgwin->session) ? "connected" : "not connected");
    else if (!msxsession_fujinet_running(dbgwin->session))
        snprintf(note, sizeof note, "FujiNet is off (disabled, or its runtime did not start).");
    else
        snprintf(note, sizeof note, "FujiNet is on, cartridge %s.",
                 msxsession_fujinet_link(dbgwin->session) ? "connected" : "not connected");
    set_label(dbgwin->fuji_note, note);
    state[0] = '\0';
    msxdebug_fujinet_state(dbgwin->dbg, state, (int)sizeof state);
    set_text_at(dbgwin->fuji_state, state, 0);
    if (num < 0) num = 0;

    if (!force && num == dbgwin->fuji_count && (num == 0 || ptr[num - 1].serial == dbgwin->fuji_last)) return;
    follow = force || dbgwin->fuji_count < 0 || edit_at_end(dbgwin, dbgwin->fuji_text);
    dbgwin->fuji_count = num;
    dbgwin->fuji_last = num > 0 ? ptr[num - 1].serial : 0;
    buf[0] = '\0';
    for (idx = 0; idx < num; idx++) {
        int k;
        len = addf(buf, len, sizeof buf, "%s%s", idx ? "\n" : "", ptr[idx].text);
        if (ptr[idx].repeat > 1) len = addf(buf, len, sizeof buf, " \xc3\x97%u", (unsigned)ptr[idx].repeat);
        len = addf(buf, len, sizeof buf, "\n     ");
        for (k = 0; k < ptr[idx].nbytes && k < (int)sizeof ptr[idx].data; k++)
            len = addf(buf, len, sizeof buf, "%02X ", ptr[idx].data[k]);
        if (ptr[idx].nbytes > (int)sizeof ptr[idx].data) len = addf(buf, len, sizeof buf, "\xe2\x80\xa6");
    }
    if (num == 0) len = addf(buf, len, sizeof buf, "(no FujiBus packets since the trace was cleared)");
    set_text_at(dbgwin->fuji_text, buf, follow);
}


/* One tab brought up to date (force: the live ones too, the Video tab redrawn). */
static void refresh_page(debugger *dbgwin, int page, int force)
{
    switch (page) {
    case TAB_MEMORY: refresh_mem(dbgwin); break;
    case TAB_DISASM: refresh_disasm(dbgwin); break;
    case TAB_BREAKS: refresh_bps(dbgwin); refresh_bp_stop(dbgwin); break;
    case TAB_WATCHES: refresh_watches(dbgwin); break;
    case TAB_CART: cart_fetch(dbgwin); refresh_cart(dbgwin); refresh_io(dbgwin); break;
    case TAB_TRACE: hist_refresh(dbgwin, 0, force); break;
    case TAB_HISTORY: hist_refresh(dbgwin, 1, force); break;
    case TAB_VIDEO: if (dbgwin->video) dbg_video_refresh(dbgwin->video, force); break;
    case TAB_SOUND: refresh_sound(dbgwin); break;
    case TAB_SYMBOLS: refresh_symbols(dbgwin); break;
    case TAB_FUJINET: refresh_fujinet(dbgwin, 0); break;
    default: break;
    }
}

/* The tabs on screen: the one the tab control shows, and any in a window of its own. */
static void refresh_shown(debugger *dbgwin, int force)
{
    int ptr;
    for (ptr = 0; ptr < TAB_COUNT; ptr++)
        if (page_visible(dbgwin, ptr)) refresh_page(dbgwin, ptr, force);
}

/* At a new stop the console says why, as the other frontends' do. */
static void console_stop(debugger *dbgwin, const msxdebug_snapshot *snap)
{
    char line[220];
    if (!snap->paused || snap->serial == dbgwin->con_serial) return;
    dbgwin->con_serial = snap->serial;
    snprintf(line, sizeof line, "\xe2\x80\x94 %s", snap->text);
    console_output(dbgwin, line);
}

/* After anything that may have moved the engine: the status, the register
 * strip and the tabs showing follow it. */
static void resync(debugger *dbgwin)
{
    msxdebug_snapshot snap;
    const int stopped = msxdebug_stop_snapshot(dbgwin->dbg, &snap) != 0;
    console_stop(dbgwin, &snap);
    dbgwin->was_stopped = stopped;
    dbgwin->seen_serial = snap.serial;
    dbgwin->seen_gen = msxdebug_generation(dbgwin->dbg);
    dbgwin->running_ticks = 0;
    refresh_status(dbgwin);
    refresh_cpu(dbgwin);
    refresh_shown(dbgwin, 1);
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
static void settle_and_sync(debugger *dbgwin, int expect_stop)
{
    int idx;
    dbgwin->notice[0] = '\0';        /* the machine moved: its new state replaces a notice */
    for (idx = 0; expect_stop && idx < 40 && !msxdebug_is_stopped(dbgwin->dbg); idx++)
        Sleep(1);
    resync(dbgwin);
}

static void tick(debugger *dbgwin)
{
    msxdebug_snapshot snap;
    unsigned gen;
    int stopped;
    if (!IsWindowVisible(dbgwin->hwnd)) return;
    if (dbgwin->notice[0] && !notice_showing(dbgwin)) {
        dbgwin->notice[0] = '\0';
        refresh_status(dbgwin);
    }
    gen = msxdebug_generation(dbgwin->dbg);
    stopped = msxdebug_stop_snapshot(dbgwin->dbg, &snap) != 0;
    if (gen != dbgwin->seen_gen || stopped != dbgwin->was_stopped || snap.serial != dbgwin->seen_serial) {
        if (stopped != dbgwin->was_stopped) dbgwin->notice[0] = '\0';   /* a stop (or a go) says so at once */
        resync(dbgwin);
        return;
    }
    /* the pictures move while the machine runs */
    if (page_visible(dbgwin, TAB_VIDEO) && dbgwin->video) dbg_video_refresh(dbgwin->video, 0);
    if (!stopped && ++dbgwin->running_ticks >= 5) {
        /* running: the live values of the tabs showing, twice a second */
        dbgwin->running_ticks = 0;
        refresh_status(dbgwin);
        refresh_cpu(dbgwin);
        refresh_shown(dbgwin, 0);
    } else if (stopped) {
        if (page_visible(dbgwin, TAB_FUJINET) && ++dbgwin->fuji_ticks >= 5) {
            dbgwin->fuji_ticks = 0;
            refresh_fujinet(dbgwin, 0);
        }
        /* a stop that came inside the debounce gap of the slow reads: catch up */
        if (page_visible(dbgwin, TAB_CART) && dbgwin->cart_dirty) refresh_page(dbgwin, TAB_CART, 0);
        if (page_visible(dbgwin, TAB_MEMORY) && dbgwin->mem_map_dirty) refresh_page(dbgwin, TAB_MEMORY, 0);
    }
}

/* ---- actions ------------------------------------------------------------------ */

static void on_run(debugger *dbgwin)
{
    const int stopped = msxdebug_is_stopped(dbgwin->dbg);
    if (stopped) msxdebug_resume(dbgwin->dbg);
    else msxdebug_stop(dbgwin->dbg);
    settle_and_sync(dbgwin, !stopped);
}

static void on_run_to(debugger *dbgwin)
{
    if (dbgwin->selected < 0) {
        toast(dbgwin, "Select a line in the Disassembly to run to");
        return;
    }
    msxdebug_run_to(dbgwin->dbg, (uint16_t)dbgwin->selected);
    settle_and_sync(dbgwin, 1);
}

static void toggle_breakpoint(debugger *dbgwin, int addr)
{
    msxdebug_breakpoint_toggle(dbgwin->dbg, (uint16_t)addr);
    resync(dbgwin);
}

/* F9: the breakpoint on the selected disassembly line. */
static void on_toggle_bp(debugger *dbgwin)
{
    if (dbgwin->selected < 0) {
        toast(dbgwin, "Select a line in the Disassembly first");
        return;
    }
    toggle_breakpoint(dbgwin, dbgwin->selected);
}

static void on_console(debugger *dbgwin)
{
    static char out[65536];
    char buf[256], line[300], *cmd;
    const int was = msxdebug_is_stopped(dbgwin->dbg);

    edit_text(dbgwin->con_in, buf, sizeof buf);
    cmd = strip(buf);
    if (!*cmd) return;
    /* history: the newest last, no repeats of the one before */
    if (dbgwin->nhistory == 0 || strcmp(dbgwin->history[dbgwin->nhistory - 1], cmd) != 0) {
        if (dbgwin->nhistory == HISTORY_MAX) {
            memmove(dbgwin->history[0], dbgwin->history[1], sizeof dbgwin->history[0] * (HISTORY_MAX - 1));
            dbgwin->nhistory--;
        }
        snprintf(dbgwin->history[dbgwin->nhistory++], sizeof dbgwin->history[0], "%s", cmd);
    }
    dbgwin->history_pos = dbgwin->nhistory;
    snprintf(line, sizeof line, "> %s\n", cmd);
    edit_to_end(dbgwin->con_out);     /* a command brings the reader back down */
    console_append(dbgwin, line);

    out[0] = '\0';
    msxdebug_command(dbgwin->dbg, cmd, out, sizeof out);
    SetWindowTextA(dbgwin->con_in, "");
    console_output(dbgwin, out);
    settle_and_sync(dbgwin, was);
}

/* Up and Down walk the commands already run; past the newest is an empty
 * line again. */
static void history_step(debugger *dbgwin, int newer)
{
    int len;
    if (dbgwin->nhistory == 0) return;
    dbgwin->history_pos += newer ? 1 : -1;
    if (dbgwin->history_pos < 0) dbgwin->history_pos = 0;
    if (dbgwin->history_pos >= dbgwin->nhistory) {
        dbgwin->history_pos = dbgwin->nhistory;
        SetWindowTextA(dbgwin->con_in, "");
        return;
    }
    SetWindowTextA(dbgwin->con_in, dbgwin->history[dbgwin->history_pos]);
    len = GetWindowTextLengthA(dbgwin->con_in);
    SendMessageA(dbgwin->con_in, EM_SETSEL, (WPARAM)len, (LPARAM)len);
}

/* Tab: the engine completes the end of the line; one answer takes its
 * place, several are listed (and the part they share is typed). */
static void complete_console(debugger *dbgwin)
{
    char text[256], comps[4096], common[256] = "", line[256];
    char *sp, *cur, *nl;
    int num, first = 1;
    edit_text(dbgwin->con_in, text, sizeof text);
    num = msxdebug_completions(dbgwin->dbg, text, comps, sizeof comps);
    if (num <= 0) return;
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
    if (num > 1) console_append(dbgwin, comps);
    sp = strrchr(text, ' ');
    if ((int)strlen(common) < (int)strlen(sp ? sp + 1 : text))
        snprintf(common, sizeof common, "%s", sp ? sp + 1 : text);   /* never shorter than typed */
    if (sp) sp[1] = '\0'; else text[0] = '\0';
    {
        char joined[600];
        snprintf(joined, sizeof joined, "%s%s%s", text, common, num == 1 ? " " : "");
        SetWindowTextA(dbgwin->con_in, joined);
        SendMessageA(dbgwin->con_in, EM_SETSEL, (WPARAM)strlen(joined), (LPARAM)strlen(joined));
    }
}


/* A value typed into a register box: hex ("1F", "$1F", "0x1F"), "#31" decimal,
 * or anything the engine's expressions take (a label, "hl + 2", "[$C000]"). */
static int parse_value(debugger *dbgwin, const char *text, long *out, char *err, int errsz)
{
    char buf[128], *ptr;
    snprintf(buf, sizeof buf, "%s", text);
    ptr = strip(buf);
    if (!*ptr) {
        snprintf(err, (size_t)errsz, "Type a value");
        return 0;
    }
    if (parse_num(ptr, out)) return 1;
    return msxdebug_eval(dbgwin->dbg, ptr, out, err, errsz) == 0;
}

static int *reg_field(msxdebug_cpu *cur, int ctl_id)
{
    switch (ctl_id) {
    case MSX_REG_AF: return &cur->af;
    case MSX_REG_BC: return &cur->bc;
    case MSX_REG_DE: return &cur->de;
    case MSX_REG_HL: return &cur->hl;
    case MSX_REG_IX: return &cur->ix;
    case MSX_REG_IY: return &cur->iy;
    case MSX_REG_SP: return &cur->sp;
    case MSX_REG_PC: return &cur->pc;
    case MSX_REG_AF2: return &cur->af2;
    case MSX_REG_BC2: return &cur->bc2;
    case MSX_REG_DE2: return &cur->de2;
    case MSX_REG_HL2: return &cur->hl2;
    case MSX_REG_I: return &cur->i;
    case MSX_REG_R: return &cur->r;
    case MSX_REG_IM: return &cur->im;
    default: return NULL;
    }
}

/* Writes the strip's CPU back (every register in one go); the reason it was
 * refused goes to the status line. */
static void regs_write(debugger *dbgwin, const msxdebug_cpu *cur)
{
    char err[160];
    err[0] = '\0';
    if (!msxdebug_regs_set(dbgwin->dbg, cur, err, sizeof err)) toast(dbgwin, err[0] ? err : "That register could not be set");
}

/* A register box (Enter). Refused, with a notice, unless stopped. */
static void on_reg(debugger *dbgwin, int idx)
{
    char buf[64], err[160];
    long val;
    msxdebug_cpu cur;
    int *field;
    edit_text(dbgwin->reg[idx], buf, sizeof buf);
    err[0] = '\0';
    if (!msxdebug_is_stopped(dbgwin->dbg)) {
        toast(dbgwin, "Stop the machine first to change a register");
    } else if (!parse_value(dbgwin, buf, &val, err, sizeof err)) {
        toast(dbgwin, err[0] ? err : "Not a number (hex, # decimal, or an expression)");
    } else {
        msxdebug_cpu_get(dbgwin->dbg, &cur);
        field = reg_field(&cur, k_reg_ids[idx]);
        if (field) *field = (int)val;
        regs_write(dbgwin, &cur);
    }
    SetFocus(dbgwin->hwnd);
    resync(dbgwin);
}

static void on_check(debugger *dbgwin, int idx)
{
    msxdebug_cpu cur;
    const int on = SendMessageA(dbgwin->chk[idx], BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (dbgwin->updating) return;
    if (!msxdebug_is_stopped(dbgwin->dbg)) {
        toast(dbgwin, "Stop the machine first to change a flag");
        refresh_cpu(dbgwin);     /* the box goes back to the CPU's */
        return;
    }
    msxdebug_cpu_get(dbgwin->dbg, &cur);
    if (idx == 6) cur.iff1 = on;
    else if (idx == 7) cur.iff2 = on;
    else cur.af = on ? (cur.af | k_chk_bits[idx]) : (cur.af & ~k_chk_bits[idx]);
    regs_write(dbgwin, &cur);
    resync(dbgwin);
}

/* Go to: the dump from just above an address, label, expression or range, its
 * bytes selected and scrolled to. */
static void on_mem_goto(debugger *dbgwin)
{
    char buf[64];
    uint16_t low, high;
    edit_text(dbgwin->mem_goto, buf, sizeof buf);
    if (!msxsym_parse_range(dbgwin->dbg, buf, &low, &high)) {
        dbgwin->mem_hl = 0;
        refresh_mem(dbgwin);
        toast(dbgwin, "Not an address, label or range");
        return;
    }
    show_in_memory(dbgwin, low, high);
}

/* The dump a page of MEM_BYTES on or back. */
static void on_mem_page(debugger *dbgwin, int dir)
{
    dbgwin->mem_top += dir * MEM_BYTES;
    if (dbgwin->mem_top < 0) dbgwin->mem_top = 0;
    if (dbgwin->mem_top > 0x10000 - MEM_BYTES) dbgwin->mem_top = 0x10000 - MEM_BYTES;
    refresh_mem(dbgwin);
}

/* "address value [value...]": bytes into memory from the address on. */
static void on_mem_write(debugger *dbgwin)
{
    char buf[256], *tok, *save = NULL, msg[96];
    int addr, num = 0;
    edit_text(dbgwin->mem_write, buf, sizeof buf);
    tok = strtok_s(buf, " \t,", &save);
    if (!tok) return;
    addr = resolve_addr(dbgwin, tok);
    if (addr < 0) {
        toast(dbgwin, "Not an address or label");
        return;
    }
    while ((tok = strtok_s(NULL, " \t,", &save)) != NULL) {
        long val;
        if (!parse_num(tok, &val) || val < 0 || val > 0xFF) {
            toast(dbgwin, "A value is a byte (00-FF)");
            return;
        }
        msxdebug_write(dbgwin->dbg, (uint16_t)(addr + num), (uint8_t)val);
        num++;
    }
    if (!num) {
        toast(dbgwin, "Type: address value [value...]");
        return;
    }
    snprintf(msg, sizeof msg, "Wrote %d byte%s at $%04X", num, num == 1 ? "" : "s", addr);
    toast(dbgwin, msg);
    resync(dbgwin);
}

static void on_jump(debugger *dbgwin)
{
    char buf[64];
    int addr;
    edit_text(dbgwin->jump, buf, sizeof buf);
    addr = resolve_addr(dbgwin, buf);
    if (addr < 0) {
        toast(dbgwin, "Not an address or label");
        return;
    }
    SendMessageA(dbgwin->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
    dbgwin->disasm_top = addr;
    dbgwin->selected = addr;
    refresh_disasm(dbgwin);
}


/* The selected breakpoint row's index, or -1. */
static int bp_selected(debugger *dbgwin)
{
    const int idx = (int)SendMessageA(dbgwin->bp_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    return idx >= 0 && idx < dbgwin->nbps ? idx : -1;
}

static void on_bp_remove(debugger *dbgwin)
{
    const int idx = bp_selected(dbgwin);
    if (idx < 0) {
        set_label(dbgwin->bp_msg, "Select a breakpoint to remove");
        return;
    }
    set_label(dbgwin->bp_msg, "");
    msxdebug_breakpoint_remove(dbgwin->dbg, dbgwin->bps[idx].id);
    resync(dbgwin);
}

static void on_bp_add(debugger *dbgwin)
{
    char start_text[64], end_text[64], cond[256], err[160];
    int start, end, sel, ctl_id, port;
    edit_text(dbgwin->bp_start, start_text, sizeof start_text);
    edit_text(dbgwin->bp_end, end_text, sizeof end_text);
    edit_text(dbgwin->bp_cond, cond, sizeof cond);
    sel = (int)SendMessageA(dbgwin->bp_type, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= NBPTYPES) sel = 0;
    port = (k_bp_type_ids[sel] & (MSXDEBUG_BP_IN | MSXDEBUG_BP_OUT)) != 0;
    start = resolve_addr(dbgwin, strip(start_text));
    if (start < 0) {
        set_label(dbgwin->bp_msg, port ? "The port is not a number" : "The start is not an address, label or expression");
        return;
    }
    end = strip(end_text)[0] ? resolve_addr(dbgwin, strip(end_text)) : start;
    if (end < 0) {
        set_label(dbgwin->bp_msg, "The end is not an address, label or expression");
        return;
    }
    if (end < start) end = start;
    if (port && end > 0xFF) {
        set_label(dbgwin->bp_msg, "An I/O port is $00-$FF");
        return;
    }
    err[0] = '\0';
    ctl_id = msxdebug_breakpoint_add(dbgwin->dbg, k_bp_type_ids[sel], (uint16_t)start, (uint16_t)end,
                                 strip(cond), err, sizeof err);
    if (ctl_id < 0) {
        set_label(dbgwin->bp_msg, err[0] ? err : "That breakpoint could not be set");
        return;
    }
    set_label(dbgwin->bp_msg, "");
    SetWindowTextA(dbgwin->bp_start, "");
    SetWindowTextA(dbgwin->bp_end, "");
    SetWindowTextA(dbgwin->bp_cond, "");
    resync(dbgwin);
}

/* The type decides what the boxes take. */
static void on_bp_type(debugger *dbgwin)
{
    const int sel = (int)SendMessageA(dbgwin->bp_type, CB_GETCURSEL, 0, 0);
    const int port = sel >= 4;
    SendMessageW(dbgwin->bp_start, EM_SETCUEBANNER, TRUE, (LPARAM)(port ? L"port, e.g. 98" : L"4010 / label / expression"));
    SendMessageW(dbgwin->bp_end, EM_SETCUEBANNER, TRUE, (LPARAM)(port ? L"(last port)" : L"(end)"));
}

/* ---- watches and the memory search ------------------------------------------- */

static void add_watch(debugger *dbgwin)
{
    char text[128];
    edit_text(dbgwin->wt_entry, text, sizeof text);
    if (msxwatch_add(strip(text)) < 0) {
        toast(dbgwin, "Type an address, label, expression or a range (c000-c0ff)");
        return;
    }
    SetWindowTextA(dbgwin->wt_entry, "");
    refresh_watches(dbgwin);
}

static int watch_range(debugger *dbgwin, uint16_t *low, uint16_t *high)
{
    const int sel = (int)SendMessageA(dbgwin->wt_list, LB_GETCURSEL, 0, 0);
    return sel >= 0 && msxwatch_range(dbgwin->dbg, sel, low, high);
}

/* Break on a write that changes the watched bytes. */
static void watch_break(debugger *dbgwin, uint16_t low, uint16_t high)
{
    char err[160];
    err[0] = '\0';
    if (msxdebug_breakpoint_add(dbgwin->dbg, MSXDEBUG_BP_WRITE, low, high, "value != old", err, sizeof err) >= 0)
        toast(dbgwin, "Breaking when it changes");
    else
        toast(dbgwin, err[0] ? err : "That breakpoint could not be set");
    resync(dbgwin);
}

static void do_search(debugger *dbgwin, int backward)
{
    char text[200];
    uint8_t pat[MSXSEARCH_MAX], mask;
    uint16_t found;
    int count, flags, row;

    edit_text(dbgwin->srch_entry, text, sizeof text);
    if (!msxsearch_parse((int)SendMessageA(dbgwin->srch_kind, CB_GETCURSEL, 0, 0), text, pat, &count, &mask, &flags)) {
        set_label(dbgwin->srch_note, "Type hex bytes (3E 00) or some text.");
        return;
    }
    if (!msxdebug_is_stopped(dbgwin->dbg)) {
        set_label(dbgwin->srch_note, "Stop the machine to search its memory.");
        return;
    }
    row = msxdebug_search_next(dbgwin->dbg, pat, count, mask, flags,
                             (uint16_t)(dbgwin->mem_hl ? (backward ? dbgwin->mem_hl_lo : dbgwin->mem_hl_lo + 1) : dbgwin->mem_top),
                             backward, &found);
    if (row > 0) {
        char note[40];
        snprintf(note, sizeof note, "Found at $%04X", found);
        set_label(dbgwin->srch_note, note);
        show_in_memory(dbgwin, found, found + count - 1);
    } else {
        set_label(dbgwin->srch_note, row < 0 ? "Stop the machine to search its memory." : "Not found.");
    }
}

static int pick_file(debugger *dbgwin, int save, const char *title, const char *filter, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = dbgwin->hwnd;
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

static void on_symbols(debugger *dbgwin)
{
    char path[MAX_PATH], msg[512];
    if (!pick_file(dbgwin, 0, "Load symbols",
                   "Symbol files (*.sym;*.map;*.noi;*.txt)\0*.sym;*.map;*.noi;*.txt\0All files\0*.*\0\0",
                   path, sizeof path))
        return;
    msg[0] = '\0';
    if (msxdebug_load_symbols(dbgwin->dbg, path, msg, sizeof msg) < 0 && !msg[0])
        snprintf(msg, sizeof msg, "No symbols found in %s", base_name(path));
    say(dbgwin, msg);
    resync(dbgwin);
}

static void on_save(debugger *dbgwin, int which)
{
    char path[MAX_PATH], msg[512], title[64];
    snprintf(title, sizeof title, "Save %s", k_save_titles[which]);
    if (!pick_file(dbgwin, 1, title, "All files\0*.*\0\0", path, sizeof path)) return;
    msg[0] = '\0';
    msxdebug_save(dbgwin->dbg, k_save_kinds[which], path, msg, sizeof msg);
    if (!msg[0]) snprintf(msg, sizeof msg, "Saved %s", base_name(path));
    say(dbgwin, msg);
}

static void save_menu(debugger *dbgwin)
{
    HMENU m = CreatePopupMenu();
    RECT row;
    int idx;
    for (idx = 0; idx < NSAVE; idx++) AppendMenuA(m, MF_STRING, (UINT_PTR)(IDC_SAVE_KIND0 + idx), k_save_titles[idx]);
    GetWindowRect(dbgwin->save_btn, &row);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN, row.left, row.bottom, 0, dbgwin->hwnd, NULL);
    DestroyMenu(m);
}

static void on_accept(debugger *dbgwin, int ctl_id)
{
    if (ctl_id >= IDC_REG0 && ctl_id <= IDC_REG_LAST) {
        on_reg(dbgwin, ctl_id - IDC_REG0);
        return;
    }
    switch (ctl_id) {
    case IDC_CON_IN: on_console(dbgwin); break;
    case IDC_MEM_GOTO: on_mem_goto(dbgwin); break;
    case IDC_MEM_WRITE: on_mem_write(dbgwin); break;
    case IDC_SRCH_ENTRY: do_search(dbgwin, 0); break;
    case IDC_JUMP: on_jump(dbgwin); break;
    case IDC_BP_START:
    case IDC_BP_END:
    case IDC_BP_COND: on_bp_add(dbgwin); break;
    case IDC_WT_ENTRY: add_watch(dbgwin); break;
    case IDC_SYM_FILTER: refresh_symbols(dbgwin); break;
    case IDC_H_RANGE0: hist_refresh(dbgwin, 0, 1); break;
    case IDC_H_RANGE1: hist_refresh(dbgwin, 1, 1); break;
    default: break;
    }
}

/* ---- the disassembly -------------------------------------------------------------- */

/* Drawn by hand, one row per line: the breakpoint gutter (a red dot when the
 * breakpoint is enabled, grey when disabled), address, bytes, label,
 * instruction, and the comment (a jump target's label) in grey. The PC's
 * line is filled with the accent, the selected one with the selection
 * colour. */
static void paint_disasm(debugger *dbgwin, HWND hwnd, HDC dc)
{
    RECT cur;
    int idx;
    const int gutter = scaled(dbgwin, GUTTER_W);
    GetClientRect(hwnd, &cur);
    FillRect(dc, &cur, GetSysColorBrush(COLOR_WINDOW));
    SelectObject(dc, dbgwin->mono);
    SetBkMode(dc, TRANSPARENT);
    if (dbgwin->line_count == 0) {
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        TextOutA(dc, gutter, 0, "(no disassembly)", 16);
        return;
    }
    for (idx = 0; idx < dbgwin->line_count && idx * dbgwin->line_h < cur.bottom; idx++) {
        const msxdebug_line *l = &dbgwin->lines[idx];
        const int pos_y = idx * dbgwin->line_h, ty = pos_y + (dbgwin->line_h - dbgwin->mono_h) / 2;
        const int is_pc = l->is_pc, sel = l->address == dbgwin->selected;
        COLORREF fg = GetSysColor(COLOR_WINDOWTEXT);
        RECT row;
        char text[240];
        int len;
        SetRect(&row, 0, pos_y, cur.right, pos_y + dbgwin->line_h);
        if (is_pc) {
            FillRect(dc, &row, dbgwin->accent);
            fg = INK_RGB;
        } else if (sel) {
            FillRect(dc, &row, GetSysColorBrush(COLOR_HIGHLIGHT));
            fg = GetSysColor(COLOR_HIGHLIGHTTEXT);
        }
        if (l->has_breakpoint) {
            const int size = dbgwin->line_h - scaled(dbgwin, 6) > 4 ? dbgwin->line_h - scaled(dbgwin, 6) : 4;
            HGDIOBJ oldb = SelectObject(dc, l->breakpoint_enabled ? dbgwin->bp_on : dbgwin->bp_off);
            HGDIOBJ oldp = SelectObject(dc, is_pc ? GetStockObject(WHITE_PEN) : GetStockObject(NULL_PEN));
            Ellipse(dc, scaled(dbgwin, 4), pos_y + (dbgwin->line_h - size) / 2, scaled(dbgwin, 4) + size + 1, pos_y + (dbgwin->line_h - size) / 2 + size + 1);
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
            TextOutA(dc, gutter + dbgwin->mono_w * COMMENT_COL, ty, text, len);
        }
    }
}

/* A click selects a line (it works while the machine runs, too); a click on
 * the gutter, or a double-click, toggles its breakpoint. The wheel and the
 * keys browse by instructions (and turn Follow PC off). */
static LRESULT CALLBACK disasm_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    debugger *dbgwin = g_dbg;
    if (!dbgwin) return DefWindowProcA(hwnd, msg, wparam, lparam);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT cur;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        GetClientRect(hwnd, &cur);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, cur.right > 0 ? cur.right : 1, cur.bottom > 0 ? cur.bottom : 1);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, dbgwin->mono);
        paint_disasm(dbgwin, hwnd, mem);
        BitBlt(dc, 0, 0, cur.right, cur.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        paint_disasm(dbgwin, hwnd, (HDC)wparam);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        /* more or fewer rows fit now */
        if (dbgwin->disasm == hwnd && page_visible(dbgwin, TAB_DISASM)) refresh_disasm(dbgwin);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const int line = dbgwin->line_h > 0 ? GET_Y_LPARAM(lparam) / dbgwin->line_h : -1;
        const int gutter = GET_X_LPARAM(lparam) < scaled(dbgwin, GUTTER_W);
        SetFocus(hwnd);
        if (line < 0 || line >= dbgwin->line_count) return 0;
        dbgwin->selected = dbgwin->lines[line].address;
        if (gutter ? msg == WM_LBUTTONDOWN : msg == WM_LBUTTONDBLCLK)
            toggle_breakpoint(dbgwin, dbgwin->lines[line].address);
        else
            InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        int rows = -delta * 3 / WHEEL_DELTA;
        if (!rows) rows = delta > 0 ? -1 : 1;
        SendMessageA(dbgwin->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
        dbgwin->disasm_top = row_address(dbgwin, dbgwin->disasm_top, rows);
        refresh_disasm(dbgwin);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_PRIOR || wparam == VK_NEXT || wparam == VK_UP || wparam == VK_DOWN) {
            const int half = disasm_rows(dbgwin) / 2 > 0 ? disasm_rows(dbgwin) / 2 : 1;
            const int rows = wparam == VK_PRIOR ? -half : wparam == VK_NEXT ? half : wparam == VK_UP ? -1 : 1;
            SendMessageA(dbgwin->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
            dbgwin->disasm_top = row_address(dbgwin, dbgwin->disasm_top, rows);
            refresh_disasm(dbgwin);
            return 0;
        }
        break;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

/* ---- edit subclassing ------------------------------------------------------------- */

static WNDPROC g_edit_proc;
static WNDPROC g_sym_proc;

/* Enter in a single-line field means "apply this value"; Tab in the console
 * completes and Up/Down recall. None may beep its way through the default
 * handler. The notifications go to the debugger window, whatever window the
 * field's tab is in. */
static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    const int ctl_id = (int)GetWindowLongPtrA(hwnd, GWLP_ID);
    HWND main = g_dbg ? g_dbg->hwnd : GetAncestor(hwnd, GA_ROOT);
    if (msg == WM_KEYDOWN && wparam == VK_RETURN) {
        PostMessageA(main, WM_DBG_ACCEPT, (WPARAM)ctl_id, 0);
        return 0;
    }
    if (msg == WM_CHAR && (wparam == '\r' || wparam == '\n')) return 0;
    if (ctl_id == IDC_CON_IN) {
        if (msg == WM_KEYDOWN && wparam == VK_TAB) {
            PostMessageA(main, WM_DBG_TAB, 0, 0);
            return 0;
        }
        if (msg == WM_KEYDOWN && (wparam == VK_UP || wparam == VK_DOWN)) {
            PostMessageA(main, WM_DBG_HISTORY, wparam == VK_DOWN, 0);
            return 0;
        }
        if (msg == WM_CHAR && wparam == VK_TAB) return 0;
        if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    }
    return CallWindowProcA(g_edit_proc, hwnd, msg, wparam, lparam);
}

/* A click on a symbol's line shows its address in Memory. The lines come
 * from a filtered list whose indices move, so the address of each line was
 * kept when the list was built (sym_addr); what is clicked is a line. */
static LRESULT CALLBACK sym_list_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_LBUTTONDOWN && g_dbg) {
        const int line = (int)HIWORD(SendMessageA(hwnd, EM_CHARFROMPOS, 0, lparam));
        if (line >= 0 && line < g_dbg->sym_lines)
            PostMessageA(g_dbg->hwnd, WM_DBG_GOTO, (WPARAM)g_dbg->sym_addr[line], 0);
    }
    return CallWindowProcA(g_sym_proc, hwnd, msg, wparam, lparam);
}

/* ---- construction ----------------------------------------------------------------- */

static HWND child_ex(debugger *dbgwin, DWORD ex_style, const char *cls, const char *text, DWORD style, int ctl_id, HFONT font)
{
    HWND hwnd = CreateWindowExA(ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                             dbgwin->new_parent ? dbgwin->new_parent : dbgwin->hwnd, (HMENU)(INT_PTR)ctl_id,
                             (HINSTANCE)GetWindowLongPtrA(dbgwin->hwnd, GWLP_HINSTANCE), NULL);
    SendMessageA(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
    return hwnd;
}

static HWND child(debugger *dbgwin, const char *cls, const char *text, DWORD style, int ctl_id, HFONT font)
{
    return child_ex(dbgwin, 0, cls, text, style, ctl_id, font);
}

/* A read-only monospace text view. */
static HWND view(debugger *dbgwin, int ctl_id, DWORD extra)
{
    HWND hwnd = child(dbgwin, "EDIT", "", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | extra,
                   ctl_id, dbgwin->mono);
    SendMessageA(hwnd, EM_SETLIMITTEXT, 0, 0);
    if (dbgwin->nviews < (int)(sizeof dbgwin->views / sizeof dbgwin->views[0])) dbgwin->views[dbgwin->nviews++] = hwnd;
    return hwnd;
}

static HWND field(debugger *dbgwin, int ctl_id, const wchar_t *cue)
{
    HWND hwnd = child(dbgwin, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, ctl_id, dbgwin->mono);
    g_edit_proc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)edit_subclass);
    if (cue) SendMessageW(hwnd, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
    return hwnd;
}

static HWND label(debugger *dbgwin, const char *text, HFONT font, DWORD style)
{
    return child(dbgwin, "STATIC", text, style | SS_NOPREFIX, IDC_LABEL_FIRST + dbgwin->nlabels++, font);
}

/* A caption or hint, drawn grey. */
static HWND dim_label(debugger *dbgwin, const char *text, DWORD style)
{
    HWND hwnd = label(dbgwin, text, dbgwin->ui_font, style);
    if (dbgwin->ndim < (int)(sizeof dbgwin->dim / sizeof dbgwin->dim[0])) dbgwin->dim[dbgwin->ndim++] = hwnd;
    return hwnd;
}

static HWND button(debugger *dbgwin, const char *text, int ctl_id)
{
    return child(dbgwin, "BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, ctl_id, dbgwin->ui_font);
}

static HWND checkbox(debugger *dbgwin, const char *text, int ctl_id)
{
    return child(dbgwin, "BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, ctl_id, dbgwin->ui_font);
}

static HWND combo(debugger *dbgwin, int ctl_id)
{
    return child(dbgwin, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ctl_id, dbgwin->ui_font);
}

/* A list box of lines in the monospace font. */
static HWND list_box(debugger *dbgwin, int ctl_id)
{
    return child_ex(dbgwin, WS_EX_CLIENTEDGE, "LISTBOX", "", WS_VSCROLL | WS_HSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                    ctl_id, dbgwin->mono);
}

static void size_columns(debugger *dbgwin)
{
    static const int widths[5] = { 70, 90, 150, 380, 80 };
    int idx;
    for (idx = 0; idx < 5; idx++)
        SendMessageA(dbgwin->bp_list, LVM_SETCOLUMNWIDTH, (WPARAM)idx, scaled(dbgwin, widths[idx]));
}

/* A tab control reads '&' as a mnemonic, so the label's own is doubled. */
static const char *tab_text(int page, char *buf, size_t cap)
{
    const char *in = k_tabs[page].label;
    size_t outbuf = 0;
    for (; *in && outbuf + 3 < cap; in++) {
        buf[outbuf++] = *in;
        if (*in == '&') buf[outbuf++] = '&';
    }
    buf[outbuf] = '\0';
    return buf;
}

static void build_controls(debugger *dbgwin)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(dbgwin->hwnd, GWLP_HINSTANCE);
    TCITEMA item;
    int idx;

    dbgwin->tabs = child(dbgwin, WC_TABCONTROLA, "", WS_CLIPSIBLINGS | WS_TABSTOP, IDC_TABS, dbgwin->ui_font);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (idx = 0; idx < TAB_COUNT; idx++) {
        char text[40];
        item.pszText = (char *)tab_text(idx, text, sizeof text);
        SendMessageA(dbgwin->tabs, TCM_INSERTITEMA, (WPARAM)idx, (LPARAM)&item);
        dbgwin->tab_page[idx] = idx;
    }
    dbgwin->ntabs = TAB_COUNT;
    for (idx = 0; idx < TAB_COUNT; idx++) {       /* a container per tab: its controls are its children */
        dbgwin->pages[idx] = CreateWindowExA(0, PAGE_CLASS, "", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, dbgwin->hwnd,
                                   NULL, inst, NULL);
        SetWindowLongPtrA(dbgwin->pages[idx], GWLP_USERDATA, (LONG_PTR)dbgwin->hwnd);
    }

    /* The toolbar. Run/Stop is owner-drawn so it can wear the accent colour
     * while the machine is stopped. */
    dbgwin->run_btn = child(dbgwin, "BUTTON", "Stop (F5)", BS_OWNERDRAW | WS_TABSTOP, IDC_RUN, dbgwin->ui_font);
    dbgwin->step_btn = button(dbgwin, "Step (F7)", IDC_STEP);
    dbgwin->over_btn = button(dbgwin, "Step Over (F8)", IDC_OVER);
    dbgwin->out_btn = button(dbgwin, "Step Out (Shift+F8)", IDC_OUT);
    dbgwin->run_to_btn = button(dbgwin, "Run to Cursor", IDC_RUN_TO);
    dbgwin->frame_btn = button(dbgwin, "Frame (Shift+F7)", IDC_FRAME);
    dbgwin->sym_btn = button(dbgwin, "Symbols...", IDC_SYMBOLS);
    dbgwin->save_btn = button(dbgwin, "Save...", IDC_SAVE);
    dbgwin->popout_btn = button(dbgwin, "Pop Out", IDC_POPOUT);
    dbgwin->help_btn = button(dbgwin, "Help", IDC_HELP_BTN);
    dbgwin->status = child(dbgwin, "STATIC", "", SS_RIGHT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_STATUS, dbgwin->ui_font);
    if (dbgwin->ndim < (int)(sizeof dbgwin->dim / sizeof dbgwin->dim[0])) dbgwin->dim[dbgwin->ndim++] = dbgwin->status;

    /* The register strip, under every tab. */
    for (idx = 0; idx < NREGS; idx++) {
        dbgwin->reg_label[idx] = dim_label(dbgwin, k_reg_names[idx], SS_RIGHT);
        dbgwin->reg[idx] = field(dbgwin, IDC_REG0 + idx, NULL);
        SendMessageA(dbgwin->reg[idx], EM_SETLIMITTEXT, 8, 0);
    }
    for (idx = 0; idx < NCHECKS; idx++) dbgwin->chk[idx] = checkbox(dbgwin, k_chk_names[idx], IDC_CHK0 + idx);
    dbgwin->halt_label = label(dbgwin, "", dbgwin->ui_bold, SS_LEFT);
    dbgwin->cpu_hint = dim_label(dbgwin, "Enter sets a register (hex, # decimal or an expression); they change only while the machine is stopped.",
                            SS_LEFT | SS_ENDELLIPSIS);

    /* Console */
    dbgwin->new_parent = dbgwin->pages[TAB_CONSOLE];
    dbgwin->con_out = view(dbgwin, IDC_CON_OUT, 0);
    SetWindowTextA(dbgwin->con_out, "MSX debugger console. Type 'help' for every command; Tab completes, Up/Down "
                               "recall. Anything else is a Tcl command for openMSX's own console.\r\n");
    dbgwin->con_prompt = child(dbgwin, "STATIC", ">", SS_LEFT, IDC_CON_PROMPT, dbgwin->mono);
    dbgwin->con_in = field(dbgwin, IDC_CON_IN, L"help, step, over, break 4010, bpa c000 if value == $ff, mem c000, watch, video, ...");

    /* Disassembly */
    dbgwin->new_parent = dbgwin->pages[TAB_DISASM];
    dbgwin->follow_pc = checkbox(dbgwin, "Follow PC", IDC_FOLLOW_PC);
    SendMessageA(dbgwin->follow_pc, BM_SETCHECK, BST_CHECKED, 0);
    dbgwin->jump_label = dim_label(dbgwin, "Jump to", SS_LEFT);
    dbgwin->jump = field(dbgwin, IDC_JUMP, L"4010 or a label");
    dbgwin->disasm_hint = dim_label(dbgwin, "Click a line to select it (Run to Cursor); click the gutter, double-click or "
                                  "press F9 for a breakpoint; scroll to browse",
                               SS_LEFT | SS_ENDELLIPSIS);
    dbgwin->disasm = CreateWindowExA(WS_EX_CLIENTEDGE, DISASM_CLASS, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10,
                                dbgwin->pages[TAB_DISASM], (HMENU)(INT_PTR)IDC_DISASM, inst, NULL);

    /* Memory: Go to, Write (poke), the search, the dump */
    dbgwin->new_parent = dbgwin->pages[TAB_MEMORY];
    dbgwin->goto_label = dim_label(dbgwin, "Go to", SS_LEFT);
    dbgwin->mem_goto = field(dbgwin, IDC_MEM_GOTO, L"c000 / label / c000-c0ff");
    dbgwin->mem_back = button(dbgwin, "Back 4K", IDC_MEM_BACK);
    dbgwin->mem_on = button(dbgwin, "On 4K", IDC_MEM_ON);
    dbgwin->write_label = dim_label(dbgwin, "Poke", SS_LEFT);
    dbgwin->mem_write = field(dbgwin, IDC_MEM_WRITE, L"address value [value...]");
    dbgwin->srch_label = dim_label(dbgwin, "Search", SS_LEFT);
    dbgwin->srch_kind = combo(dbgwin, IDC_SRCH_KIND);
    SendMessageA(dbgwin->srch_kind, CB_ADDSTRING, 0, (LPARAM)"Hex bytes");
    SendMessageA(dbgwin->srch_kind, CB_ADDSTRING, 0, (LPARAM)"Text");
    SendMessageA(dbgwin->srch_kind, CB_SETCURSEL, 0, 0);
    dbgwin->srch_entry = field(dbgwin, IDC_SRCH_ENTRY, L"3E 00 / ABC");
    dbgwin->srch_next = button(dbgwin, "Find Next", IDC_SRCH_NEXT);
    dbgwin->srch_prev = button(dbgwin, "Find Previous", IDC_SRCH_PREV);
    dbgwin->srch_note = dim_label(dbgwin, "", SS_LEFT | SS_ENDELLIPSIS);
    dbgwin->mem_view = view(dbgwin, IDC_MEM_VIEW, WS_HSCROLL | ES_NOHIDESEL);

    /* Breakpoints */
    dbgwin->new_parent = dbgwin->pages[TAB_BREAKS];
    dbgwin->bp_hint = dim_label(dbgwin, "Click a disassembly line's gutter to set an execute breakpoint; the box enables it.",
                           SS_LEFT | SS_ENDELLIPSIS);
    dbgwin->bp_remove = button(dbgwin, "Remove", IDC_BP_REMOVE);
    dbgwin->bp_hits = button(dbgwin, "Clear Hits", IDC_BP_HITS);
    dbgwin->bp_clear = button(dbgwin, "Clear All", IDC_BP_CLEAR);
    dbgwin->bp_list = child_ex(dbgwin, WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
                          WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | LVS_NOSORTHEADER,
                          IDC_BP_LIST, dbgwin->mono);
    SendMessageA(dbgwin->bp_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    {
        static const char *const cols[5] = { "#", "Type", "Range", "Condition", "Hits" };
        LVCOLUMNA col;
        memset(&col, 0, sizeof col);
        col.mask = LVCF_TEXT;
        for (idx = 0; idx < 5; idx++) {
            col.pszText = (char *)cols[idx];
            SendMessageA(dbgwin->bp_list, LVM_INSERTCOLUMNA, (WPARAM)idx, (LPARAM)&col);
        }
        size_columns(dbgwin);
    }
    dbgwin->bp_empty = dim_label(dbgwin, "No breakpoints. Click the gutter of a disassembly line, or add one below.", SS_CENTER);
    dbgwin->bp_type = combo(dbgwin, IDC_BP_TYPE);
    for (idx = 0; idx < NBPTYPES; idx++) SendMessageA(dbgwin->bp_type, CB_ADDSTRING, 0, (LPARAM)k_bp_types[idx]);
    SendMessageA(dbgwin->bp_type, CB_SETCURSEL, 0, 0);
    dbgwin->bp_start = field(dbgwin, IDC_BP_START, NULL);
    dbgwin->bp_end = field(dbgwin, IDC_BP_END, NULL);
    dbgwin->bp_cond = field(dbgwin, IDC_BP_COND, L"condition, e.g. value != old  (value, old, addr, a, hl, [addr], or Tcl)");
    dbgwin->bp_add = button(dbgwin, "Add", IDC_BP_ADD);
    dbgwin->bp_msg = label(dbgwin, "", dbgwin->ui_font, SS_LEFT | SS_ENDELLIPSIS);
    dbgwin->bp_stop = dim_label(dbgwin, "", SS_LEFT | SS_ENDELLIPSIS);
    on_bp_type(dbgwin);

    /* Watches */
    dbgwin->new_parent = dbgwin->pages[TAB_WATCHES];
    dbgwin->wt_label = dim_label(dbgwin, "Watch", SS_LEFT);
    dbgwin->wt_entry = field(dbgwin, IDC_WT_ENTRY, L"an address, label, expression or range (score, c000-c0ff, [hl])");
    dbgwin->wt_add = button(dbgwin, "Add", IDC_WT_ADD);
    dbgwin->wt_list = list_box(dbgwin, IDC_WT_LIST);
    dbgwin->wt_mem = button(dbgwin, "Show in Memory", IDC_WT_MEM);
    dbgwin->wt_brk = button(dbgwin, "Break on Change", IDC_WT_BRK);
    dbgwin->wt_remove = button(dbgwin, "Remove", IDC_WT_REMOVE);

    /* Cart & I/O */
    dbgwin->new_parent = dbgwin->pages[TAB_CART];
    dbgwin->cart_text = view(dbgwin, IDC_CART_TEXT, WS_HSCROLL);
    dbgwin->io_text = view(dbgwin, IDC_IO_TEXT, WS_HSCROLL);

    /* Trace (0, live) and History (1, when stopped) share one ring */
    for (idx = 0; idx < 2; idx++) {
        dbgwin->new_parent = dbgwin->pages[TAB_TRACE + idx];
        dbgwin->h_rec[idx] = child(dbgwin, "BUTTON", "Record", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, IDC_H_REC0 + idx, dbgwin->ui_font);
        dbgwin->h_filter[idx] = combo(dbgwin, IDC_H_FILTER0 + idx);
        SendMessageA(dbgwin->h_filter[idx], CB_ADDSTRING, 0, (LPARAM)"All instructions");
        SendMessageA(dbgwin->h_filter[idx], CB_ADDSTRING, 0, (LPARAM)"Branches only");
        SendMessageA(dbgwin->h_filter[idx], CB_ADDSTRING, 0, (LPARAM)"In range");
        SendMessageA(dbgwin->h_filter[idx], CB_SETCURSEL, 0, 0);
        dbgwin->h_range[idx] = field(dbgwin, IDC_H_RANGE0 + idx, L"range, e.g. 4000-40ff");
        dbgwin->h_clear[idx] = button(dbgwin, "Clear", IDC_H_CLEAR0 + idx);
        dbgwin->h_note[idx] = dim_label(dbgwin, "", SS_RIGHT | SS_ENDELLIPSIS);
        dbgwin->h_text[idx] = view(dbgwin, IDC_H_TEXT0 + idx, WS_HSCROLL);
        dbgwin->h_seen[idx] = (uint64_t)-1;
    }

    /* Video: the shared controller's tab, one child holding all its controls */
    dbgwin->new_parent = NULL;
    dbgwin->video = dbg_video_create(dbgwin->pages[TAB_VIDEO], dbgwin->dbg, dbgwin->ui_font, dbgwin->mono, dbgwin->dpi);

    /* Sound */
    dbgwin->new_parent = dbgwin->pages[TAB_SOUND];
    dbgwin->sound_text = view(dbgwin, IDC_SOUND_TEXT, WS_HSCROLL);

    /* Symbols */
    dbgwin->new_parent = dbgwin->pages[TAB_SYMBOLS];
    dbgwin->sym_filter_label = dim_label(dbgwin, "Filter", SS_LEFT);
    dbgwin->sym_filter = field(dbgwin, IDC_SYM_FILTER, L"part of a name");
    dbgwin->sym_count = dim_label(dbgwin, "", SS_LEFT);
    dbgwin->sym_list = view(dbgwin, IDC_SYM_LIST, WS_HSCROLL);
    g_sym_proc = (WNDPROC)SetWindowLongPtrA(dbgwin->sym_list, GWLP_WNDPROC, (LONG_PTR)sym_list_proc);

    /* FujiNet: the note and the log are UTF-8 (see wide_view_text) */
    dbgwin->new_parent = dbgwin->pages[TAB_FUJINET];
    dbgwin->fuji_title = label(dbgwin, "FujiBus packets (SLIP-decoded)", dbgwin->ui_bold, SS_LEFT);
    dbgwin->fuji_note = dim_label(dbgwin, "", SS_LEFT | SS_ENDELLIPSIS);
    dbgwin->fuji_clear = button(dbgwin, "Clear", IDC_FUJI_CLEAR);
    dbgwin->fuji_text = view(dbgwin, IDC_FUJI_TEXT, WS_HSCROLL);
    dbgwin->fuji_state_title = label(dbgwin, "FujiNet cartridge", dbgwin->ui_bold, SS_LEFT);
    dbgwin->fuji_state = view(dbgwin, IDC_FUJI_STATE, WS_HSCROLL);

    /* The tab control was made first, so it sits below the pages' containers
     * already; with WS_CLIPSIBLINGS and the bottom of the z-order a repaint of
     * it never paints its body over a page. */
    dbgwin->new_parent = NULL;
    SetWindowPos(dbgwin->tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/* Help's contexts (msxhelp.h), one per tab */
static const char *const k_tab_contexts[TAB_COUNT] = {
    "debugger/console", "debugger/disassembly", "debugger/memory", "debugger/breakpoints", "debugger/watches",
    "debugger/cart", "debugger/trace", "debugger/history", "debugger/video", "debugger/sound",
    "debugger/symbols", "debugger/fujinet",
};

/* On screen: the tab the tab control shows, or one living in a window of its own. */
static int page_visible(debugger *dbgwin, int page)
{
    return dbgwin->popped[page] != NULL || dbgwin->page == page;
}

static void show_page(debugger *dbgwin, int page)
{
    int ptr;
    dbgwin->page = page;
    /* Help, F1 and Ctrl+Shift+H open the tab's section */
    if (page >= 0 && page < TAB_COUNT) msx_help_set_context(dbgwin->hwnd, k_tab_contexts[page], 1);
    for (ptr = 0; ptr < TAB_COUNT; ptr++)     /* a tab's controls follow its container */
        ShowWindow(dbgwin->pages[ptr], dbgwin->popped[ptr] || ptr == page ? SW_SHOW : SW_HIDE);
    show_bp_list(dbgwin);
}

/* The tab control's index for a tab, or -1 when it is in a window of its own. */
static int tab_index(debugger *dbgwin, int page)
{
    int idx;
    for (idx = 0; idx < dbgwin->ntabs; idx++)
        if (dbgwin->tab_page[idx] == page) return idx;
    return -1;
}

static void place(HWND hwnd, int pos_x, int pos_y, int wtext, int hgt);
static void layout(debugger *dbgwin);

/* Brings a tab to the front: its window if it has one, else the tab control's. */
static void goto_page(debugger *dbgwin, int page)
{
    const int idx = tab_index(dbgwin, page);
    if (dbgwin->popped[page]) {
        SetForegroundWindow(dbgwin->popped[page]);
        return;
    }
    if (idx >= 0) {
        SendMessageA(dbgwin->tabs, TCM_SETCURSEL, (WPARAM)idx, 0);
        show_page(dbgwin, page);
        layout(dbgwin);
        refresh_page(dbgwin, page, 1);
    }
}

/* ---- peeling a tab off into a window of its own, and putting it back ----------- */

static void dock_page(debugger *dbgwin, int page)
{
    TCITEMA item;
    char shown[40];
    HWND pop = dbgwin->popped[page];
    int pos = 0, ptr, idx;

    if (!pop) return;
    dbgwin->popped[page] = NULL;
    SetParent(dbgwin->pages[page], dbgwin->hwnd);
    for (ptr = 0; ptr < page; ptr++)          /* back where it was in the order */
        if (!dbgwin->popped[ptr]) pos++;
    for (idx = dbgwin->ntabs; idx > pos; idx--) dbgwin->tab_page[idx] = dbgwin->tab_page[idx - 1];
    dbgwin->tab_page[pos] = page;
    dbgwin->ntabs++;
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    item.pszText = (char *)tab_text(page, shown, sizeof shown);
    SendMessageA(dbgwin->tabs, TCM_INSERTITEMA, (WPARAM)pos, (LPARAM)&item);
    DestroyWindow(pop);
    SendMessageA(dbgwin->tabs, TCM_SETCURSEL, (WPARAM)pos, 0);
    show_page(dbgwin, page);
    layout(dbgwin);
    refresh_page(dbgwin, page, 1);
}

static void pop_out(debugger *dbgwin, int page)
{
    HWND pop;
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(dbgwin->hwnd, GWLP_HINSTANCE);
    char title[96];
    int tab_pos, idx, next;

    if (page < 0 || page >= TAB_COUNT || dbgwin->popped[page] || dbgwin->ntabs <= 1) return;   /* one tab always stays */
    tab_pos = tab_index(dbgwin, page);
    if (tab_pos < 0) return;
    SendMessageA(dbgwin->tabs, TCM_DELETEITEM, (WPARAM)tab_pos, 0);
    for (idx = tab_pos; idx < dbgwin->ntabs - 1; idx++) dbgwin->tab_page[idx] = dbgwin->tab_page[idx + 1];
    dbgwin->ntabs--;

    snprintf(title, sizeof title, "MSX Debugger - %s", k_tabs[page].label);
    pop = CreateWindowExA(0, POP_CLASS, title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                          scaled(dbgwin, 900), scaled(dbgwin, 620), dbgwin->hwnd, NULL, inst, NULL);
    SetWindowLongPtrA(pop, GWLP_USERDATA, page);
    msx_help_set_context(pop, k_tab_contexts[page], 1);       /* F1 in the window opens its tab's section */
    {
        HWND dock = CreateWindowExA(0, "BUTTON", "Dock", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, scaled(dbgwin, 4),
                                    scaled(dbgwin, 4), scaled(dbgwin, 70), scaled(dbgwin, 26), pop, (HMENU)(INT_PTR)IDC_DOCK, inst, NULL);
        SendMessageA(dock, WM_SETFONT, (WPARAM)dbgwin->ui_font, TRUE);
    }
    SetParent(dbgwin->pages[page], pop);
    dbgwin->popped[page] = pop;
    if (page == dbgwin->page) {              /* the tab in front left: the one after it comes forward */
        next = tab_pos < dbgwin->ntabs ? tab_pos : dbgwin->ntabs - 1;
        SendMessageA(dbgwin->tabs, TCM_SETCURSEL, (WPARAM)next, 0);
        show_page(dbgwin, dbgwin->tab_page[next]);
    } else {                            /* another tab left: the one in front stays */
        SendMessageA(dbgwin->tabs, TCM_SETCURSEL, (WPARAM)tab_index(dbgwin, dbgwin->page), 0);
        show_page(dbgwin, dbgwin->page);
    }
    layout(dbgwin);
    ShowWindow(pop, SW_SHOW);
    refresh_page(dbgwin, page, 1);
    refresh_page(dbgwin, dbgwin->page, 1);
}

/* A tab's container: its controls' notifications go to the debugger window,
 * whichever window the container is in. */
static LRESULT CALLBACK page_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
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
        if (main) return SendMessageA(main, msg, wparam, lparam);
        break;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

/* The window a peeled-off tab lives in. */
static LRESULT CALLBACK pop_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    const int page = (int)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_SIZE:
        if (g_dbg) layout(g_dbg);
        return 0;
    case WM_COMMAND:
        if (g_dbg && LOWORD(wparam) == IDC_DOCK) dock_page(g_dbg, page);
        return 0;
    case WM_CLOSE:
        if (g_dbg) dock_page(g_dbg, page);
        else DestroyWindow(hwnd);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lparam)->ptMinTrackSize.x = MulDiv(420, g_dbg && g_dbg->dpi ? g_dbg->dpi : 96, 96);
        ((MINMAXINFO *)lparam)->ptMinTrackSize.y = MulDiv(300, g_dbg && g_dbg->dpi ? g_dbg->dpi : 96, 96);
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

/* ---- layout ----------------------------------------------------------------------- */

static void place(HWND hwnd, int pos_x, int pos_y, int wtext, int hgt)
{
    MoveWindow(hwnd, pos_x, pos_y, wtext > 1 ? wtext : 1, hgt > 1 ? hgt : 1, TRUE);
}

/* The controls of each tab inside its container. */
static void layout_pages(debugger *dbgwin)
{
    const int button_h = scaled(dbgwin, 26), edit_h = scaled(dbgwin, 24), label_h = scaled(dbgwin, 18), gap = scaled(dbgwin, 6);
    RECT row;
    int ptr, wtext, hwnd, pos_x, pos_y, idx, button_w;

    for (ptr = 0; ptr < TAB_COUNT; ptr++) {
        if (!dbgwin->pages[ptr]) continue;
        GetClientRect(dbgwin->pages[ptr], &row);
        wtext = row.right;
        hwnd = row.bottom;
        if (wtext < 40 || hwnd < 40) continue;
        switch (ptr) {
        case TAB_CONSOLE:
            button_w = dbgwin->mono_w + scaled(dbgwin, 6);
            place(dbgwin->con_out, 0, 0, wtext, hwnd - edit_h - scaled(dbgwin, 8));
            place(dbgwin->con_prompt, 0, hwnd - edit_h + (edit_h - dbgwin->mono_h) / 2, button_w, dbgwin->mono_h);
            place(dbgwin->con_in, button_w + scaled(dbgwin, 2), hwnd - edit_h, wtext - button_w - scaled(dbgwin, 2), edit_h);
            break;
        case TAB_DISASM:
            button_w = text_w(dbgwin, dbgwin->ui_font, "Follow PC") + scaled(dbgwin, 24);
            place(dbgwin->follow_pc, 0, scaled(dbgwin, 1), button_w, scaled(dbgwin, 22));
            pos_x = button_w + scaled(dbgwin, 12);
            button_w = text_w(dbgwin, dbgwin->ui_font, "Jump to") + scaled(dbgwin, 6);
            place(dbgwin->jump_label, pos_x, (edit_h - label_h) / 2, button_w, label_h);
            pos_x += button_w;
            place(dbgwin->jump, pos_x, 0, scaled(dbgwin, 150), edit_h);
            pos_x += scaled(dbgwin, 162);
            place(dbgwin->disasm_hint, pos_x, (edit_h - label_h) / 2, wtext - pos_x, label_h);
            place(dbgwin->disasm, 0, edit_h + scaled(dbgwin, 6), wtext, hwnd - edit_h - scaled(dbgwin, 6));
            break;
        case TAB_MEMORY:
            pos_x = 0;
            button_w = text_w(dbgwin, dbgwin->ui_font, "Go to") + scaled(dbgwin, 6);
            place(dbgwin->goto_label, pos_x, (edit_h - label_h) / 2, button_w, label_h);
            pos_x += button_w;
            place(dbgwin->mem_goto, pos_x, 0, scaled(dbgwin, 190), edit_h);
            pos_x += scaled(dbgwin, 196);
            button_w = btn_w(dbgwin, "Back 4K");
            place(dbgwin->mem_back, pos_x, -scaled(dbgwin, 1), button_w, button_h);
            pos_x += button_w + scaled(dbgwin, 4);
            button_w = btn_w(dbgwin, "On 4K");
            place(dbgwin->mem_on, pos_x, -scaled(dbgwin, 1), button_w, button_h);
            pos_x += button_w + scaled(dbgwin, 24);
            button_w = text_w(dbgwin, dbgwin->ui_font, "Poke") + scaled(dbgwin, 6);
            place(dbgwin->write_label, pos_x, (edit_h - label_h) / 2, button_w, label_h);
            pos_x += button_w;
            place(dbgwin->mem_write, pos_x, 0, wtext - pos_x > scaled(dbgwin, 120) ? (wtext - pos_x < scaled(dbgwin, 300) ? wtext - pos_x : scaled(dbgwin, 300)) : scaled(dbgwin, 120), edit_h);
            pos_y = button_h + gap;
            pos_x = 0;
            button_w = text_w(dbgwin, dbgwin->ui_font, "Search") + scaled(dbgwin, 6);
            place(dbgwin->srch_label, pos_x, pos_y + (edit_h - label_h) / 2, button_w, label_h);
            pos_x += button_w;
            place(dbgwin->srch_kind, pos_x, pos_y, scaled(dbgwin, 110), scaled(dbgwin, 200));
            pos_x += scaled(dbgwin, 116);
            place(dbgwin->srch_entry, pos_x, pos_y, scaled(dbgwin, 230), edit_h);
            pos_x += scaled(dbgwin, 236);
            button_w = btn_w(dbgwin, "Find Next");
            place(dbgwin->srch_next, pos_x, pos_y - scaled(dbgwin, 1), button_w, button_h);
            pos_x += button_w + scaled(dbgwin, 4);
            button_w = btn_w(dbgwin, "Find Previous");
            place(dbgwin->srch_prev, pos_x, pos_y - scaled(dbgwin, 1), button_w, button_h);
            pos_x += button_w + scaled(dbgwin, 12);
            place(dbgwin->srch_note, pos_x, pos_y + (edit_h - label_h) / 2, wtext - pos_x, label_h);
            pos_y += button_h + gap;
            place(dbgwin->mem_view, 0, pos_y, wtext, hwnd - pos_y);
            break;
        case TAB_BREAKS: {
            const int stop_y = hwnd - label_h, msg_y = stop_y - label_h - scaled(dbgwin, 2), add_y = msg_y - scaled(dbgwin, 8) - edit_h;
            const int list_y = button_h + gap;
            int clear_w = btn_w(dbgwin, "Clear All"), help_w = btn_w(dbgwin, "Clear Hits"), right_w = btn_w(dbgwin, "Remove");
            place(dbgwin->bp_clear, wtext - clear_w, 0, clear_w, button_h);
            place(dbgwin->bp_hits, wtext - clear_w - gap - help_w, 0, help_w, button_h);
            place(dbgwin->bp_remove, wtext - clear_w - help_w - 2 * gap - right_w, 0, right_w, button_h);
            place(dbgwin->bp_hint, 0, (button_h - label_h) / 2, wtext - clear_w - help_w - right_w - 4 * gap, label_h);
            place(dbgwin->bp_list, 0, list_y, wtext, add_y - gap - list_y);
            place(dbgwin->bp_empty, 0, list_y + scaled(dbgwin, 12), wtext, label_h);
            pos_x = 0;
            place(dbgwin->bp_type, pos_x, add_y, scaled(dbgwin, 110), scaled(dbgwin, 240));
            pos_x += scaled(dbgwin, 116);
            place(dbgwin->bp_start, pos_x, add_y, scaled(dbgwin, 140), edit_h);
            pos_x += scaled(dbgwin, 146);
            place(dbgwin->bp_end, pos_x, add_y, scaled(dbgwin, 110), edit_h);
            pos_x += scaled(dbgwin, 116);
            button_w = btn_w(dbgwin, "Add");
            place(dbgwin->bp_cond, pos_x, add_y, wtext - pos_x - button_w - gap, edit_h);
            place(dbgwin->bp_add, wtext - button_w, add_y - scaled(dbgwin, 1), button_w, button_h);
            place(dbgwin->bp_msg, 0, msg_y, wtext, label_h);
            place(dbgwin->bp_stop, 0, stop_y, wtext, label_h);
            break;
        }
        case TAB_WATCHES: {
            const int bottom = hwnd - button_h;
            button_w = text_w(dbgwin, dbgwin->ui_font, "Watch") + scaled(dbgwin, 6);
            place(dbgwin->wt_label, 0, (edit_h - label_h) / 2, button_w, label_h);
            idx = btn_w(dbgwin, "Add");
            place(dbgwin->wt_entry, button_w, 0, wtext - button_w - idx - gap, edit_h);
            place(dbgwin->wt_add, wtext - idx, -scaled(dbgwin, 1), idx, button_h);
            place(dbgwin->wt_list, 0, button_h + gap, wtext, bottom - button_h - 2 * gap);
            pos_x = 0;
            button_w = btn_w(dbgwin, "Show in Memory");
            place(dbgwin->wt_mem, pos_x, bottom, button_w, button_h);
            pos_x += button_w + gap;
            button_w = btn_w(dbgwin, "Break on Change");
            place(dbgwin->wt_brk, pos_x, bottom, button_w, button_h);
            pos_x += button_w + gap;
            button_w = btn_w(dbgwin, "Remove");
            place(dbgwin->wt_remove, pos_x, bottom, button_w, button_h);
            break;
        }
        case TAB_CART: {
            const int left = (wtext - gap) * 2 / 5;
            place(dbgwin->cart_text, 0, 0, left, hwnd);
            place(dbgwin->io_text, left + gap, 0, wtext - left - gap, hwnd);
            break;
        }
        case TAB_TRACE:
        case TAB_HISTORY:
            idx = ptr - TAB_TRACE;
            pos_x = 0;
            button_w = btn_w(dbgwin, "Record");
            place(dbgwin->h_rec[idx], pos_x, 0, button_w, button_h);
            pos_x += button_w + gap;
            place(dbgwin->h_filter[idx], pos_x, 1, scaled(dbgwin, 150), scaled(dbgwin, 200));
            pos_x += scaled(dbgwin, 156);
            place(dbgwin->h_range[idx], pos_x, 1, scaled(dbgwin, 190), edit_h);
            pos_x += scaled(dbgwin, 196);
            button_w = btn_w(dbgwin, "Clear");
            place(dbgwin->h_clear[idx], pos_x, 0, button_w, button_h);
            pos_x += button_w + gap;
            place(dbgwin->h_note[idx], pos_x, (button_h - label_h) / 2, wtext - pos_x, label_h);
            place(dbgwin->h_text[idx], 0, button_h + gap, wtext, hwnd - button_h - gap);
            break;
        case TAB_VIDEO:
            if (dbgwin->video) dbg_video_place(dbgwin->video, 0, 0, wtext, hwnd);
            break;
        case TAB_SOUND:
            place(dbgwin->sound_text, 0, 0, wtext, hwnd);
            break;
        case TAB_SYMBOLS:
            button_w = text_w(dbgwin, dbgwin->ui_font, "Filter") + scaled(dbgwin, 6);
            place(dbgwin->sym_filter_label, 0, (edit_h - label_h) / 2, button_w, label_h);
            place(dbgwin->sym_filter, button_w, 0, scaled(dbgwin, 240), edit_h);
            place(dbgwin->sym_count, button_w + scaled(dbgwin, 250), (edit_h - label_h) / 2, scaled(dbgwin, 240), label_h);
            place(dbgwin->sym_list, 0, edit_h + gap, wtext, hwnd - edit_h - gap);
            break;
        case TAB_FUJINET: {
            /* the packets left (a note and Clear over them), the cartridge's link and counts right */
            const int right_w = wtext / 3 > scaled(dbgwin, 340) ? scaled(dbgwin, 340) : wtext / 3 > scaled(dbgwin, 220) ? wtext / 3 : scaled(dbgwin, 220);
            const int left_w = wtext - right_w - scaled(dbgwin, 12);
            const int clear_w = btn_w(dbgwin, "Clear");
            place(dbgwin->fuji_title, 0, 0, left_w, label_h);
            place(dbgwin->fuji_clear, left_w - clear_w, label_h + scaled(dbgwin, 4), clear_w, button_h);
            place(dbgwin->fuji_note, 0, label_h + scaled(dbgwin, 4) + (button_h - label_h) / 2, left_w - clear_w - gap, label_h);
            place(dbgwin->fuji_text, 0, label_h + button_h + scaled(dbgwin, 10), left_w, hwnd - label_h - button_h - scaled(dbgwin, 10));
            place(dbgwin->fuji_state_title, left_w + scaled(dbgwin, 12), 0, right_w, label_h);
            place(dbgwin->fuji_state, left_w + scaled(dbgwin, 12), label_h + scaled(dbgwin, 4), right_w, hwnd - label_h - scaled(dbgwin, 4));
            break;
        }
        default: break;
        }
    }
}

/* The register strip's height: three rows. */
static int strip_height(const debugger *dbgwin)
{
    return 3 * (scaled(dbgwin, 24) + scaled(dbgwin, 5)) + scaled(dbgwin, 6);
}

static void layout(debugger *dbgwin)
{
    RECT client, page;
    int pos_x, pos_y, wtext, idx, page_x, page_y, page_w, page_h, strip_y, tab_y;
    const int button_h = scaled(dbgwin, 26), edit_h = scaled(dbgwin, 24), label_h = scaled(dbgwin, 18), gap = scaled(dbgwin, 6);

    if (!dbgwin->tabs) return;
    GetClientRect(dbgwin->hwnd, &client);

    /* The toolbar: stepping, then symbols, save and pop out, the status after
     * them. Too narrow for the status beside them, it goes on a line of its own. */
    {
        HWND bar[9] = { dbgwin->run_btn, dbgwin->step_btn, dbgwin->over_btn, dbgwin->out_btn, dbgwin->run_to_btn, dbgwin->frame_btn,
                        dbgwin->sym_btn, dbgwin->save_btn, dbgwin->popout_btn };
        const char *btext[9] = { "Stop (F5)", "Step (F7)", "Step Over (F8)", "Step Out (Shift+F8)",
                                 "Run to Cursor", "Frame (Shift+F7)", "Symbols...", "Save...", "Pop Out" };
        const int help_w = btn_w(dbgwin, "Help");
        const int sright = client.right - help_w - gap;
        pos_x = scaled(dbgwin, 8);
        for (idx = 0; idx < 9; idx++) {
            wtext = btn_w(dbgwin, btext[idx]);
            place(bar[idx], pos_x, scaled(dbgwin, 8), wtext, button_h);
            pos_x += wtext + gap + (idx == 5 ? scaled(dbgwin, 14) : 0);
        }
        pos_y = scaled(dbgwin, 8) + button_h + scaled(dbgwin, 8);
        place(dbgwin->help_btn, client.right - scaled(dbgwin, 8) - help_w, scaled(dbgwin, 8), help_w, button_h);
        if (sright - scaled(dbgwin, 8) - pos_x >= scaled(dbgwin, 220)) {
            place(dbgwin->status, pos_x, scaled(dbgwin, 8) + (button_h - label_h) / 2, sright - scaled(dbgwin, 8) - pos_x, label_h);
        } else {
            place(dbgwin->status, scaled(dbgwin, 8), pos_y - scaled(dbgwin, 2), client.right - scaled(dbgwin, 16), label_h);
            pos_y += label_h + scaled(dbgwin, 4);
        }
        tab_y = pos_y;
    }

    /* The register strip, under every tab: AF BC DE HL IX IY SP PC / the
     * shadow set, I, R, IM and HALT / the flags and flip-flops and a hint. */
    strip_y = client.bottom - strip_height(dbgwin);
    {
        const int right_w = dbgwin->mono_w * 6 + scaled(dbgwin, 8), cell = (client.right - scaled(dbgwin, 16) + scaled(dbgwin, 8)) / 8;
        const int left_w = text_w(dbgwin, dbgwin->ui_font, "WWW") + scaled(dbgwin, 2);
        const int row_h = edit_h + scaled(dbgwin, 5);
        page_x = scaled(dbgwin, 12);
        for (idx = 0; idx < NREGS; idx++) {
            const int row = idx / 8, col = idx % 8;
            const int row_top = strip_y + scaled(dbgwin, 3) + row * row_h;
            place(dbgwin->reg_label[idx], page_x + col * cell, row_top + (edit_h - label_h) / 2, left_w, label_h);
            place(dbgwin->reg[idx], page_x + col * cell + left_w + scaled(dbgwin, 4), row_top, right_w, edit_h);
        }
        place(dbgwin->halt_label, page_x + 7 * cell + left_w + scaled(dbgwin, 4), strip_y + scaled(dbgwin, 3) + row_h + (edit_h - label_h) / 2, right_w, label_h);
        pos_x = page_x;
        pos_y = strip_y + scaled(dbgwin, 3) + 2 * row_h;
        for (idx = 0; idx < NCHECKS; idx++) {
            wtext = text_w(dbgwin, dbgwin->ui_font, k_chk_names[idx]) + scaled(dbgwin, 28);
            place(dbgwin->chk[idx], pos_x, pos_y + scaled(dbgwin, 1), wtext, scaled(dbgwin, 22));
            pos_x += wtext + scaled(dbgwin, 6);
        }
        place(dbgwin->cpu_hint, pos_x + scaled(dbgwin, 16), pos_y + (edit_h - label_h) / 2, client.right - pos_x - scaled(dbgwin, 16) - page_x, label_h);
    }

    /* The tab control is the strip alone: the pages sit on the window's own
     * background, which their captions are drawn on. */
    SetRect(&page, 0, 0, scaled(dbgwin, 400), scaled(dbgwin, 400));
    SendMessageA(dbgwin->tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    place(dbgwin->tabs, scaled(dbgwin, 8), tab_y, client.right - scaled(dbgwin, 16), page.top);
    page_x = scaled(dbgwin, 12);
    page_y = tab_y + page.top + scaled(dbgwin, 6);
    page_w = client.right - 2 * page_x;
    page_h = strip_y - scaled(dbgwin, 6) - page_y;
    if (page_w < scaled(dbgwin, 300)) page_w = scaled(dbgwin, 300);
    if (page_h < scaled(dbgwin, 160)) page_h = scaled(dbgwin, 160);

    /* each tab's container: in the tab control's page, or filling the window it was peeled off into */
    for (idx = 0; idx < TAB_COUNT; idx++) {
        if (dbgwin->popped[idx]) {
            RECT pop_rect;
            GetClientRect(dbgwin->popped[idx], &pop_rect);
            MoveWindow(dbgwin->pages[idx], scaled(dbgwin, 6), scaled(dbgwin, 34), pop_rect.right > scaled(dbgwin, 12) ? pop_rect.right - scaled(dbgwin, 12) : 1,
                       pop_rect.bottom > scaled(dbgwin, 40) ? pop_rect.bottom - scaled(dbgwin, 40) : 1, TRUE);
        } else {
            MoveWindow(dbgwin->pages[idx], page_x, page_y, page_w, page_h, TRUE);
        }
    }
    layout_pages(dbgwin);

    /* everything again: what the controls left behind where they were */
    RedrawWindow(dbgwin->hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* ---- the window ------------------------------------------------------------------- */

static void hide_window(debugger *dbgwin)
{
    KillTimer(dbgwin->hwnd, TIMER_REFRESH);
    msxdebug_trace_enable(dbgwin->dbg, 0);        /* the ring costs the CPU its slow path: only while a window wants it */
    msxdebug_history_enable(dbgwin->dbg, 0);
    msxdebug_detach(dbgwin->dbg);     /* lets the machine run */
    ShowWindow(dbgwin->hwnd, SW_HIDE);
}

static void attach_and_refresh(debugger *dbgwin)
{
    msxdebug_attach(dbgwin->dbg);     /* stops the machine */
    dbgwin->was_stopped = 0;
    dbgwin->seen_gen = msxdebug_generation(dbgwin->dbg);
    SetTimer(dbgwin->hwnd, TIMER_REFRESH, 100, NULL);
    settle_and_sync(dbgwin, 1);
}

static void draw_run_button(debugger *dbgwin, const DRAWITEMSTRUCT *di)
{
    char text[32];
    const int stopped = msxdebug_is_stopped(dbgwin->dbg);
    RECT row = di->rcItem;
    if (stopped) {
        FillRect(di->hDC, &row, dbgwin->accent);
        FrameRect(di->hDC, &row, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(di->hDC, INK_RGB);
    } else {
        DrawFrameControl(di->hDC, &row, DFC_BUTTON, DFCS_BUTTONPUSH | ((di->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0));
        SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
    }
    SetBkMode(di->hDC, TRANSPARENT);
    GetWindowTextA(di->hwndItem, text, sizeof text);
    SelectObject(di->hDC, dbgwin->ui_font);
    DrawTextA(di->hDC, text, -1, &row, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (di->itemState & ODS_FOCUS) {
        InflateRect(&row, -3, -3);
        DrawFocusRect(di->hDC, &row);
    }
}


static void select_tab(debugger *dbgwin, int page)
{
    goto_page(dbgwin, page);
}

static LRESULT on_notify(debugger *dbgwin, NMHDR *nmhdr)
{
    if (nmhdr->hwndFrom == dbgwin->tabs && nmhdr->code == (UINT)TCN_SELCHANGE) {
        const int idx = (int)SendMessageA(dbgwin->tabs, TCM_GETCURSEL, 0, 0);
        if (idx >= 0 && idx < dbgwin->ntabs) {
            show_page(dbgwin, dbgwin->tab_page[idx]);
            refresh_page(dbgwin, dbgwin->tab_page[idx], 1);
        }
        return 0;
    }
    if (nmhdr->code == (UINT)NM_CUSTOMDRAW && (nmhdr->hwndFrom == dbgwin->h_rec[0] || nmhdr->hwndFrom == dbgwin->h_rec[1]))
        return msx_accent_customdraw((const NMCUSTOMDRAW *)nmhdr, SendMessageA(nmhdr->hwndFrom, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (nmhdr->hwndFrom != dbgwin->bp_list) return 0;
    if (nmhdr->code == (UINT)LVN_ITEMCHANGED) {
        const NMLISTVIEW *notify = (const NMLISTVIEW *)nmhdr;
        /* a row's box: its breakpoint on or off (the list follows after) */
        if (!dbgwin->updating && notify->iItem >= 0 && notify->iItem < dbgwin->nbps && (notify->uChanged & LVIF_STATE) &&
            ((notify->uNewState ^ notify->uOldState) & LVIS_STATEIMAGEMASK) && (notify->uOldState & LVIS_STATEIMAGEMASK)) {
            const int on = ((notify->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
            if (on != (dbgwin->bps[notify->iItem].enabled != 0)) {
                msxdebug_breakpoint_enable(dbgwin->dbg, dbgwin->bps[notify->iItem].id, on);
                PostMessageA(dbgwin->hwnd, WM_DBG_SYNC, 0, 0);
            }
        }
    } else if (nmhdr->code == (UINT)LVN_KEYDOWN) {
        /* after the list's own handling: removing rebuilds it */
        if (((const NMLVKEYDOWN *)nmhdr)->wVKey == VK_DELETE)
            PostMessageA(dbgwin->hwnd, WM_COMMAND, MAKEWPARAM(IDC_BP_REMOVE, BN_CLICKED), 0);
    } else if (nmhdr->code == (UINT)NM_DBLCLK) {
        /* an address breakpoint's row opens the disassembly there */
        const int idx = bp_selected(dbgwin);
        if (idx >= 0 && !(dbgwin->bps[idx].type & (MSXDEBUG_BP_IN | MSXDEBUG_BP_OUT))) {
            char start_text[16];
            snprintf(start_text, sizeof start_text, "$%04X", dbgwin->bps[idx].start);
            select_tab(dbgwin, TAB_DISASM);
            SetWindowTextA(dbgwin->jump, start_text);
            on_jump(dbgwin);
        }
    }
    return 0;
}

static LRESULT CALLBACK dbg_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    debugger *dbgwin = g_dbg;
    if (msg == WM_GETMINMAXINFO) {
        const int dpi = dbgwin && dbgwin->dpi ? dbgwin->dpi : 96;
        ((MINMAXINFO *)lparam)->ptMinTrackSize.x = MulDiv(980, dpi, 96);
        ((MINMAXINFO *)lparam)->ptMinTrackSize.y = MulDiv(600, dpi, 96);
        return 0;
    }
    if (!dbgwin || dbgwin->hwnd != hwnd) return DefWindowProcA(hwnd, msg, wparam, lparam);

    switch (msg) {
    case WM_SIZE:
        layout(dbgwin);
        return 0;
    case WM_DPICHANGED: {
        const RECT *row = (const RECT *)lparam;
        dbgwin->dpi = HIWORD(wparam);
        make_fonts(dbgwin);
        size_columns(dbgwin);
        if (dbgwin->video) dbg_video_fonts(dbgwin->video, dbgwin->ui_font, dbgwin->mono, dbgwin->dpi);
        SetWindowPos(hwnd, NULL, row->left, row->top, row->right - row->left, row->bottom - row->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout(dbgwin);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wparam == TIMER_REFRESH) tick(dbgwin);
        return 0;
    case WM_DBG_ACCEPT:
        on_accept(dbgwin, (int)wparam);
        return 0;
    case WM_DBG_TAB:
        complete_console(dbgwin);
        return 0;
    case WM_DBG_HISTORY:
        history_step(dbgwin, (int)wparam);
        return 0;
    case WM_DBG_SYNC:
        resync(dbgwin);
        return 0;
    case WM_DBG_GOTO:
        goto_page(dbgwin, TAB_MEMORY);
        show_in_memory(dbgwin, (int)wparam, (int)wparam);
        return 0;
    case WM_NOTIFY:
        return on_notify(dbgwin, (NMHDR *)lparam);
    case WM_DRAWITEM:
        if (wparam == IDC_RUN) {
            draw_run_button(dbgwin, (const DRAWITEMSTRUCT *)lparam);
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HWND ctl = (HWND)lparam;
        HDC dc = (HDC)wparam;
        if (in_list(ctl, dbgwin->views, dbgwin->nviews)) {
            /* read-only views on the window colour, as text views are */
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        if (ctl == dbgwin->bp_msg) {
            SetTextColor(dc, RGB(0xc0, 0x1c, 0x28));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if (ctl == dbgwin->status && notice_showing(dbgwin)) {
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if (in_list(ctl, dbgwin->dim, dbgwin->ndim)) {
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        /* the disassembly scrolls under the pointer, focused or not */
        POINT ptr = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        RECT row;
        GetWindowRect(dbgwin->disasm, &row);
        if (page_visible(dbgwin, TAB_DISASM) && PtInRect(&row, ptr)) return SendMessageA(dbgwin->disasm, msg, wparam, lparam);
        break;
    }
    case WM_COMMAND: {
        const int ctl_id = LOWORD(wparam);
        if (ctl_id >= IDC_CHK0 && ctl_id <= IDC_CHK_LAST && HIWORD(wparam) == BN_CLICKED) {
            on_check(dbgwin, ctl_id - IDC_CHK0);
            return 0;
        }
        if (ctl_id >= IDC_SAVE_KIND0 && ctl_id <= IDC_SAVE_KIND_LAST) {
            on_save(dbgwin, ctl_id - IDC_SAVE_KIND0);
            return 0;
        }
        switch (ctl_id) {
        case IDC_RUN: on_run(dbgwin); return 0;
        case IDC_STEP: msxdebug_step(dbgwin->dbg); settle_and_sync(dbgwin, 1); return 0;
        case IDC_OVER: msxdebug_step_over(dbgwin->dbg); settle_and_sync(dbgwin, 1); return 0;
        case IDC_OUT: msxdebug_step_out(dbgwin->dbg); settle_and_sync(dbgwin, 1); return 0;
        case IDC_FRAME: msxdebug_frame(dbgwin->dbg); settle_and_sync(dbgwin, 1); return 0;
        case IDC_RUN_TO: on_run_to(dbgwin); return 0;
        case IDC_TOGGLE_BP: on_toggle_bp(dbgwin); return 0;
        case IDC_HELP_BTN: msx_help_for(dbgwin->hwnd); return 0;
        case IDC_SYMBOLS: on_symbols(dbgwin); return 0;
        case IDC_SAVE: save_menu(dbgwin); return 0;
        case IDC_POPOUT: pop_out(dbgwin, dbgwin->page); return 0;
        case IDC_MEM_BACK: on_mem_page(dbgwin, -1); return 0;
        case IDC_MEM_ON: on_mem_page(dbgwin, 1); return 0;
        case IDC_SRCH_NEXT: do_search(dbgwin, 0); return 0;
        case IDC_SRCH_PREV: do_search(dbgwin, 1); return 0;
        case IDC_FOLLOW_PC: refresh_disasm(dbgwin); return 0;
        case IDC_BP_TYPE:
            if (HIWORD(wparam) == CBN_SELCHANGE) on_bp_type(dbgwin);
            return 0;
        case IDC_BP_ADD: on_bp_add(dbgwin); return 0;
        case IDC_BP_REMOVE: on_bp_remove(dbgwin); return 0;
        case IDC_BP_HITS:
            msxdebug_breakpoint_clear_hits(dbgwin->dbg);
            resync(dbgwin);
            return 0;
        case IDC_BP_CLEAR:
            msxdebug_breakpoint_clear(dbgwin->dbg);
            set_label(dbgwin->bp_msg, "");
            resync(dbgwin);
            return 0;
        case IDC_SYM_FILTER:
            if (HIWORD(wparam) == EN_CHANGE) refresh_symbols(dbgwin);       /* the list follows the typing */
            return 0;
        case IDC_WT_ADD: add_watch(dbgwin); return 0;
        case IDC_WT_MEM: {
            uint16_t low, high;
            if (watch_range(dbgwin, &low, &high)) {
                goto_page(dbgwin, TAB_MEMORY);
                show_in_memory(dbgwin, low, high);
            } else {
                toast(dbgwin, "Select a watch that resolves to an address");
            }
            return 0;
        }
        case IDC_WT_BRK: {
            uint16_t low, high;
            if (watch_range(dbgwin, &low, &high)) watch_break(dbgwin, low, high);
            else toast(dbgwin, "Select a watch that resolves to an address");
            return 0;
        }
        case IDC_WT_REMOVE: {
            const int sel = (int)SendMessageA(dbgwin->wt_list, LB_GETCURSEL, 0, 0);
            if (sel >= 0) {
                msxwatch_remove(sel);
                refresh_watches(dbgwin);
            }
            return 0;
        }
        case IDC_H_REC0:
            msxdebug_trace_enable(dbgwin->dbg, SendMessageA((HWND)lparam, BM_GETCHECK, 0, 0) == BST_CHECKED);
            hist_refresh(dbgwin, 0, 1);
            return 0;
        case IDC_H_REC1:
            msxdebug_history_enable(dbgwin->dbg, SendMessageA((HWND)lparam, BM_GETCHECK, 0, 0) == BST_CHECKED);
            hist_refresh(dbgwin, 1, 1);
            return 0;
        case IDC_H_CLEAR0:
            msxdebug_trace_clear(dbgwin->dbg);
            hist_refresh(dbgwin, 0, 1);
            return 0;
        case IDC_H_CLEAR1:
            msxdebug_history_clear(dbgwin->dbg);
            hist_refresh(dbgwin, 1, 1);
            return 0;
        case IDC_H_FILTER0:
        case IDC_H_FILTER1:
            if (HIWORD(wparam) == CBN_SELCHANGE) hist_refresh(dbgwin, ctl_id == IDC_H_FILTER1, 1);
            return 0;
        case IDC_FUJI_CLEAR:
            msxdebug_fujinet_trace_clear(dbgwin->dbg);
            refresh_fujinet(dbgwin, 1);
            return 0;
        case IDCANCEL: hide_window(dbgwin); return 0;
        default: break;
        }
        break;
    }
    case WM_CLOSE:
        /* Hide and let the machine run; F12 brings it back, stopped. */
        hide_window(dbgwin);
        return 0;
    case WM_DESTROY: {
        int ptr;
        KillTimer(hwnd, TIMER_REFRESH);
        for (ptr = 0; ptr < TAB_COUNT; ptr++)     /* a peeled-off tab's window goes with the debugger */
            if (dbgwin->popped[ptr]) {
                HWND pop = dbgwin->popped[ptr];
                dbgwin->popped[ptr] = NULL;
                DestroyWindow(pop);
            }
        if (msxdebug_is_attached(dbgwin->dbg)) msxdebug_detach(dbgwin->dbg);
        DeleteObject(dbgwin->ui_font);
        DeleteObject(dbgwin->ui_bold);
        DeleteObject(dbgwin->mono);
        DeleteObject(dbgwin->accent);
        DeleteObject(dbgwin->bp_on);
        DeleteObject(dbgwin->bp_off);
        DestroyAcceleratorTable(dbgwin->accel);
        free(dbgwin->sym_text);
        free(dbgwin);
        g_dbg = NULL;
        return 0;
    }
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
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
    long num;
    size_t len;
    int idx, pass;
    if (!tab || !*tab) return -1;
    for (idx = 0; idx < (int)(sizeof k_tab_aliases / sizeof k_tab_aliases[0]); idx++)
        if (_stricmp(tab, k_tab_aliases[idx].name) == 0) return k_tab_aliases[idx].tab;
    num = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (num >= 0 && num < TAB_COUNT) ? (int)num : -1;
    len = strlen(tab);
    for (pass = 0; pass < 3; pass++)
        for (idx = 0; idx < TAB_COUNT; idx++) {
            if (pass == 0 && _strnicmp(tab, k_tabs[idx].name, len) == 0) return idx;
            if (pass == 1 && _strnicmp(tab, k_tabs[idx].label, len) == 0) return idx;
            if (pass == 2) {
                const char *ptr = k_tabs[idx].label;
                while ((ptr = strchr(ptr, ' ')) != NULL)
                    if (_strnicmp(tab, ++ptr, len) == 0) return idx;
            }
        }
    return -1;
}

/* For captures and tests: MSX_HISTORY records from the start, MSX_WATCH="x,y"
 * adds watches, MSX_WATCH_BREAK="x" a write-when-changed breakpoint on it,
 * MSX_MEM_GOTO="x" shows it in Memory, MSX_POPOUT=<tab> peels a tab off. */
static void apply_env_hooks(debugger *dbgwin)
{
    const char *hist = getenv("MSX_HISTORY"), *watch_text = getenv("MSX_WATCH"), *break_text = getenv("MSX_WATCH_BREAK"),
               *goto_text = getenv("MSX_MEM_GOTO");
    uint16_t low, high;
    int pop;

    if (hist && *hist && *hist != '0') msxdebug_history_enable(dbgwin->dbg, 1);
    if (watch_text && *watch_text) {
        char list[512], *item, *save = NULL;
        snprintf(list, sizeof list, "%s", watch_text);
        for (item = strtok_s(list, ",", &save); item; item = strtok_s(NULL, ",", &save)) {
            item = strip(item);
            if (*item) msxwatch_add(item);
        }
    }
    if (break_text && *break_text && msxsym_parse_range(dbgwin->dbg, break_text, &low, &high)) {
        char err[160];
        msxdebug_breakpoint_add(dbgwin->dbg, MSXDEBUG_BP_WRITE, low, high, "value != old", err, sizeof err);
    }
    if (goto_text && *goto_text && msxsym_parse_range(dbgwin->dbg, goto_text, &low, &high)) show_in_memory(dbgwin, low, high);
    pop = tab_from_env_var("MSX_POPOUT");
    if (pop >= 0) pop_out(dbgwin, pop);
    resync(dbgwin);
}

void msx_debugger_show(HWND parent, msxsession *session)
{
    static int registered;
    INITCOMMONCONTROLSEX icc;
    HINSTANCE inst;
    debugger *dbgwin;
    ACCEL accels[7];
    int tab;

    if (g_dbg) {
        dbgwin = g_dbg;
        ShowWindow(dbgwin->hwnd, IsIconic(dbgwin->hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(dbgwin->hwnd);
        if (!msxdebug_is_attached(dbgwin->dbg) || !IsWindowVisible(dbgwin->hwnd) || !msxdebug_is_stopped(dbgwin->dbg))
            attach_and_refresh(dbgwin);
        else
            resync(dbgwin);
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

    dbgwin = calloc(1, sizeof *dbgwin);
    if (!dbgwin) return;
    dbgwin->session = session;
    dbgwin->dbg = msxsession_debugger(session);
    if (!dbgwin->dbg) {
        free(dbgwin);
        MessageBoxA(parent, "The debugger could not be created for this session.", "Debugger Unavailable",
                    MB_ICONERROR);
        return;
    }
    dbgwin->selected = -1;
    dbgwin->pc_line = -1;
    dbgwin->fuji_count = -1;
    dbgwin->h_seen[0] = dbgwin->h_seen[1] = (uint64_t)-1;
    g_dbg = dbgwin;

    dbgwin->dpi = window_dpi(parent);
    /* owned by the main window: over it, and gone with it */
    dbgwin->hwnd = CreateWindowExA(0, DBG_CLASS, "Debugger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                              CW_USEDEFAULT, scaled(dbgwin, 1250), scaled(dbgwin, 860), parent, NULL, inst, NULL);
    if (!dbgwin->hwnd) {
        free(dbgwin);
        g_dbg = NULL;
        return;
    }
    if (window_dpi(dbgwin->hwnd) != dbgwin->dpi) {
        /* placed on a monitor of another DPI than the parent's */
        dbgwin->dpi = window_dpi(dbgwin->hwnd);
        SetWindowPos(dbgwin->hwnd, NULL, 0, 0, scaled(dbgwin, 1250), scaled(dbgwin, 860), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    make_fonts(dbgwin);
    dbgwin->accent = CreateSolidBrush(ACCENT_RGB);
    dbgwin->bp_on = CreateSolidBrush(RGB(0xe0, 0x1b, 0x24));
    dbgwin->bp_off = CreateSolidBrush(RGB(0x9a, 0x99, 0x96));

    build_controls(dbgwin);

    /* F5/F7/F8/Shift+F8/Shift+F7/F9/F12 work wherever the focus is in the
     * window. */
    accels[0].fVirt = FVIRTKEY;          accels[0].key = VK_F5;  accels[0].cmd = IDC_RUN;
    accels[1].fVirt = FVIRTKEY;          accels[1].key = VK_F7;  accels[1].cmd = IDC_STEP;
    accels[2].fVirt = FVIRTKEY;          accels[2].key = VK_F8;  accels[2].cmd = IDC_OVER;
    accels[3].fVirt = FVIRTKEY | FSHIFT; accels[3].key = VK_F8;  accels[3].cmd = IDC_OUT;
    accels[4].fVirt = FVIRTKEY | FSHIFT; accels[4].key = VK_F7;  accels[4].cmd = IDC_FRAME;
    accels[5].fVirt = FVIRTKEY;          accels[5].key = VK_F9;  accels[5].cmd = IDC_TOGGLE_BP;
    accels[6].fVirt = FVIRTKEY;          accels[6].key = VK_F12; accels[6].cmd = IDCANCEL;
    dbgwin->accel = CreateAcceleratorTableA(accels, 7);

    tab = tab_from_env_var("MSX_DEBUGGER_TAB");
    dbgwin->page = tab >= 0 ? tab : TAB_CONSOLE;
    SendMessageA(dbgwin->tabs, TCM_SETCURSEL, (WPARAM)dbgwin->page, 0);
    show_page(dbgwin, dbgwin->page);
    layout(dbgwin);

    ShowWindow(dbgwin->hwnd, SW_SHOW);
    if (dbgwin->page == TAB_CONSOLE) SetFocus(dbgwin->con_in);
    attach_and_refresh(dbgwin);
    apply_env_hooks(dbgwin);
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
    int ptr, mine;
    if (!g_dbg || !g_dbg->accel) return 0;
    mine = msg->hwnd == g_dbg->hwnd || IsChild(g_dbg->hwnd, msg->hwnd);
    for (ptr = 0; !mine && ptr < TAB_COUNT; ptr++)     /* a peeled-off tab's window too */
        if (g_dbg->popped[ptr] && (msg->hwnd == g_dbg->popped[ptr] || IsChild(g_dbg->popped[ptr], msg->hwnd))) mine = 1;
    if (!mine) return 0;
    /* F12 closes without waiting for the accelerator (Windows keeps F12 for
     * debuggers); its key-up goes with it */
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_F12) {
        hide_window(g_dbg);
        return 1;
    }
    return TranslateAcceleratorA(g_dbg->hwnd, g_dbg->accel, msg) ? 1 : 0;
}
