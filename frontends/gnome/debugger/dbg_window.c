/*
 * Debugger window (GTK4/libadwaita) over the session's Z80 debugger
 * engine, via core/include/msxdebug.h. Twelve tabs, in the family order:
 * Console (the prompt: expressions, completion, history), Disassembly (a
 * breakpoint gutter and a selectable row), Memory (the 64K the CPU sees,
 * with a Poke bar and a search), Breakpoints (Execute / Read / Write / Access
 * / Port In / Port Out, conditions are expressions), Watches, Cart & I/O
 * (the machine, its memory map, slots, mapper, PPI, keyboard and joysticks),
 * Trace and History (one ring of executed instructions), Video
 * (dbg_video.c; the old VDP panes are its Decoded sources and its VRAM
 * section), Sound (PSG, SCC, MSX-MUSIC, MSX-AUDIO as present), Symbols and
 * FujiNet (every FujiBus packet on the cartridge's link, both ways). An
 * editable register strip sits under all of them. A toolbar carries the
 * stepping controls, symbol loading, file saves and Pop Out (a tab in a
 * window of its own; Dock puts it back); the family's keys apply (F5 run /
 * stop, F7 step, F8 step over, Shift+F8 step out, Shift+F7 frame, F9
 * breakpoint on the selected line, F12 close).
 *
 * The engine is engaged only while this window exists: opening it attaches
 * (which stops the machine, as on every sibling), closing it detaches and lets
 * the machine run on at full speed. Closing destroys the window, so the
 * Video controller (which asks the host to capture frames) goes with it.
 *
 * The window polls the engine's generation counter and stop snapshot on a
 * short timer and refreshes only what is on screen (the tab showing, and any
 * tab in a window of its own), and only when something changed, so a stopped
 * machine costs nothing and a running one shows live values twice a second.
 * It does not use the engine's stop callback: that fires off the UI thread,
 * and the snapshot says the same thing.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"
#include "dbg_video.h"
#include "../help/help_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msxdebug.h"
#include "msxmemtools.h"

#define DISASM_WINDOW 48
#define NREGS 15
#define NFLAGS 8
#define MEM_ROWS 32
#define MEM_PAGE (MEM_ROWS * 16)
#define POKE_MAX 64
#define TRACE_MAX 512
#define TICK_MS 50
#define LIVE_TICKS 10           /* live values twice a second while the machine runs */
#define HIST_SHOWN 400          /* lines on the Trace / History page */
#define HIST_COPY 4000

/* The tabs, in order; MSX_DEBUGGER_TAB and MSX_POPOUT take a number or the
 * start of a name. */
enum { TAB_CONSOLE, TAB_DISASM, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_CART, TAB_TRACE,
       TAB_HISTORY, TAB_VIDEO, TAB_SOUND, TAB_SYMBOLS, TAB_FUJINET, TAB_COUNT };

static const char *const k_tab_names[TAB_COUNT] = {
    "Console", "Disassembly", "Memory", "Breakpoints", "Watches", "Cart & I/O", "Trace",
    "History", "Video", "Sound", "Symbols", "FujiNet",
};

/* Help's contexts (msxhelp.h), one per tab */
static const char *const k_tab_contexts[TAB_COUNT] = {
    "debugger/console", "debugger/disassembly", "debugger/memory", "debugger/breakpoints",
    "debugger/watches", "debugger/cart", "debugger/trace", "debugger/history",
    "debugger/video", "debugger/sound", "debugger/symbols", "debugger/fujinet",
};

/* The tabs' older names, which the environment variables still take. */
static const struct { const char *name; int tab; } k_tab_aliases[] = {
    { "cpu", TAB_MEMORY }, { "registers", TAB_MEMORY }, { "vdp", TAB_VIDEO }, { "io", TAB_CART },
    { "i/o", TAB_CART }, { "slots", TAB_CART }, { "cart", TAB_CART }, { "psg", TAB_SOUND },
    { "scc", TAB_SOUND },
};

/* The register entries, each with how many hex digits it shows. */
static const struct { const char *name; int reg; int digits; } k_regs[NREGS] = {
    { "AF", MSX_REG_AF, 4 }, { "BC", MSX_REG_BC, 4 }, { "DE", MSX_REG_DE, 4 },
    { "HL", MSX_REG_HL, 4 }, { "IX", MSX_REG_IX, 4 }, { "IY", MSX_REG_IY, 4 },
    { "SP", MSX_REG_SP, 4 }, { "PC", MSX_REG_PC, 4 },
    { "AF'", MSX_REG_AF2, 4 }, { "BC'", MSX_REG_BC2, 4 }, { "DE'", MSX_REG_DE2, 4 },
    { "HL'", MSX_REG_HL2, 4 }, { "I", MSX_REG_I, 2 }, { "R", MSX_REG_R, 2 },
    { "IM", MSX_REG_IM, 1 },
};

/* The flags, in F's bit order from bit 7; the last two are not in F. */
static const struct { const char *name; unsigned mask; } k_flags[NFLAGS] = {
    { "S", 0x80 }, { "Z", 0x40 }, { "Y", 0x20 }, { "H", 0x10 },
    { "X", 0x08 }, { "P/V", 0x04 }, { "N", 0x02 }, { "C", 0x01 },
};

typedef struct {
    GtkWindow *win;
    msxsession *session;
    msxdebug *dbg;
    unsigned seen_generation;
    uint64_t seen_serial;
    gboolean was_stopped;
    gboolean torn_down;
    int running_ticks;
    guint timer;
    gulong dark_id;

    AdwWindowTitle *title;    /* "Debugger", with the stop reason under it */
    GtkButton *run_btn;
    AdwToastOverlay *toasts;
    GtkNotebook *notebook;
    GtkWidget *pages[TAB_COUNT];
    GtkWindow *detached[TAB_COUNT];
    GtkTextTag *tag_pc, *tag_mem_mark;

    /* console */
    GtkTextView *console_out;
    GtkEntry *console_in;
    GPtrArray *history;
    guint history_pos;

    /* the register strip */
    GtkEntry *reg[NREGS];
    GtkCheckButton *flag[NFLAGS];
    GtkCheckButton *iff[2];
    GtkLabel *halt, *state;
    gboolean updating_flags;
    gboolean force_entries;   /* a refresh may overwrite a focused entry */

    /* memory */
    GtkTextView *mem_view;
    GtkEntry *mem_goto, *mem_write;
    int mem_base;
    msxdebug_region map[8];   /* the memory map, fetched once per stop */
    int map_count;
    uint64_t map_serial;
    gboolean map_valid;
    gboolean mem_hl;          /* the range Go to / a watch / a search found, marked */
    int mem_hl_lo, mem_hl_hi;
    GtkDropDown *srch_kind;
    GtkEntry *srch_entry;
    GtkLabel *srch_note;

    /* disassembly */
    GtkTextView *disasm;
    GtkCheckButton *follow_pc;
    GtkEntry *jump;
    int disasm_top;           /* address of the first instruction */
    int line_addr[DISASM_WINDOW * 2];   /* per buffer line; -1 for a label line */
    int line_count;
    int selected;             /* the selected row's address, -1 none */

    /* breakpoints */
    GtkListBox *bp_list;
    GtkDropDown *bp_type;
    GtkEntry *bp_addr, *bp_cond;
    GtkLabel *bp_msg, *bp_stop;
    GString *bp_sig;          /* what the list shows, to skip a needless rebuild */

    /* watches */
    GtkEntry *wt_entry;
    GtkListBox *wt_list;
    GString *wt_sig;

    /* cart & i/o, sound */
    GtkTextView *cart_text, *io_text, *sound_text;

    /* trace (0, live) and history (1, when stopped) share one ring */
    GtkToggleButton *h_rec[2];
    GtkDropDown *h_filter[2];
    GtkEntry *h_range[2];
    GtkTextView *h_text[2];
    GtkLabel *h_note[2];
    uint64_t h_seen[2];
    gboolean h_sync;

    /* symbols */
    GtkListBox *symlist;
    GtkEntry *sym_filter;
    GtkLabel *sym_count;
    int sym_built;            /* entries the list was built from, -1 = never */
    unsigned sym_gen;
    char sym_text[64];

    /* fujinet */
    GtkLabel *fuji_note;
    GtkTextView *fuji_text, *fuji_state;
    uint64_t fuji_last;
    int fuji_count;
} DbgWin;

static DbgWin *g_win;

static void refresh_now(DbgWin *self);

/* ---- accent --------------------------------------------------------------- */

static gboolean dark_theme(void)
{
    return adw_style_manager_get_dark(adw_style_manager_get_default());
}

/* The accent as a fill (with its ink) and as text on the theme: the icon's
 * blue is unreadable on a dark theme, so there the light variant is used with
 * black ink. */
static unsigned accent_fill(void) { return dark_theme() ? MSXSESSION_ACCENT_LIGHT_RGB : MSXSESSION_ACCENT_RGB; }
static unsigned accent_ink(void) { return dark_theme() ? 0x000000u : MSXSESSION_ACCENT_INK_RGB; }

static void install_css(void)
{
    static GtkCssProvider *css;
    char buf[640];
    g_snprintf(buf, sizeof buf,
        ".msx-accent { background: #%06x; color: #%06x; }\n"
        ".msx-accent:hover { background: #%06x; }\n"
        ".msx-accent-text { color: #%06x; font-weight: bold; }\n"
        ".msx-toggle:checked { background: #%06x; color: #%06x; }\n",
        accent_fill(), accent_ink(), accent_fill(), accent_fill(), accent_fill(), accent_ink());
    if (!css) {
        css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(gdk_display_get_default(),
            GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
#if GTK_CHECK_VERSION(4, 12, 0)
    gtk_css_provider_load_from_string(css, buf);
#else
    gtk_css_provider_load_from_data(css, buf, -1);
#endif
}

static void accent_tag_style(GtkTextTag *tag)
{
    char fill[16], ink[16];
    g_snprintf(fill, sizeof fill, "#%06x", accent_fill());
    g_snprintf(ink, sizeof ink, "#%06x", accent_ink());
    g_object_set(tag, "paragraph-background", fill, "foreground", ink, NULL);
}

static GtkTextTag *accent_tag(GtkTextView *view, const char *name)
{
    GtkTextTag *tag = gtk_text_buffer_create_tag(gtk_text_view_get_buffer(view), name, NULL);
    accent_tag_style(tag);
    return tag;
}

/* The theme changed: the CSS and the tags that wear the accent follow. */
static void on_dark_changed(GObject *obj, GParamSpec *spec, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)obj; (void)spec;
    install_css();
    if (self->tag_pc) accent_tag_style(self->tag_pc);
    if (self->tag_mem_mark) {
        char fill[16], ink[16];
        g_snprintf(fill, sizeof fill, "#%06x", accent_fill());
        g_snprintf(ink, sizeof ink, "#%06x", accent_ink());
        g_object_set(self->tag_mem_mark, "background", fill, "foreground", ink, NULL);
    }
}

/* ---- helpers -------------------------------------------------------------- */

/* Engine text for a text buffer: valid UTF-8, control bytes dropped. */
static char *clean_text(const char *text)
{
    char *valid = g_utf8_make_valid(text, -1), *src, *dst;
    for (src = dst = valid; *src; src++)
        if ((unsigned char)*src >= 0x20 || *src == '\n' || *src == '\t') *dst++ = *src;
    *dst = '\0';
    return valid;
}

static GtkAdjustment *vadj(GtkTextView *view)
{
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(view));
    return GTK_IS_SCROLLED_WINDOW(parent)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent)) : NULL;
}

/* A log (the console, the FujiNet packets, the trace) follows its end while
 * its reader is there. New text grows the view only once it is laid out (or
 * its tab first shows), so the end is pinned whenever the view's height
 * changes; whether to follow is decided as text arrives. */
static gboolean following(GtkTextView *view)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "follow")) != 0;
}

static void on_log_resized(GtkAdjustment *adj, gpointer view)
{
    if (following(view))
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) -
                                      gtk_adjustment_get_page_size(adj));
}

static void make_log(GtkTextView *view)
{
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    /* _object: the handler goes when the view does */
    g_signal_connect_object(vadj(view), "changed", G_CALLBACK(on_log_resized), view, 0);
}

/* Before new text: follow if the reader is at the end (or never looked). */
static void log_note_reader(GtkTextView *view)
{
    GtkAdjustment *adj = vadj(view);
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(
        gtk_adjustment_get_page_size(adj) <= 0.0 ||
        gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >=
            gtk_adjustment_get_upper(adj) - 2.0));
}

/* Replaces a view's text where the reader left it: a refresh (a step, or
 * the live values twice a second) must not throw a scrolled dump back to
 * its top. The top line is noted and scrolled back to once the new text is
 * laid out (scroll_to_mark waits for that). A log that is following goes
 * to its end instead. */
static void set_text_at(GtkTextView *view, const char *text, gboolean follow)
{
    GtkTextBuffer *tbuf = gtk_text_view_get_buffer(view);
    GtkAdjustment *adj = vadj(view);
    char *clean = clean_text(text);
    GtkTextIter iter;
    int top = 0;

    if (!follow && adj && gtk_adjustment_get_value(adj) > 0.0) {
        gtk_text_view_get_line_at_y(view, &iter, (int)gtk_adjustment_get_value(adj), NULL);
        top = gtk_text_iter_get_line(&iter);
    }
    gtk_text_buffer_set_text(tbuf, clean, -1);
    g_free(clean);
    if (follow) {
        g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
        if (adj) on_log_resized(adj, view);
        return;
    }
    if (top > 0) {
        GtkTextMark *mark;
        gtk_text_buffer_get_iter_at_line(tbuf, &iter, top);
        mark = gtk_text_buffer_create_mark(tbuf, NULL, &iter, TRUE);
        gtk_text_view_scroll_to_mark(view, mark, 0.0, TRUE, 0.0, 0.0);
        gtk_text_buffer_delete_mark(tbuf, mark);
    }
}

static void set_text(GtkTextView *view, const char *text)
{
    set_text_at(view, text, FALSE);
}

static void append_text(GtkTextView *view, const char *text, gboolean force_follow)
{
    GtkTextBuffer *tbuf = gtk_text_view_get_buffer(view);
    GtkTextIter end;
    char *clean = clean_text(text);
    log_note_reader(view);
    if (force_follow)
        g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    gtk_text_buffer_get_end_iter(tbuf, &end);
    gtk_text_buffer_insert(tbuf, &end, clean, -1);
    g_free(clean);
}

/* A number as typed: hex by default, $ or 0x or a trailing h for hex, #
 * for decimal. */
static int parse_num(const char *text, long *out)
{
    char buf[64], *end;
    size_t count;
    long val;
    while (*text == ' ') text++;
    g_strlcpy(buf, text, sizeof buf);
    count = strlen(buf);
    while (count && buf[count - 1] == ' ') buf[--count] = '\0';
    if (!count) return 0;
    if (buf[0] == '$') val = strtol(buf + 1, &end, 16);
    else if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) val = strtol(buf + 2, &end, 16);
    else if (buf[0] == '#') val = strtol(buf + 1, &end, 10);
    else if (buf[count - 1] == 'h' || buf[count - 1] == 'H') { buf[count - 1] = '\0'; val = strtol(buf, &end, 16); }
    else val = strtol(buf, &end, 16);
    if (end == buf || *end) return 0;
    *out = val;
    return 1;
}

/* A value typed in a register entry or a poke: a number, else an expression
 * ("hl+1", "[sp]", a label). */
static int parse_value(DbgWin *self, const char *text, long *out)
{
    char err[160];
    if (parse_num(text, out)) return 1;
    return msxdebug_eval(self->dbg, text, out, err, sizeof err) == 0;
}

/* One address (a label, a number or an expression), or the start of a range. */
static int parse_addr(DbgWin *self, const char *text, int *out)
{
    uint16_t range_lo, range_hi;
    if (!msxsym_parse_range(self->dbg, text, &range_lo, &range_hi)) return 0;
    *out = range_lo;
    return 1;
}

/* A refresh does not overwrite a value being typed: a focused entry keeps
 * what is in it, until Enter (force_entries) says to take the engine's. */
static void set_entry(DbgWin *self, GtkEntry *ent, const char *text)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(ent));
    GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
    if (focus && !self->force_entries &&
        (focus == GTK_WIDGET(ent) || gtk_widget_is_ancestor(focus, GTK_WIDGET(ent))))
        return;
    gtk_editable_set_text(GTK_EDITABLE(ent), text);
}

static GtkWidget *mono_view(GtkTextView **out, gboolean wrap)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    *out = GTK_TEXT_VIEW(view);
    return scroll;
}

static GtkWidget *labeled(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *lbl = gtk_label_new(text);
    gtk_widget_add_css_class(lbl, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_box_append(GTK_BOX(box), lbl);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

/* A heading over a block. */
static GtkWidget *headed(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *lbl = gtk_label_new(text);
    gtk_widget_add_css_class(lbl, "heading");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_box_append(GTK_BOX(box), lbl);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

static GtkWidget *padded(GtkWidget *self)
{
    gtk_widget_set_margin_top(self, 8);
    gtk_widget_set_margin_bottom(self, 8);
    gtk_widget_set_margin_start(self, 8);
    gtk_widget_set_margin_end(self, 8);
    return self;
}

static void toast(DbgWin *self, const char *msg)
{
    adw_toast_overlay_add_toast(self->toasts, adw_toast_new(msg));
}

/* Messages from Load Symbols and Save land in the console's transcript and,
 * since the console may not be the tab showing, in a toast. */
static void notice(DbgWin *self, const char *msg)
{
    char line[700];
    g_snprintf(line, sizeof line, "%s\n", msg);
    append_text(self->console_out, line, FALSE);
    toast(self, msg);
}

/* The step controls wait a moment for the engine to come to rest, so a
 * step does not flash "Running" before the status catches up. */
static void settle(DbgWin *self, gboolean want_stopped)
{
    int idx;
    for (idx = 0; idx < 20 && (msxdebug_is_stopped(self->dbg) != 0) != want_stopped; idx++)
        g_usleep(2000);
}

/* Tabs can be moved into windows of their own, so a page's place in the
 * notebook is not its identity: each carries its original index. */
static int page_tab(GtkWidget *page)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(page), "tab-index")) - 1;
}

static int current_tab(DbgWin *self)
{
    GtkWidget *page = gtk_notebook_get_nth_page(self->notebook, gtk_notebook_get_current_page(self->notebook));
    return page ? page_tab(page) : -1;
}

/* On screen: the notebook's page, or a window of its own. */
static gboolean tab_visible(DbgWin *self, int tab)
{
    return current_tab(self) == tab || self->detached[tab] != NULL;
}

static void show_tab(DbgWin *self, int tab)
{
    if (self->detached[tab])
        gtk_window_present(self->detached[tab]);
    else
        gtk_notebook_set_current_page(self->notebook, gtk_notebook_page_num(self->notebook, self->pages[tab]));
}

/* ---- the register strip --------------------------------------------------- */

static int *cpu_field(msxdebug_cpu *cpu, int reg)
{
    switch (reg) {
    case MSX_REG_PC: return &cpu->pc;
    case MSX_REG_SP: return &cpu->sp;
    case MSX_REG_AF: return &cpu->af;
    case MSX_REG_BC: return &cpu->bc;
    case MSX_REG_DE: return &cpu->de;
    case MSX_REG_HL: return &cpu->hl;
    case MSX_REG_IX: return &cpu->ix;
    case MSX_REG_IY: return &cpu->iy;
    case MSX_REG_AF2: return &cpu->af2;
    case MSX_REG_BC2: return &cpu->bc2;
    case MSX_REG_DE2: return &cpu->de2;
    case MSX_REG_HL2: return &cpu->hl2;
    case MSX_REG_I: return &cpu->i;
    case MSX_REG_R: return &cpu->r;
    case MSX_REG_IM: return &cpu->im;
    case MSX_REG_IFF1: return &cpu->iff1;
    case MSX_REG_IFF2: return &cpu->iff2;
    default: return NULL;
    }
}

/* Writes a changed register set; the engine says why when it refuses (running,
 * IM above 2). */
static gboolean apply_regs(DbgWin *self, const msxdebug_cpu *cpu)
{
    char err[160] = "";
    if (msxdebug_regs_set(self->dbg, cpu, err, sizeof err)) return TRUE;
    toast(self, err[0] ? err : "The registers could not be set");
    return FALSE;
}

static void refresh_regs(DbgWin *self)
{
    msxdebug_cpu cpu;
    char buf[32];
    int idx;
    gboolean stopped = msxdebug_is_stopped(self->dbg) != 0;

    for (idx = 0; idx < NREGS; idx++) gtk_widget_set_sensitive(GTK_WIDGET(self->reg[idx]), stopped);
    for (idx = 0; idx < NFLAGS; idx++) gtk_widget_set_sensitive(GTK_WIDGET(self->flag[idx]), stopped);
    for (idx = 0; idx < 2; idx++) gtk_widget_set_sensitive(GTK_WIDGET(self->iff[idx]), stopped);
    if (!stopped) {
        for (idx = 0; idx < NREGS; idx++) set_entry(self, self->reg[idx], "");
        self->updating_flags = TRUE;
        for (idx = 0; idx < NFLAGS; idx++) gtk_check_button_set_active(self->flag[idx], FALSE);
        for (idx = 0; idx < 2; idx++) gtk_check_button_set_active(self->iff[idx], FALSE);
        self->updating_flags = FALSE;
        gtk_label_set_text(self->halt, "");
        gtk_label_set_text(self->state, "running: stop the machine to read or edit the registers");
        return;
    }
    msxdebug_cpu_get(self->dbg, &cpu);
    for (idx = 0; idx < NREGS; idx++) {
        int *field = cpu_field(&cpu, k_regs[idx].reg);
        g_snprintf(buf, sizeof buf, "%0*X", k_regs[idx].digits, field ? (unsigned)*field : 0);
        set_entry(self, self->reg[idx], buf);
    }
    self->updating_flags = TRUE;
    for (idx = 0; idx < NFLAGS; idx++)
        gtk_check_button_set_active(self->flag[idx], (cpu.af & k_flags[idx].mask) != 0);
    gtk_check_button_set_active(self->iff[0], cpu.iff1 != 0);
    gtk_check_button_set_active(self->iff[1], cpu.iff2 != 0);
    self->updating_flags = FALSE;
    gtk_label_set_text(self->halt, cpu.halted ? "HALT" : "");
    gtk_label_set_text(self->state, (msxdebug_trace_enabled(self->dbg) || msxdebug_history_enabled(self->dbg)) ? ""
                                 : "HALT is shown only while Trace or History records");
}

/* Enter in a register entry sets it while the machine is stopped. */
static void on_reg_activate(GtkEntry *entry, gpointer user_data)
{
    DbgWin *self = user_data;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "reg"));
    msxdebug_cpu cpu;
    long val;
    int *field;
    if (!msxdebug_is_stopped(self->dbg)) {
        toast(self, "Stop the machine to edit the registers");
    } else if (!parse_value(self, gtk_editable_get_text(GTK_EDITABLE(entry)), &val)) {
        toast(self, "A register takes a number (hex by default, # for decimal) or an expression");
    } else {
        msxdebug_cpu_get(self->dbg, &cpu);
        field = cpu_field(&cpu, k_regs[idx].reg);
        if (field) {
            *field = (int)(val & (k_regs[idx].digits >= 4 ? 0xFFFF : k_regs[idx].digits == 2 ? 0xFF : 0xF));
            apply_regs(self, &cpu);
        }
    }
    /* Enter ends the edit: show the engine's value, or put the old one back */
    self->force_entries = TRUE;
    refresh_now(self);
    self->force_entries = FALSE;
}

static void on_flag_toggled(GtkCheckButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "flag"));
    msxdebug_cpu cpu;
    gboolean active = gtk_check_button_get_active(btn);
    if (self->updating_flags) return;
    if (!msxdebug_is_stopped(self->dbg)) {
        toast(self, "Stop the machine to edit the registers");
        refresh_regs(self);          /* put the box back */
        return;
    }
    msxdebug_cpu_get(self->dbg, &cpu);
    if (idx < NFLAGS) {
        cpu.af = active ? (cpu.af | (int)k_flags[idx].mask) : (cpu.af & ~(int)k_flags[idx].mask);
    } else {
        *cpu_field(&cpu, idx == NFLAGS ? MSX_REG_IFF1 : MSX_REG_IFF2) = active ? 1 : 0;
    }
    apply_regs(self, &cpu);
    refresh_now(self);
}

/* ---- refreshers ----------------------------------------------------------- */

static void refresh_mem(DbgWin *self)
{
    uint8_t data[MEM_PAGE];
    GString *str = g_string_new(NULL);
    GtkTextBuffer *tbuf = gtk_text_view_get_buffer(self->mem_view);
    int row, col;
    char last[64] = "";

    memset(data, 0, sizeof data);
    msxdebug_read(self->dbg, (uint16_t)self->mem_base, data, MEM_PAGE);
    /* the slots do not change while the machine is stopped: one fetch per stop */
    if (!self->map_valid || self->map_serial != self->seen_serial || !msxdebug_is_stopped(self->dbg)) {
        self->map_count = msxdebug_memory_map(self->dbg, self->map, 8);
        self->map_serial = self->seen_serial;
        self->map_valid = TRUE;
    }
    g_string_append(str, "      0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    for (row = 0; row < MEM_ROWS; row++) {
        char region[64];
        g_string_append_printf(str, "%04X  ", self->mem_base + row * 16);
        for (col = 0; col < 16; col++)
            g_string_append_printf(str, "%02X ", data[row * 16 + col]);
        g_string_append_c(str, ' ');
        for (col = 0; col < 16; col++) {
            uint8_t chr = data[row * 16 + col];
            g_string_append_c(str, (chr >= 0x20 && chr <= 0x7E) ? (char)chr : '.');
        }
        /* the region is named on the first row of a run, not on every row */
        msxmem_region_name(self->map, self->map_count, (uint16_t)(self->mem_base + row * 16), region, sizeof region);
        if (region[0] && strcmp(region, last) != 0)
            g_string_append_printf(str, "  ; %s", region);
        g_strlcpy(last, region, sizeof last);
        g_string_append_c(str, '\n');
    }
    set_text(self->mem_view, str->str);
    g_string_free(str, TRUE);

    /* the range Go to found stays marked across refreshes, byte by byte */
    if (self->mem_hl) {
        int addr;
        for (addr = self->mem_hl_lo; addr <= self->mem_hl_hi; addr++) {
            GtkTextIter iter_start, iter_end;
            int line, off;
            if (addr < self->mem_base || addr >= self->mem_base + MEM_PAGE) continue;
            line = (addr - self->mem_base) / 16 + 1;
            off = 6 + ((addr - self->mem_base) % 16) * 3;
            gtk_text_buffer_get_iter_at_line_offset(tbuf, &iter_start, line, off);
            gtk_text_buffer_get_iter_at_line_offset(tbuf, &iter_end, line, off + 2);
            gtk_text_buffer_apply_tag_by_name(tbuf, "mark", &iter_start, &iter_end);
        }
    }
}

static void refresh_disasm(DbgWin *self)
{
    static msxdebug_line lines[DISASM_WINDOW];
    GtkTextBuffer *tbuf = gtk_text_view_get_buffer(self->disasm);
    GString *str = g_string_new(NULL);
    GtkTextIter iter_start, iter_end;
    int count, pc_line, idx, line = 0, sel_line = -1, pc_buf = -1;

    /* Following, the PC sits a third of the way down the window. */
    if (gtk_check_button_get_active(self->follow_pc)) {
        msxdebug_cpu cpu;
        msxdebug_cpu_get(self->dbg, &cpu);
        self->disasm_top = msxdebug_row_address(self->dbg, (uint16_t)cpu.pc, -(DISASM_WINDOW / 3));
    }

    count = msxdebug_disassemble(self->dbg, (uint16_t)self->disasm_top, lines, DISASM_WINDOW, &pc_line);
    gtk_text_buffer_set_text(tbuf, "", -1);
    for (idx = 0; idx < count; idx++) {
        /* a label gets a line of its own above its instruction */
        if (lines[idx].label[0]) {
            g_string_append_printf(str, "      %s:\n", lines[idx].label);
            self->line_addr[line++] = -1;
        }
        g_string_append_printf(str, "%s%c %04X  %-11s %-16s%s%s\n",
                               lines[idx].has_breakpoint ? "\xe2\x97\x8f" : " ",
                               lines[idx].is_pc ? '>' : ' ',
                               lines[idx].address, lines[idx].bytes, lines[idx].disasm,
                               lines[idx].comment[0] ? "; " : "", lines[idx].comment);
        if (lines[idx].address == self->selected) sel_line = line;
        if (idx == pc_line) pc_buf = line;
        self->line_addr[line++] = lines[idx].address;
    }
    self->line_count = line;
    if (count == 0) g_string_append(str, "(no disassembly)\n");
    gtk_text_buffer_set_text(tbuf, str->str, -1);

    for (idx = 0, line = 0; idx < count; idx++) {
        /* the gutter marker: red while the breakpoint is enabled, grey off */
        if (lines[idx].label[0]) line++;
        if (lines[idx].has_breakpoint) {
            gtk_text_buffer_get_iter_at_line(tbuf, &iter_start, line);
            iter_end = iter_start;
            gtk_text_iter_forward_char(&iter_end);
            gtk_text_buffer_apply_tag_by_name(tbuf, lines[idx].breakpoint_enabled ? "bp_on" : "bp_off",
                                              &iter_start, &iter_end);
        }
        line++;
    }
    if (sel_line >= 0) {
        gtk_text_buffer_get_iter_at_line(tbuf, &iter_start, sel_line);
        gtk_text_buffer_get_iter_at_line(tbuf, &iter_end, sel_line + 1);
        gtk_text_buffer_apply_tag_by_name(tbuf, "sel", &iter_start, &iter_end);
    }
    /* the PC line in the accent colour (over the selection) */
    if (pc_buf >= 0) {
        gtk_text_buffer_get_iter_at_line(tbuf, &iter_start, pc_buf);
        gtk_text_buffer_get_iter_at_line(tbuf, &iter_end, pc_buf + 1);
        gtk_text_buffer_apply_tag_by_name(tbuf, "pc", &iter_start, &iter_end);
    }
    g_string_free(str, TRUE);
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat folded into the newest. */
static void refresh_fujinet(DbgWin *self, gboolean force)
{
    static msx_fuji_packet pkt[TRACE_MAX];
    static char state[2048];
    GString *txt;
    int count = msxdebug_fujinet_trace(self->dbg, pkt, TRACE_MAX), idx;

    if (msxsession_fujinet_external(self->session)) {
        g_autofree char *ext = g_strdup_printf(
            "External FujiNet %s, cartridge %s.",
            msxsession_fujinet_endpoint(self->session),
            msxsession_fujinet_link(self->session) ? "connected" : "not connected");
        gtk_label_set_text(self->fuji_note, ext);
    } else if (!msxsession_fujinet_running(self->session))
        gtk_label_set_text(self->fuji_note,
                           "FujiNet is off (disabled, or its runtime did not start).");
    else
        gtk_label_set_text(self->fuji_note,
                           msxsession_fujinet_link(self->session)
                               ? "FujiNet is on, cartridge connected."
                               : "FujiNet is on, cartridge not connected.");
    msxdebug_fujinet_state(self->dbg, state, (int)sizeof state);
    {   /* unchanged keeps a selection */
        GtkTextBuffer *sbuf = gtk_text_view_get_buffer(self->fuji_state);
        GtkTextIter iter_start, iter_end;
        char *cur, *clean;
        gtk_text_buffer_get_bounds(sbuf, &iter_start, &iter_end);
        cur = gtk_text_buffer_get_text(sbuf, &iter_start, &iter_end, FALSE);
        clean = clean_text(state);
        if (strcmp(cur, clean) != 0) set_text(self->fuji_state, state);
        g_free(cur);
        g_free(clean);
    }

    if (!force && count == self->fuji_count && (count == 0 || pkt[count - 1].serial == self->fuji_last))
        return;
    self->fuji_count = count;
    self->fuji_last = count > 0 ? pkt[count - 1].serial : 0;
    log_note_reader(self->fuji_text);
    txt = g_string_new(NULL);
    for (idx = 0; idx < count; idx++) {
        int sub;
        g_string_append_printf(txt, "%s%s", idx ? "\n" : "", pkt[idx].text);
        if (pkt[idx].repeat > 1) g_string_append_printf(txt, " \xc3\x97%u", pkt[idx].repeat);
        g_string_append(txt, "\n     ");
        for (sub = 0; sub < pkt[idx].nbytes && sub < (int)sizeof pkt[idx].data; sub++)
            g_string_append_printf(txt, "%02X ", pkt[idx].data[sub]);
        if (pkt[idx].nbytes > (int)sizeof pkt[idx].data) g_string_append(txt, "\xe2\x80\xa6");
    }
    if (count == 0)
        g_string_append(txt, "(no FujiBus packets since the trace was cleared)");
    set_text_at(self->fuji_text, txt->str, force || following(self->fuji_text));
    g_string_free(txt, TRUE);
}

/* ---- Breakpoints and Watches ---------------------------------------------- */

static void bp_enable_toggled(GtkCheckButton *btn, gpointer user_data);
static void bp_remove_clicked(GtkButton *btn, gpointer user_data);

static void refresh_bps(DbgWin *self)
{
    static msxdebug_breakpoint bps[128];
    msxdebug_snapshot snap;
    GtkWidget *child;
    GString *sig = g_string_new(NULL);
    int count = msxdebug_breakpoint_list(self->dbg, bps, 128), idx;
    char text[200];

    /* why the machine stopped, in the core's words */
    msxdebug_stop_snapshot(self->dbg, &snap);
    if (snap.paused) {
        g_snprintf(text, sizeof text, "%s", snap.text[0] ? snap.text : "paused");
        text[0] = (char)g_ascii_toupper(text[0]);
    } else {
        g_strlcpy(text, "Running.", sizeof text);
    }
    gtk_label_set_text(self->bp_stop, text);

    /* the list is rebuilt only when what it shows changed (hit counts move
     * while the machine runs), so a click on a row is not pulled out from
     * under the pointer */
    for (idx = 0; idx < count; idx++)
        g_string_append_printf(sig, "%d/%d/%u-%u/%d/%u/%s/%d\n", bps[idx].id, bps[idx].type,
                               bps[idx].start, bps[idx].end, bps[idx].enabled, bps[idx].hits,
                               bps[idx].condition, snap.paused && snap.stop.bp_id == bps[idx].id);
    if (self->bp_sig && g_string_equal(sig, self->bp_sig)) {
        g_string_free(sig, TRUE);
        return;
    }
    if (self->bp_sig) g_string_free(self->bp_sig, TRUE);
    self->bp_sig = sig;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->bp_list))) != NULL)
        gtk_list_box_remove(self->bp_list, child);
    if (count == 0) {
        GtkWidget *lbl = gtk_label_new("No breakpoints. Select a disassembly line and press F9, "
                                     "or add one above.");
        gtk_widget_add_css_class(lbl, "dim-label");
        gtk_list_box_append(self->bp_list, lbl);
        return;
    }
    for (idx = 0; idx < count; idx++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *enable = gtk_check_button_new();
        GtkWidget *remove_btn = gtk_button_new_with_label("Remove");
        GtkWidget *lbl;
        char desc[240], line[300];
        msxbp_describe(self->dbg, &bps[idx], desc, sizeof desc);
        g_snprintf(line, sizeof line, "#%-3d %s    hits %u", bps[idx].id, desc, bps[idx].hits);
        {
            char *clean = clean_text(line);
            lbl = gtk_label_new(clean);
            g_free(clean);
        }
        gtk_label_set_xalign(GTK_LABEL(lbl), 0);
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_widget_add_css_class(lbl, "monospace");
        if (snap.paused && snap.stop.bp_id == bps[idx].id)   /* the one that stopped the machine */
            gtk_widget_add_css_class(lbl, "msx-accent-text");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(enable), bps[idx].enabled != 0);
        gtk_widget_set_tooltip_text(enable, "Enabled");
        g_object_set_data(G_OBJECT(enable), "bp", GINT_TO_POINTER(bps[idx].id));
        g_object_set_data(G_OBJECT(remove_btn), "bp", GINT_TO_POINTER(bps[idx].id));
        g_signal_connect(enable, "toggled", G_CALLBACK(bp_enable_toggled), self);
        g_signal_connect(remove_btn, "clicked", G_CALLBACK(bp_remove_clicked), self);
        gtk_widget_add_css_class(remove_btn, "flat");
        gtk_widget_set_tooltip_text(remove_btn, "Remove this breakpoint");
        gtk_widget_set_margin_start(row, 6);
        gtk_widget_set_margin_end(row, 6);
        gtk_box_append(GTK_BOX(row), enable);
        gtk_box_append(GTK_BOX(row), lbl);
        gtk_box_append(GTK_BOX(row), remove_btn);
        gtk_list_box_append(self->bp_list, row);
    }
}

static void on_wt_remove(GtkButton *btn, gpointer user_data);
static void on_wt_memory(GtkButton *btn, gpointer user_data);
static void on_wt_break(GtkButton *btn, gpointer user_data);

/* Rebuilt only when a line changed: a live value moving must not pull a
 * button out from under the pointer. */
static void refresh_watches(DbgWin *self)
{
    const int count = msxwatch_count();
    GString *sig = g_string_new(NULL);
    char lines[MSXWATCH_MAX][200];
    int idx;

    for (idx = 0; idx < count; idx++) {
        msxwatch_format(self->dbg, idx, lines[idx], sizeof lines[idx]);
        g_string_append_printf(sig, "%s\n", lines[idx]);
    }
    if (self->wt_sig && g_string_equal(sig, self->wt_sig)) {
        g_string_free(sig, TRUE);
        return;
    }
    if (self->wt_sig) g_string_free(self->wt_sig, TRUE);
    self->wt_sig = sig;

    gtk_list_box_remove_all(self->wt_list);
    if (count == 0) {
        GtkWidget *lbl = gtk_label_new("No watches. Type an address, label, expression or lo-hi above.");
        gtk_widget_add_css_class(lbl, "dim-label");
        gtk_list_box_append(self->wt_list, lbl);
        return;
    }
    for (idx = 0; idx < count; idx++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *label;
        static const struct { const char *text, *tip; GCallback handler; } btns[] = {
            { "Show in Memory", "Show it in the Memory tab", G_CALLBACK(on_wt_memory) },
            { "Break on Change", "Break when it changes", G_CALLBACK(on_wt_break) },
            { "Remove", "Forget this watch", G_CALLBACK(on_wt_remove) },
        };
        size_t sub;
        char *clean = clean_text(lines[idx]);
        label = gtk_label_new(clean);
        g_free(clean);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_add_css_class(label, "monospace");
        gtk_widget_set_margin_start(row, 6);
        gtk_widget_set_margin_end(row, 6);
        gtk_box_append(GTK_BOX(row), label);
        for (sub = 0; sub < G_N_ELEMENTS(btns); sub++) {
            GtkWidget *btn = gtk_button_new_with_label(btns[sub].text);
            gtk_widget_add_css_class(btn, "flat");
            gtk_widget_set_tooltip_text(btn, btns[sub].tip);
            g_object_set_data(G_OBJECT(btn), "wt-idx", GINT_TO_POINTER(idx));
            g_signal_connect(btn, "clicked", btns[sub].handler, self);
            gtk_box_append(GTK_BOX(row), btn);
        }
        gtk_list_box_append(self->wt_list, row);
    }
}

/* ---- Cart & I/O ------------------------------------------------------------ */

static void refresh_cart(DbgWin *self)
{
    static msxdebug_io ports;
    static msxdebug_cart cart;
    static msxdebug_region map[8];
    GString *txt = g_string_new(NULL);
    int count, idx, sub;

    msxdebug_cart_info(self->dbg, &cart);
    g_string_append_printf(txt, "Machine   %s (%s)\n", cart.machine[0] ? cart.machine : "none", cart.type);
    if (cart.z80_hz) g_string_append_printf(txt, "CPU       Z80 at %.3f MHz\n", cart.z80_hz / 1e6);
    g_string_append_printf(txt, "FujiNet   %s\n", !cart.fujinet ? "no cartridge in a slot"
                               : cart.fujinet_external ? (cart.fujinet_link ? "external, link up" : "external, link down")
                                                       : (cart.fujinet_link ? "cartridge, link up" : "cartridge, link down"));
    g_string_append_printf(txt, "Mapper    %s\n", cart.mapper ? "memory mapper present" : "none");
    g_string_append_printf(txt, "Sound     PSG%s%s%s\n", cart.scc ? ", SCC" : "", cart.music ? ", MSX-MUSIC" : "",
                           cart.audio ? ", MSX-AUDIO" : "");
    if (cart.devices[0]) g_string_append_printf(txt, "\nDevices\n  %s\n", cart.devices);
    g_string_append(txt, "\nMemory map (as the slots are selected now)\n");
    count = msxdebug_memory_map(self->dbg, map, 8);
    if (!count) g_string_append(txt, "  Stop the machine to read its memory map.\n");
    for (idx = 0; idx < count; idx++)
        g_string_append_printf(txt, "  %04X-%04X  %c%c  %s\n", map[idx].lo, map[idx].hi,
                               map[idx].readable ? 'r' : '-', map[idx].writable ? 'w' : '-', map[idx].name);
    set_text_at(self->cart_text, txt->str, FALSE);
    g_string_free(txt, TRUE);

    txt = g_string_new(NULL);
    if (!msxdebug_io_get(self->dbg, &ports) || !ports.valid) {
        g_string_append(txt, "The machine did not answer. Stop it to read the slots and ports.\n");
    } else {
        g_string_append(txt, "Slots (the primary slot selected for each 16K page)\n");
        for (idx = 0; idx < 4; idx++) {
            g_string_append_printf(txt, "  page %d  $%04X  slot %d", idx, idx * 0x4000, ports.page_slot[idx]);
            if (ports.page_subslot[idx] >= 0) g_string_append_printf(txt, "-%d", ports.page_subslot[idx]);
            g_string_append_printf(txt, "   %s\n", ports.page_device[idx][0] ? ports.page_device[idx] : "(empty)");
        }
        for (idx = 0; idx < 4; idx++)
            if (ports.expanded[idx]) {
                if (ports.subslot_reg[idx] >= 0)
                    g_string_append_printf(txt, "  slot %d is expanded, register $FFFF reads %02X\n", idx, ports.subslot_reg[idx]);
                else
                    g_string_append_printf(txt, "  slot %d is expanded\n", idx);
            }
        g_string_append_printf(txt, "\nPPI   $A8 slots %02X   $A9 keyboard %02X   $AA %02X   $AB control %02X\n",
                               ports.ppi[0], ports.ppi[1], ports.ppi[2], ports.ppi[3]);
        if (ports.mapper)
            g_string_append_printf(txt, "Mapper segments  $FC %02X  $FD %02X  $FE %02X  $FF %02X\n",
                                   ports.mapper_reg[0], ports.mapper_reg[1], ports.mapper_reg[2], ports.mapper_reg[3]);
        for (idx = 0; idx < ports.rom_mappers; idx++) {
            g_string_append_printf(txt, "ROM mapper  %s  blocks", ports.rom[idx].device);
            for (sub = 0; sub < 8; sub++) {
                if (ports.rom[idx].block[sub] == 255) g_string_append(txt, " --");
                else g_string_append_printf(txt, " %02X", ports.rom[idx].block[sub]);
            }
            g_string_append_c(txt, '\n');
        }
        g_string_append(txt, "\nKeyboard matrix (a 0 bit is a pressed key)\n");
        for (idx = 0; idx < ports.keymatrix_rows && idx < 16; idx++) {
            g_string_append_printf(txt, "  row %2d  %02X%s\n", idx, ports.keymatrix[idx], ports.keymatrix[idx] != 0xFF ? "  *" : "");
        }
        g_string_append_printf(txt, "\nJoystick ports  A %02X   B %02X   (a set bit is not pressed)\n",
                               ports.joyport[0], ports.joyport[1]);
        if (ports.slotmap[0]) g_string_append_printf(txt, "\nopenMSX slotmap\n%s\n", ports.slotmap);
    }
    set_text_at(self->io_text, txt->str, FALSE);
    g_string_free(txt, TRUE);
}

/* ---- Sound ---------------------------------------------------------------- */

static void bar_of(char *bar, int level, int max)
{
    int sub;
    for (sub = 0; sub < max; sub++) bar[sub] = sub < level ? '#' : '.';
    bar[max] = '\0';
}

static void refresh_sound(DbgWin *self)
{
    static msxdebug_sound snd;
    static const char voices[] = "ABC";
    GString *txt = g_string_new(NULL);
    int voice, sub, any = 0;

    if (!msxdebug_is_stopped(self->dbg)) {
        set_text_at(self->sound_text, "Stop the machine to read the sound chips.\n", FALSE);
        g_string_free(txt, TRUE);
        return;
    }
    msxdebug_sound_get(self->dbg, &snd);
    if (snd.psg.present) {
        any = 1;
        g_string_append(txt, "PSG (AY-3-8910 on ports $A0-$A2)\n");
        for (voice = 0; voice < 3; voice++) {
            char bar[17];
            bar_of(bar, snd.psg.level[voice], 16);
            g_string_append_printf(txt, "  %c  period %03X  %8.1f Hz  %-4s%-6s  level %X [%s]%s\n", voices[voice],
                                   snd.psg.period[voice], snd.psg.freq[voice],
                                   snd.psg.tone_on[voice] ? "tone" : "----", snd.psg.noise_on[voice] ? "+noise" : "",
                                   snd.psg.level[voice], bar, snd.psg.envelope[voice] ? "  (envelope)" : "");
        }
        g_string_append_printf(txt, "  noise period %02X   envelope period %04X   shape %X\n  registers:",
                               snd.psg.noise_period, snd.psg.env_period, snd.psg.env_shape);
        for (sub = 0; sub < 16; sub++) g_string_append_printf(txt, " %02X", snd.psg.reg[sub]);
        g_string_append(txt, "\n\n");
    }
    if (snd.scc.present) {
        any = 1;
        g_string_append_printf(txt, "SCC (%s)\n", snd.scc.name);
        for (voice = 0; voice < 5; voice++) {
            char bar[17];
            bar_of(bar, snd.scc.volume[voice], 16);
            g_string_append_printf(txt, "  %d  period %03X  %8.1f Hz  %-3s  volume %X [%s]\n", voice + 1,
                                   snd.scc.period[voice], snd.scc.freq[voice], snd.scc.enabled[voice] ? "on" : "off",
                                   snd.scc.volume[voice], bar);
        }
        for (voice = 0; voice < 5; voice++) {
            g_string_append_printf(txt, "  wave %d:", voice + 1);
            for (sub = 0; sub < 32; sub++) g_string_append_printf(txt, " %02X", snd.scc.wave[voice][sub]);
            g_string_append_c(txt, '\n');
        }
        g_string_append_c(txt, '\n');
    }
    if (snd.opll.present) {
        any = 1;
        g_string_append_printf(txt, "MSX-MUSIC (%s)%s\n", snd.opll.name, snd.opll.rhythm ? "  rhythm mode" : "");
        for (voice = 0; voice < 9; voice++)
            g_string_append_printf(txt, "  %d  fnum %03X  block %d  %8.1f Hz  %-3s  instrument %X  volume %X\n", voice + 1,
                                   snd.opll.fnum[voice], snd.opll.block[voice], snd.opll.freq[voice],
                                   snd.opll.key_on[voice] ? "on" : "off", snd.opll.instrument[voice],
                                   snd.opll.volume[voice]);
        g_string_append(txt, "  registers:");
        for (sub = 0; sub < 0x40; sub++) {
            if (sub % 16 == 0) g_string_append_printf(txt, "\n   %02X:", sub);
            g_string_append_printf(txt, " %02X", snd.opll.reg[sub]);
        }
        g_string_append(txt, "\n\n");
    }
    if (snd.y8950.present) {
        any = 1;
        g_string_append_printf(txt, "MSX-AUDIO (%s)\n  registers:", snd.y8950.name);
        for (sub = 0; sub < 256; sub++) {
            if (sub % 16 == 0) g_string_append_printf(txt, "\n   %02X:", sub);
            g_string_append_printf(txt, " %02X", snd.y8950.reg[sub]);
        }
        g_string_append(txt, "\n");
    }
    if (!any) g_string_append(txt, "No sound chip answered.\n");
    set_text_at(self->sound_text, txt->str, FALSE);
    g_string_free(txt, TRUE);
}

/* ---- Trace and History ------------------------------------------------------ */

static void hist_refresh(DbgWin *self, int idx, gboolean force)
{
    static msxdebug_hist buf[HIST_COPY];
    const gboolean live = idx == 0;
    const gboolean paused = msxdebug_is_stopped(self->dbg) != 0;
    const gboolean active = msxdebug_history_enabled(self->dbg) != 0;
    const uint64_t count = msxdebug_history_count(self->dbg);
    uint16_t range_lo = 0, range_hi = 0xFFFF;
    int filter = MSXDEBUG_TRACE_ALL, total, pos, shown = 0;
    g_autofree char *note = NULL;
    GString *txt;
    int hit[HIST_SHOWN];

    self->h_sync = TRUE;   /* the two Record buttons follow the engine */
    gtk_toggle_button_set_active(self->h_rec[0], active);
    gtk_toggle_button_set_active(self->h_rec[1], active);
    self->h_sync = FALSE;

    if (!force && count == self->h_seen[idx] && (live || paused))
        return;
    self->h_seen[idx] = count;
    if (!live && !paused) {
        gtk_label_set_text(self->h_note[idx], "Stop the machine to read its history.");
        return;
    }
    switch (gtk_drop_down_get_selected(self->h_filter[idx])) {
    case 1: filter = MSXDEBUG_TRACE_BRANCHES; break;
    case 2:
        if (msxsym_parse_range(self->dbg, gtk_editable_get_text(GTK_EDITABLE(self->h_range[idx])), &range_lo, &range_hi))
            filter = MSXDEBUG_TRACE_RANGE;
        break;
    default: break;
    }
    total = msxdebug_history_copy(self->dbg, buf, HIST_COPY);
    for (pos = total - 1; pos >= 0 && shown < HIST_SHOWN; pos--)   /* the newest matches, scanning back */
        if (msxdebug_history_match(&buf[pos], filter, range_lo, range_hi))
            hit[shown++] = pos;
    txt = g_string_new(NULL);
    if (!active && total == 0)
        g_string_append_printf(txt, "%s is off. Press Record, then let the machine run.", live ? "Trace" : "History");
    for (pos = shown - 1; pos >= 0; pos--) {   /* oldest first */
        char line[256];
        msxdebug_history_format(&buf[hit[pos]], line, sizeof line);
        g_string_append(txt, line);
        g_string_append_c(txt, '\n');
    }
    set_text_at(self->h_text[idx], txt->str, TRUE);
    g_string_free(txt, TRUE);
    note = g_strdup_printf("%s%" G_GUINT64_FORMAT " instructions recorded, last %d shown",
                           active ? "Recording. " : "", count, shown);
    gtk_label_set_text(self->h_note[idx], note);
}

/* ---- Symbols -------------------------------------------------------------- */

static void on_sym_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);

/* The list is rebuilt only when the table (its size, or a command since) or the
 * filter changed -- a real game .sym can be a few hundred entries. */
static void refresh_symbols(DbgWin *self, gboolean force)
{
    static int idx[4096];
    const char *filter = gtk_editable_get_text(GTK_EDITABLE(self->sym_filter));
    const int total = msxdebug_symbols_count(self->dbg);
    const unsigned gen = msxdebug_generation(self->dbg);
    int count, pos;
    char text[128];

    if (!force && total == self->sym_built && gen == self->sym_gen && strcmp(filter, self->sym_text) == 0)
        return;
    self->sym_built = total;
    self->sym_gen = gen;
    g_strlcpy(self->sym_text, filter, sizeof self->sym_text);

    gtk_list_box_remove_all(self->symlist);
    count = msxdebug_symbols_filter(self->dbg, filter, idx, (int)G_N_ELEMENTS(idx));
    for (pos = 0; pos < count; pos++) {
        uint16_t addr;
        char name[96], line[160];
        GtkWidget *row, *label;
        if (!msxdebug_symbols_get(self->dbg, idx[pos], &addr, name, sizeof name)) continue;
        g_snprintf(line, sizeof line, "%04X  %s", addr, name);
        row = gtk_list_box_row_new();
        label = gtk_label_new(line);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_widget_add_css_class(label, "monospace");
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data(G_OBJECT(row), "addr", GUINT_TO_POINTER(addr));
        gtk_list_box_append(self->symlist, row);
    }
    g_snprintf(text, sizeof text, "%d of %d symbol%s%s", count, total, total == 1 ? "" : "s",
               filter[0] ? " (filtered)" : "");
    gtk_label_set_text(self->sym_count, text);
}

/* ---- status and the tab showing ------------------------------------------- */

static void refresh_status(DbgWin *self)
{
    msxdebug_snapshot snap;
    char text[256];
    gboolean stopped;
    msxdebug_stop_snapshot(self->dbg, &snap);
    stopped = snap.paused != 0;
    if (stopped)
        g_snprintf(text, sizeof text, "Stopped%s%s", snap.text[0] ? ": " : "", snap.text);
    else
        g_snprintf(text, sizeof text, "Running");
    adw_window_title_set_subtitle(self->title, text);
    gtk_button_set_label(self->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    if (stopped) gtk_widget_add_css_class(GTK_WIDGET(self->run_btn), "msx-accent");
    else gtk_widget_remove_css_class(GTK_WIDGET(self->run_btn), "msx-accent");
}

/* One tab brought up to date; the others catch up when their tab is shown. */
static void refresh_tab(DbgWin *self, int tab)
{
    switch (tab) {
    case TAB_DISASM: refresh_disasm(self); break;
    case TAB_MEMORY: refresh_mem(self); break;
    case TAB_BREAKS: refresh_bps(self); break;
    case TAB_WATCHES: refresh_watches(self); break;
    case TAB_CART: refresh_cart(self); break;
    case TAB_TRACE: hist_refresh(self, 0, FALSE); break;
    case TAB_HISTORY: hist_refresh(self, 1, FALSE); break;
    case TAB_VIDEO: dbg_video_refresh(self->pages[TAB_VIDEO], FALSE); break;
    case TAB_SOUND: refresh_sound(self); break;
    case TAB_SYMBOLS: refresh_symbols(self, FALSE); break;
    case TAB_FUJINET: refresh_fujinet(self, FALSE); break;
    default: break;
    }
}

/* The status, the register strip, and every tab on screen: the one showing and
 * any in a window of its own. */
static void refresh_now(DbgWin *self)
{
    msxdebug_snapshot snap;
    int tab;
    msxdebug_stop_snapshot(self->dbg, &snap);
    self->seen_generation = msxdebug_generation(self->dbg);
    self->seen_serial = snap.serial;
    self->was_stopped = snap.paused != 0;
    refresh_status(self);
    refresh_regs(self);
    for (tab = 0; tab < TAB_COUNT; tab++)
        if (tab_visible(self, tab))
            refresh_tab(self, tab);
}

/* ---- the timer ------------------------------------------------------------ */

static void attach_and_refresh(DbgWin *self);

static gboolean tick_refresh(gpointer user_data)
{
    DbgWin *self = user_data;
    msxdebug_snapshot snap;
    gboolean stopped;
    if (self->torn_down || !gtk_widget_get_visible(GTK_WIDGET(self->win))) return G_SOURCE_CONTINUE;
    if (!msxdebug_is_attached(self->dbg)) {
        attach_and_refresh(self);
        return G_SOURCE_CONTINUE;
    }
    msxdebug_stop_snapshot(self->dbg, &snap);
    stopped = snap.paused != 0;
    if (msxdebug_generation(self->dbg) != self->seen_generation || stopped != self->was_stopped ||
        snap.serial != self->seen_serial) {
        self->running_ticks = 0;
        refresh_now(self);
    } else if (!stopped) {
        /* live values twice a second while the machine runs; the Video tab
         * follows the frames the machine publishes, every tick */
        if (++self->running_ticks >= LIVE_TICKS) {
            self->running_ticks = 0;
            refresh_now(self);
        } else if (tab_visible(self, TAB_VIDEO)) {
            dbg_video_refresh(self->pages[TAB_VIDEO], FALSE);
        }
    }
    return G_SOURCE_CONTINUE;
}

/* ---- handlers ------------------------------------------------------------- */

static void on_run(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)btn;
    if (msxdebug_is_stopped(self->dbg)) {
        msxdebug_resume(self->dbg);
    } else {
        msxdebug_stop(self->dbg);
        settle(self, TRUE);
    }
    refresh_now(self);
}

/* A step: the machine is stopped, runs a moment, stops again. */
#define STEP_HANDLER(name, call) \
    static void name(GtkButton *btn, gpointer user_data) \
    { \
        DbgWin *self = user_data; \
        (void)btn; \
        call; \
        settle(self, TRUE); \
        refresh_now(self); \
    }
STEP_HANDLER(on_step, msxdebug_step(self->dbg))
STEP_HANDLER(on_step_over, msxdebug_step_over(self->dbg))
STEP_HANDLER(on_step_out, msxdebug_step_out(self->dbg))
STEP_HANDLER(on_frame, msxdebug_frame(self->dbg))
#undef STEP_HANDLER

static void on_run_to_cursor(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)btn;
    if (self->selected < 0) {
        toast(self, "Select a disassembly line to run to");
        return;
    }
    msxdebug_run_to(self->dbg, (uint16_t)self->selected);
    settle(self, TRUE);
    refresh_now(self);
}

/* F9 and the gutter: an execute breakpoint on the selected line. */
static void toggle_selected(DbgWin *self)
{
    if (self->selected < 0) {
        toast(self, "Select a disassembly line first");
        return;
    }
    msxdebug_breakpoint_toggle(self->dbg, (uint16_t)self->selected);
    refresh_now(self);
}

static void on_console(GtkEntry *entry, gpointer user_data)
{
    DbgWin *self = user_data;
    static char out[65536];
    const char *cmd = gtk_editable_get_text(GTK_EDITABLE(entry));
    char line[512];
    gboolean was_stopped = msxdebug_is_stopped(self->dbg) != 0;
    if (!cmd || !*cmd) return;
    /* the history keeps each command once in a row */
    if (self->history->len == 0 ||
        strcmp(g_ptr_array_index(self->history, self->history->len - 1), cmd) != 0)
        g_ptr_array_add(self->history, g_strdup(cmd));
    self->history_pos = self->history->len;
    g_snprintf(line, sizeof line, "> %s\n", cmd);
    append_text(self->console_out, line, TRUE);
    out[0] = '\0';
    msxdebug_command(self->dbg, cmd, out, sizeof out);
    append_text(self->console_out, out, TRUE);
    if (out[0] && out[strlen(out) - 1] != '\n')
        append_text(self->console_out, "\n", TRUE);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    if (was_stopped) settle(self, TRUE);
    refresh_now(self);
}

static void history_show(DbgWin *self)
{
    const char *text = self->history_pos < self->history->len
                           ? g_ptr_array_index(self->history, self->history_pos) : "";
    gtk_editable_set_text(GTK_EDITABLE(self->console_in), text);
    gtk_editable_set_position(GTK_EDITABLE(self->console_in), -1);
}

/* Tab completes the current word against the commands, the registers, the
 * labels and openMSX's own console; several matches are listed, with their
 * common start filled in. Up and Down walk the command history. */
static gboolean on_console_key(GtkEventControllerKey *ctl, guint keyval, guint code,
                               GdkModifierType mods, gpointer user_data)
{
    DbgWin *self = user_data;
    char comps[4096];
    const char *text, *word;
    int count;
    (void)ctl; (void)code; (void)mods;
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) {
        if (self->history_pos > 0) {
            self->history_pos--;
            history_show(self);
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
        if (self->history_pos < self->history->len) {
            self->history_pos++;
            history_show(self);
        }
        return TRUE;
    }
    if (keyval != GDK_KEY_Tab) return FALSE;
    text = gtk_editable_get_text(GTK_EDITABLE(self->console_in));
    comps[0] = '\0';
    /* the engine completes the end of the whole line (a command, then its
     * arguments), so the whole line goes and the last word is replaced */
    count = msxdebug_completions(self->dbg, text, comps, sizeof comps);
    word = strrchr(text, ' ');
    word = word ? word + 1 : text;
    if (count == 1) {
        char merged[600];
        char *eol = strchr(comps, '\n');
        if (eol) *eol = '\0';
        g_snprintf(merged, sizeof merged, "%.*s%s ", (int)(word - text), text, comps);
        gtk_editable_set_text(GTK_EDITABLE(self->console_in), merged);
        gtk_editable_set_position(GTK_EDITABLE(self->console_in), -1);
    } else if (count > 1) {
        char **cand = g_strsplit(comps, "\n", -1);
        size_t common = strlen(cand[0]), len = strlen(word);
        int idx;
        for (idx = 1; cand[idx] && *cand[idx]; idx++) {
            size_t sub = 0;
            while (sub < common && cand[idx][sub] == cand[0][sub]) sub++;
            common = sub;
        }
        if (common > len) {
            char merged[600];
            g_snprintf(merged, sizeof merged, "%.*s%.*s", (int)(word - text), text, (int)common, cand[0]);
            gtk_editable_set_text(GTK_EDITABLE(self->console_in), merged);
            gtk_editable_set_position(GTK_EDITABLE(self->console_in), -1);
        }
        g_strfreev(cand);
        append_text(self->console_out, comps, FALSE);
        if (comps[0] && comps[strlen(comps) - 1] != '\n')
            append_text(self->console_out, "\n", FALSE);
    }
    return TRUE;
}

/* ---- Memory ---------------------------------------------------------------- */

static void mem_set_base(DbgWin *self, int base)
{
    if (base < 0) base = 0;
    if (base > 0x10000 - MEM_PAGE) base = 0x10000 - MEM_PAGE;
    self->mem_base = base & ~0xF;
    refresh_mem(self);
}

/* Shows a range in the Memory tab, marked, with a little context above it. */
static void show_in_memory(DbgWin *self, int range_lo, int range_hi)
{
    self->mem_hl = TRUE;
    self->mem_hl_lo = range_lo;
    self->mem_hl_hi = range_hi;
    mem_set_base(self, (range_lo & ~0xF) - 4 * 16);
}

/* Go to: the page holding an address, a label, an expression or a range, marked. */
static void on_mem_goto(GtkEntry *entry, gpointer user_data)
{
    DbgWin *self = user_data;
    uint16_t range_lo, range_hi;
    if (!msxsym_parse_range(self->dbg, gtk_editable_get_text(GTK_EDITABLE(entry)), &range_lo, &range_hi)) {
        toast(self, "Not an address, a label or an expression");
        return;
    }
    show_in_memory(self, range_lo, range_hi);
}

static void on_mem_page(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    mem_set_base(self, self->mem_base + GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "delta")) * MEM_PAGE);
}

/* The wheel scrolls by rows. */
static gboolean on_mem_scroll(GtkEventControllerScroll *ctl, double delta_x, double delta_y, gpointer user_data)
{
    DbgWin *self = user_data;
    int rows = (int)(delta_y * 3);
    (void)ctl; (void)delta_x;
    if (!rows) rows = delta_y > 0 ? 1 : -1;
    mem_set_base(self, self->mem_base + rows * 16);
    return TRUE;
}

/* "address value...": bytes written from the address on, through the slot
 * map. RAM takes them; ROM ignores them, and the status says so. */
static void on_mem_write(GtkEntry *entry, gpointer user_data)
{
    DbgWin *self = user_data;
    char **parts = g_strsplit_set(gtk_editable_get_text(GTK_EDITABLE(entry)), " \t:=,", -1);
    uint8_t bytes[POKE_MAX], back[POKE_MAX];
    int addr = 0, count = 0, idx;
    gboolean have_addr = FALSE, valid = TRUE;

    for (idx = 0; parts[idx] && valid; idx++) {
        long val;
        if (!*parts[idx]) continue;
        if (!have_addr) {
            valid = parse_addr(self, parts[idx], &addr);
            have_addr = TRUE;
        } else if (count < POKE_MAX && parse_value(self, parts[idx], &val) && val >= -128 && val <= 255) {
            bytes[count++] = (uint8_t)val;
        } else {
            valid = FALSE;
        }
    }
    g_strfreev(parts);
    if (!valid || !have_addr || count == 0) {
        toast(self, "Poke takes an address and byte values, e.g. C000 41 42");
        return;
    }
    if (!msxdebug_is_stopped(self->dbg)) {
        toast(self, "Stop the machine to poke its memory");
        return;
    }
    for (idx = 0; idx < count; idx++)
        msxdebug_write(self->dbg, (uint16_t)(addr + idx), bytes[idx]);
    memset(back, 0, sizeof back);
    msxdebug_read(self->dbg, (uint16_t)addr, back, count);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    show_in_memory(self, addr, addr + count - 1);
    if (memcmp(back, bytes, (size_t)count) != 0)
        toast(self, "Some bytes did not change: that memory is ROM or empty");
}

static void on_search(GtkButton *button, gpointer user_data)
{
    DbgWin *self = user_data;
    const int backward = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "back"));
    uint8_t pat[MSXSEARCH_MAX], mask;
    uint16_t found;
    int flags, count, from, got;

    if (!msxsearch_parse((int)gtk_drop_down_get_selected(self->srch_kind),
                         gtk_editable_get_text(GTK_EDITABLE(self->srch_entry)), pat, &count, &mask, &flags)) {
        gtk_label_set_text(self->srch_note, "Type hex bytes (3E 01) or some text.");
        return;
    }
    if (!msxdebug_is_stopped(self->dbg)) {
        gtk_label_set_text(self->srch_note, "Stop the machine to search its memory.");
        return;
    }
    from = self->mem_hl ? (backward ? self->mem_hl_lo : self->mem_hl_lo + 1) : self->mem_base;
    got = msxdebug_search_next(self->dbg, pat, count, mask, flags, (uint16_t)(from & 0xFFFF), backward, &found);
    if (got > 0) {
        g_autofree char *msg = g_strdup_printf("Found at $%04X", found);
        gtk_label_set_text(self->srch_note, msg);
        show_in_memory(self, found, found + count - 1);
    } else {
        gtk_label_set_text(self->srch_note, got == 0 ? "Not found." : "Stop the machine to search its memory.");
    }
}

static void on_search_entry_activate(GtkEntry *entry, gpointer user_data)
{
    GtkWidget *fwd = g_object_get_data(G_OBJECT(entry), "next-button");
    on_search(GTK_BUTTON(fwd), user_data);
}

/* ---- Disassembly ----------------------------------------------------------- */

/* Click selects a row; a click on the gutter or a double-click toggles its
 * breakpoint (a label line has no address, so it selects nothing). */
static void on_disasm_click(GtkGestureClick *gesture, int clicks, double pos_x, double pos_y, gpointer user_data)
{
    DbgWin *self = user_data;
    GtkTextIter iter;
    int buf_x, buf_y, line;
    gboolean gutter;
    (void)gesture;
    gtk_text_view_window_to_buffer_coords(self->disasm, GTK_TEXT_WINDOW_WIDGET, (int)pos_x, (int)pos_y, &buf_x, &buf_y);
    if (!gtk_text_view_get_iter_at_location(self->disasm, &iter, buf_x, buf_y)) return;
    line = gtk_text_iter_get_line(&iter);
    if (line < 0 || line >= self->line_count || self->line_addr[line] < 0) return;
    gutter = gtk_text_iter_get_line_offset(&iter) < 2;
    self->selected = self->line_addr[line];
    if ((clicks == 1 && gutter) || (clicks == 2 && !gutter))
        msxdebug_breakpoint_toggle(self->dbg, (uint16_t)self->selected);
    refresh_disasm(self);
}

static void on_follow_toggled(GtkCheckButton *btn, gpointer user_data)
{
    (void)btn;
    refresh_disasm(user_data);
}

static void on_jump(GtkEntry *entry, gpointer user_data)
{
    DbgWin *self = user_data;
    int addr;
    if (!parse_addr(self, gtk_editable_get_text(GTK_EDITABLE(entry)), &addr)) {
        toast(self, "Not an address, a label or an expression");
        return;
    }
    gtk_check_button_set_active(self->follow_pc, FALSE);
    self->disasm_top = addr;
    refresh_disasm(self);
}

/* The wheel scrolls by instruction. */
static gboolean on_disasm_scroll(GtkEventControllerScroll *ctl, double delta_x, double delta_y, gpointer user_data)
{
    DbgWin *self = user_data;
    int rows = (int)(delta_y * 3);
    (void)ctl; (void)delta_x;
    if (!rows) rows = delta_y > 0 ? 1 : -1;
    gtk_check_button_set_active(self->follow_pc, FALSE);
    self->disasm_top = msxdebug_row_address(self->dbg, (uint16_t)self->disasm_top, rows);
    refresh_disasm(self);
    return TRUE;
}

/* ---- Breakpoints ----------------------------------------------------------- */

static void bp_enable_toggled(GtkCheckButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    msxdebug_breakpoint_enable(self->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bp")),
                               gtk_check_button_get_active(btn));
    refresh_now(self);
}

static void bp_remove_clicked(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    msxdebug_breakpoint_remove(self->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bp")));
    refresh_now(self);
}

static void show_bp_msg(DbgWin *self, const char *msg)
{
    gtk_label_set_text(self->bp_msg, msg);
    gtk_widget_set_visible(GTK_WIDGET(self->bp_msg), TRUE);
}

static void on_bp_add(GtkWidget *wgt, gpointer user_data)
{
    DbgWin *self = user_data;
    static const int types[6] = { MSXDEBUG_BP_EXEC, MSXDEBUG_BP_READ, MSXDEBUG_BP_WRITE,
                                  MSXDEBUG_BP_ACCESS, MSXDEBUG_BP_IN, MSXDEBUG_BP_OUT };
    guint sel = gtk_drop_down_get_selected(self->bp_type);
    uint16_t range_lo, range_hi;
    int bpid;
    char err[256] = "";
    const char *text = gtk_editable_get_text(GTK_EDITABLE(self->bp_addr));
    char cond[128];
    (void)wgt;
    if (sel >= G_N_ELEMENTS(types)) sel = 0;
    if (!msxsym_parse_range(self->dbg, text, &range_lo, &range_hi)) {
        show_bp_msg(self, "Not an address, a label, an expression or a range (lo-hi)");
        return;
    }
    if (types[sel] == MSXDEBUG_BP_EXEC) range_hi = range_lo;
    if ((types[sel] == MSXDEBUG_BP_IN || types[sel] == MSXDEBUG_BP_OUT) && range_hi > 0xFF) {
        show_bp_msg(self, "A port is 0-255");
        return;
    }
    g_strlcpy(cond, gtk_editable_get_text(GTK_EDITABLE(self->bp_cond)), sizeof cond);
    g_strstrip(cond);
    bpid = msxdebug_breakpoint_add(self->dbg, types[sel], range_lo, range_hi, cond, err, sizeof err);
    if (bpid < 0) {
        char *clean = clean_text(err[0] ? err : "The breakpoint was refused");
        show_bp_msg(self, clean);
        g_free(clean);
        return;
    }
    gtk_widget_set_visible(GTK_WIDGET(self->bp_msg), FALSE);
    gtk_editable_set_text(GTK_EDITABLE(self->bp_addr), "");
    gtk_editable_set_text(GTK_EDITABLE(self->bp_cond), "");
    refresh_now(self);
}

static void on_bp_clear_hits(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)btn;
    msxdebug_breakpoint_clear_hits(self->dbg);
    refresh_now(self);
}

static void on_bp_clear(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)btn;
    msxdebug_breakpoint_clear(self->dbg);
    refresh_now(self);
}

/* ---- Watches ---------------------------------------------------------------- */

static void on_wt_remove(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    msxwatch_remove(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "wt-idx")));
    refresh_now(self);
}

static void on_wt_memory(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    uint16_t range_lo, range_hi;
    if (msxwatch_range(self->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "wt-idx")), &range_lo, &range_hi)) {
        show_tab(self, TAB_MEMORY);
        show_in_memory(self, range_lo, range_hi);
    } else {
        toast(self, "That watch does not resolve to an address yet");
    }
}

static gboolean bp_exists(DbgWin *self, int type, uint16_t range_lo, uint16_t range_hi, const char *cond)
{
    static msxdebug_breakpoint have[128];
    int count = msxdebug_breakpoint_list(self->dbg, have, 128), idx;
    for (idx = 0; idx < count; idx++)
        if (have[idx].type == type && have[idx].start == range_lo && have[idx].end == range_hi &&
            strcmp(have[idx].condition, cond) == 0)
            return TRUE;
    return FALSE;
}

static gboolean break_on_change(DbgWin *self, uint16_t range_lo, uint16_t range_hi)
{
    char err[160] = "";
    if (bp_exists(self, MSXDEBUG_BP_WRITE, range_lo, range_hi, "value != old")) return TRUE;
    return msxdebug_breakpoint_add(self->dbg, MSXDEBUG_BP_WRITE, range_lo, range_hi, "value != old", err, sizeof err) >= 0;
}

static void on_wt_break(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    uint16_t range_lo, range_hi;
    if (!msxwatch_range(self->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "wt-idx")), &range_lo, &range_hi)) {
        toast(self, "That watch does not resolve to an address yet");
        return;
    }
    if (break_on_change(self, range_lo, range_hi)) toast(self, "Breaking when it changes");
    else toast(self, "The breakpoint was refused");
    refresh_now(self);
}

static void on_wt_add(GtkButton *button, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)button;
    if (msxwatch_add(gtk_editable_get_text(GTK_EDITABLE(self->wt_entry))) < 0) {
        toast(self, "Type an address, label, expression or lo-hi");
        return;
    }
    gtk_editable_set_text(GTK_EDITABLE(self->wt_entry), "");
    refresh_now(self);
}

static void on_wt_entry_activate(GtkEntry *entry, gpointer user_data)
{
    (void)entry;
    on_wt_add(NULL, user_data);
}

/* ---- Trace and History ------------------------------------------------------ */

static void on_hist_rec(GtkToggleButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    if (self->h_sync) return;
    msxdebug_history_enable(self->dbg, gtk_toggle_button_get_active(btn));
    refresh_now(self);
}

static void on_hist_clear(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)btn;
    msxdebug_history_clear(self->dbg);
    self->h_seen[0] = self->h_seen[1] = (uint64_t)-1;
    refresh_now(self);
}

static void on_hist_changed(GObject *obj, GParamSpec *spec, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)obj; (void)spec;
    self->h_seen[0] = self->h_seen[1] = (uint64_t)-1;
    refresh_now(self);
}

/* ---- Symbols ---------------------------------------------------------------- */

static void on_sym_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    DbgWin *self = user_data;
    int addr = (int)GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "addr"));
    (void)box;
    show_tab(self, TAB_MEMORY);
    show_in_memory(self, addr, addr);
}

static void on_sym_filter_changed(GtkEditable *entry, gpointer user_data)
{
    (void)entry;
    refresh_symbols(user_data, FALSE);
}

static void popdown_of(GtkWidget *btn)
{
    GtkWidget *pop = gtk_widget_get_ancestor(btn, GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

static void on_symbols_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    GWeakRef *ref = user_data;
    g_autoptr(GObject) wobj = g_weak_ref_get(ref);
    DbgWin *self = wobj ? g_object_get_data(wobj, "dbgwin") : NULL;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    char msg[512] = "";
    g_weak_ref_clear(ref);
    g_free(ref);
    /* the debugger may have been closed while the picker was up */
    if (!self || self->torn_down || !file) return;
    path = g_file_get_path(file);
    if (!path) return;
    msxdebug_load_symbols(self->dbg, path, msg, sizeof msg);
    notice(self, msg);
    refresh_symbols(self, TRUE);
}

static void on_symbols(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *syms = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    GWeakRef *ref = g_new0(GWeakRef, 1);

    popdown_of(GTK_WIDGET(btn));
    gtk_file_filter_set_name(syms, "Symbol files (*.sym, *.txt)");
    gtk_file_filter_add_suffix(syms, "sym");
    gtk_file_filter_add_suffix(syms, "txt");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, syms);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, "Load Symbols (HHHH NAME per line)");
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    g_weak_ref_init(ref, self->win);
    gtk_file_dialog_open(dlg, self->win, NULL, on_symbols_chosen, ref);
    g_object_unref(syms);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dlg);
}

static void on_clear_symbols(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    popdown_of(GTK_WIDGET(btn));
    msxdebug_clear_symbols(self->dbg);
    notice(self, "Loaded symbols and labels dropped; the built-in BIOS names stay");
    refresh_symbols(self, TRUE);
}

static void on_save_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    GWeakRef *ref = user_data;
    g_autoptr(GObject) wobj = g_weak_ref_get(ref);
    DbgWin *self = wobj ? g_object_get_data(wobj, "dbgwin") : NULL;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    const char *kind = g_object_get_data(src, "kind");
    char msg[512] = "";
    g_weak_ref_clear(ref);
    g_free(ref);
    if (!self || self->torn_down || !file) return;
    path = g_file_get_path(file);
    if (!path) return;
    msxdebug_save(self->dbg, kind, path, msg, sizeof msg);
    notice(self, msg);
}

static void on_save(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    const char *kind = g_object_get_data(G_OBJECT(btn), "kind");
    char title[64];
    GWeakRef *ref = g_new0(GWeakRef, 1);
    popdown_of(GTK_WIDGET(btn));
    g_snprintf(title, sizeof title, "Save %s", (const char *)g_object_get_data(G_OBJECT(btn), "title"));
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_initial_name(dlg, g_object_get_data(G_OBJECT(btn), "file"));
    g_object_set_data(G_OBJECT(dlg), "kind", (gpointer)kind);
    g_weak_ref_init(ref, self->win);
    gtk_file_dialog_save(dlg, self->win, NULL, on_save_chosen, ref);
    g_object_unref(dlg);
}

static void on_fuji_clear(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)btn;
    msxdebug_fujinet_trace_clear(self->dbg);
    refresh_fujinet(self, TRUE);
}

/* ---- tabs in windows of their own ---------------------------------------------- */

static gboolean on_key(GtkEventControllerKey *ctl, guint keyval, guint code,
                       GdkModifierType mods, gpointer user_data);

static void dock_tab(DbgWin *self, int tab, gboolean destroy)
{
    GtkWindow *win = self->detached[tab];
    GtkWidget *page = self->pages[tab], *vbox;
    int pos = 0, txt;

    if (!win) return;
    vbox = g_object_get_data(G_OBJECT(win), "vbox");
    g_object_ref(page);
    gtk_box_remove(GTK_BOX(vbox), page);
    self->detached[tab] = NULL;
    for (txt = 0; txt < tab; txt++)   /* back where it was in the order */
        if (!self->detached[txt]) pos++;
    gtk_notebook_insert_page(self->notebook, page, gtk_label_new(k_tab_names[tab]), pos);
    g_object_unref(page);
    gtk_notebook_set_current_page(self->notebook, pos);
    if (destroy) gtk_window_destroy(win);
    refresh_now(self);
}

static void on_dock_button(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    dock_tab(self, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "tab")), TRUE);
}

static gboolean on_detached_close(GtkWindow *win, gpointer user_data)
{
    DbgWin *self = user_data;
    dock_tab(self, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(win), "tab")), FALSE);
    return FALSE;   /* let it close */
}

static void pop_out_tab(DbgWin *self, int tab)
{
    GtkWidget *page, *vbox, *header, *dock;
    GtkWindow *win;
    GtkEventController *keys;
    g_autofree char *title = NULL;

    if (tab < 0 || tab >= TAB_COUNT || self->detached[tab] || gtk_notebook_get_n_pages(self->notebook) <= 1)
        return;   /* one tab always stays */
    page = self->pages[tab];
    g_object_ref(page);
    gtk_notebook_remove_page(self->notebook, gtk_notebook_page_num(self->notebook, page));

    win = GTK_WINDOW(adw_window_new());
    title = g_strdup_printf("MSX Debugger \xe2\x80\x93 %s", k_tab_names[tab]);
    gtk_window_set_title(win, title);
    gtk_window_set_default_size(win, 760, 520);
    header = adw_header_bar_new();
    dock = gtk_button_new_with_label("Dock");
    gtk_widget_set_tooltip_text(dock, "Put this tab back in the debugger");
    g_object_set_data(G_OBJECT(dock), "tab", GINT_TO_POINTER(tab));
    g_signal_connect(dock, "clicked", G_CALLBACK(on_dock_button), self);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), dock);
    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(vbox), header);
    gtk_box_append(GTK_BOX(vbox), page);
    gtk_widget_set_vexpand(page, TRUE);
    adw_window_set_content(ADW_WINDOW(win), vbox);
    g_object_unref(page);
    g_object_set_data(G_OBJECT(win), "vbox", vbox);
    g_object_set_data(G_OBJECT(win), "tab", GINT_TO_POINTER(tab));
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), self);
    gtk_widget_add_controller(GTK_WIDGET(win), keys);
    g_signal_connect(win, "close-request", G_CALLBACK(on_detached_close), self);
    self->detached[tab] = win;
    gtk_window_present(win);
    refresh_now(self);
}

static void on_pop_out(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)btn;
    pop_out_tab(self, current_tab(self));
}

static void on_switch_page(GtkNotebook *notebook, GtkWidget *page, guint num, gpointer user_data)
{
    DbgWin *self = user_data;
    int tab = page_tab(page);
    (void)notebook; (void)num;
    if (tab >= 0 && tab < TAB_COUNT) {
        msx_help_set_context(self->win, k_tab_contexts[tab]);
        refresh_tab(self, tab);
        if (tab == TAB_VIDEO) dbg_video_refresh(self->pages[TAB_VIDEO], TRUE);
        if (tab == TAB_CONSOLE && self->console_in) gtk_widget_grab_focus(GTK_WIDGET(self->console_in));
    }
}

/* ---- closing ------------------------------------------------------------------ */

/* Closing destroys the window and lets the machine run on. Idempotent: it runs
 * from the close request and, if the parent goes first, from destroy. */
static void teardown(DbgWin *self)
{
    int txt;
    if (self->torn_down) return;
    self->torn_down = TRUE;
    dbg_video_shutdown(self->pages[TAB_VIDEO]);
    /* teardown re-selects notebook pages; nothing may call back into w */
    if (self->notebook) g_signal_handlers_disconnect_by_data(self->notebook, self);
    for (txt = 0; txt < TAB_COUNT; txt++)
        if (self->detached[txt]) {
            GtkWindow *detached_win = self->detached[txt];
            g_signal_handlers_disconnect_by_data(detached_win, self);
            self->detached[txt] = NULL;
            gtk_window_destroy(detached_win);
        }
    if (self->timer) { g_source_remove(self->timer); self->timer = 0; }
    if (self->dark_id) {
        g_signal_handler_disconnect(adw_style_manager_get_default(), self->dark_id);
        self->dark_id = 0;
    }
    msxdebug_detach(self->dbg);
    if (g_win == self) g_win = NULL;
}

static gboolean on_close(GtkWindow *win, gpointer user_data)
{
    (void)win;
    teardown(user_data);
    return FALSE;   /* go on and close */
}

/* The window's memory outlives its widgets: freed once the window is gone,
 * when no child can still signal into it. */
static void dbgwin_free(gpointer data, GObject *where)
{
    DbgWin *self = data;
    (void)where;
    teardown(self);
    if (self->history) g_ptr_array_unref(self->history);
    if (self->bp_sig) g_string_free(self->bp_sig, TRUE);
    if (self->wt_sig) g_string_free(self->wt_sig, TRUE);
    g_free(self);
}

static gboolean on_key(GtkEventControllerKey *ctl, guint keyval, guint code,
                       GdkModifierType mods, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)ctl; (void)code;
    switch (keyval) {
    case GDK_KEY_F1: msx_help_for(self->win); return TRUE;
    case GDK_KEY_F5: on_run(NULL, self); return TRUE;
    case GDK_KEY_F7:
        if (mods & GDK_SHIFT_MASK) on_frame(NULL, self); else on_step(NULL, self);
        return TRUE;
    case GDK_KEY_F8:
        if (mods & GDK_SHIFT_MASK) on_step_out(NULL, self); else on_step_over(NULL, self);
        return TRUE;
    case GDK_KEY_F9: toggle_selected(self); return TRUE;
    case GDK_KEY_F12: gtk_window_close(self->win); return TRUE;
    default: return FALSE;
    }
}

/* ---- construction --------------------------------------------------------- */

static GtkWidget *tool_button(const char *label, const char *tip, GCallback handler, DbgWin *self)
{
    GtkWidget *btn = gtk_button_new_with_mnemonic(label);
    gtk_widget_set_tooltip_text(btn, tip);
    g_signal_connect(btn, "clicked", handler, self);
    return btn;
}

static GtkWidget *build_toolbar(DbgWin *self)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *sym, *sympop, *symbox, *symload, *symclear, *save, *pop, *popbox;
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly ($0000-$FFFF)", "disassembly.asm" },
        { "mem", "Memory (64K)", "memory.bin" },
        { "vram", "VRAM", "vram.bin" },
        { "regs", "Registers (text)", "registers.txt" } };
    unsigned idx;

    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
    self->run_btn = GTK_BUTTON(gtk_button_new_with_label("Stop (F5)"));
    g_signal_connect(self->run_btn, "clicked", G_CALLBACK(on_run), self);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->run_btn));
    gtk_box_append(GTK_BOX(bar), tool_button("Step (F7)", "One instruction", G_CALLBACK(on_step), self));
    gtk_box_append(GTK_BOX(bar), tool_button("Step Over (F8)", "Over a CALL, HALT or block instruction",
                                             G_CALLBACK(on_step_over), self));
    gtk_box_append(GTK_BOX(bar), tool_button("Step Out (\xe2\x87\xa7""F8)", "Until this routine returns",
                                             G_CALLBACK(on_step_out), self));
    gtk_box_append(GTK_BOX(bar), tool_button("Run to Cursor", "Run to the selected disassembly line",
                                             G_CALLBACK(on_run_to_cursor), self));
    gtk_box_append(GTK_BOX(bar), tool_button("Frame (\xe2\x87\xa7""F7)", "Run to the next vertical blank",
                                             G_CALLBACK(on_frame), self));

    sym = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(sym), "Symbols\xe2\x80\xa6");
    sympop = gtk_popover_new();
    symbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    symload = gtk_button_new_with_label("Load symbols\xe2\x80\xa6");
    symclear = gtk_button_new_with_label("Clear loaded symbols");
    gtk_widget_add_css_class(symload, "flat");
    gtk_widget_add_css_class(symclear, "flat");
    g_signal_connect(symload, "clicked", G_CALLBACK(on_symbols), self);
    g_signal_connect(symclear, "clicked", G_CALLBACK(on_clear_symbols), self);
    gtk_box_append(GTK_BOX(symbox), symload);
    gtk_box_append(GTK_BOX(symbox), symclear);
    gtk_popover_set_child(GTK_POPOVER(sympop), symbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(sym), sympop);
    gtk_box_append(GTK_BOX(bar), sym);

    save = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(save), "Save\xe2\x80\xa6");
    pop = gtk_popover_new();
    popbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    for (idx = 0; idx < G_N_ELEMENTS(saves); idx++) {
        GtkWidget *save_btn = gtk_button_new_with_label(saves[idx].title);
        gtk_widget_add_css_class(save_btn, "flat");
        g_object_set_data(G_OBJECT(save_btn), "kind", (gpointer)saves[idx].kind);
        g_object_set_data(G_OBJECT(save_btn), "title", (gpointer)saves[idx].title);
        g_object_set_data(G_OBJECT(save_btn), "file", (gpointer)saves[idx].file);
        g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save), self);
        gtk_box_append(GTK_BOX(popbox), save_btn);
    }
    gtk_popover_set_child(GTK_POPOVER(pop), popbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(save), pop);
    gtk_box_append(GTK_BOX(bar), save);
    gtk_box_append(GTK_BOX(bar), tool_button("Pop _Out",
        "Move the tab that shows into a window of its own; Dock puts it back", G_CALLBACK(on_pop_out), self));
    return bar;
}

static GtkWidget *build_console(DbgWin *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = mono_view(&self->console_out, TRUE);
    GtkEventController *keys;

    make_log(self->console_out);
    self->console_in = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->console_in,
        "Command (help, step, break, bpw, bpa, bpin, print, mem, poke, watch, find, video, cart, ...) "
        "\xe2\x80\x94 Tab completes, Up/Down recall");
    gtk_widget_set_hexpand(GTK_WIDGET(self->console_in), TRUE);
    g_signal_connect(self->console_in, "activate", G_CALLBACK(on_console), self);
    /* ahead of the entry's own text handling, so Tab and Up/Down are ours */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_console_key), self);
    gtk_widget_add_controller(GTK_WIDGET(self->console_in), keys);
    self->history = g_ptr_array_new_with_free_func(g_free);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->console_in));
    set_text_at(self->console_out,
                "MSX debugger console (Z80 over openMSX). Type 'help' for every command; "
                "anything else goes to openMSX's own console.\n"
                "Numbers: $C000, 0xC000, C000h, 49152. Registers a f b c d e h l af bc de hl ix iy sp pc; "
                "[addr] is the byte there, {addr} the word.\n", TRUE);
    return padded(box);
}

/* The register strip under every tab: AF BC DE HL IX IY SP PC, the shadow
 * set, I R IM and the flags are entries (Enter sets one while the machine is
 * stopped); IFF1 and IFF2 are boxes, HALT is shown. */
static GtkWidget *build_regs(DbgWin *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *row3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    static const char *const iffnames[2] = { "IFF1", "IFF2" };
    int idx;

    for (idx = 0; idx < NREGS; idx++) {
        self->reg[idx] = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_max_length(self->reg[idx], 8);
        gtk_editable_set_width_chars(GTK_EDITABLE(self->reg[idx]), k_regs[idx].digits + 1);
        gtk_editable_set_max_width_chars(GTK_EDITABLE(self->reg[idx]), k_regs[idx].digits + 1);
        gtk_widget_add_css_class(GTK_WIDGET(self->reg[idx]), "monospace");
        g_object_set_data(G_OBJECT(self->reg[idx]), "reg", GINT_TO_POINTER(idx));
        g_signal_connect(self->reg[idx], "activate", G_CALLBACK(on_reg_activate), self);
        /* the main set on the first row; shadows and control on the second */
        gtk_box_append(GTK_BOX(idx < 8 ? row1 : row2), labeled(k_regs[idx].name, GTK_WIDGET(self->reg[idx])));
    }
    for (idx = 0; idx < NFLAGS; idx++) {
        self->flag[idx] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(k_flags[idx].name));
        g_object_set_data(G_OBJECT(self->flag[idx]), "flag", GINT_TO_POINTER(idx));
        g_signal_connect(self->flag[idx], "toggled", G_CALLBACK(on_flag_toggled), self);
        gtk_box_append(GTK_BOX(row3), GTK_WIDGET(self->flag[idx]));
    }
    for (idx = 0; idx < 2; idx++) {
        self->iff[idx] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(iffnames[idx]));
        g_object_set_data(G_OBJECT(self->iff[idx]), "flag", GINT_TO_POINTER(NFLAGS + idx));
        g_signal_connect(self->iff[idx], "toggled", G_CALLBACK(on_flag_toggled), self);
        gtk_box_append(GTK_BOX(row3), GTK_WIDGET(self->iff[idx]));
    }
    self->halt = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(self->halt), "msx-accent-text");
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->halt), "The CPU is in HALT, waiting for an interrupt");
    gtk_box_append(GTK_BOX(row3), GTK_WIDGET(self->halt));
    self->state = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(self->state), "dim-label");
    gtk_widget_add_css_class(GTK_WIDGET(self->state), "monospace");
    gtk_label_set_xalign(self->state, 0);
    gtk_label_set_ellipsize(self->state, PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(GTK_WIDGET(self->state), TRUE);
    gtk_box_append(GTK_BOX(row3), GTK_WIDGET(self->state));
    gtk_box_append(GTK_BOX(box), row1);
    gtk_box_append(GTK_BOX(box), row2);
    gtk_box_append(GTK_BOX(box), row3);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    return box;
}

static GtkWidget *build_disasm(DbgWin *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scroll = mono_view(&self->disasm, FALSE);
    GtkGesture *click = gtk_gesture_click_new();
    GtkEventController *scrollc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    GtkTextBuffer *tbuf = gtk_text_view_get_buffer(self->disasm);
    GtkWidget *hint;

    /* later tags win: the selection under the PC line */
    gtk_text_buffer_create_tag(tbuf, "bp_on", "foreground", "#e01b24", NULL);
    gtk_text_buffer_create_tag(tbuf, "bp_off", "foreground", "#8a8a8a", NULL);
    gtk_text_buffer_create_tag(tbuf, "sel", "paragraph-background", "rgba(128,128,128,0.35)", NULL);
    self->tag_pc = accent_tag(self->disasm, "pc");

    self->follow_pc = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Follow PC"));
    gtk_check_button_set_active(self->follow_pc, TRUE);
    g_signal_connect(self->follow_pc, "toggled", G_CALLBACK(on_follow_toggled), self);
    self->jump = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->jump, "$0038 or label");
    gtk_editable_set_width_chars(GTK_EDITABLE(self->jump), 14);
    g_signal_connect(self->jump, "activate", G_CALLBACK(on_jump), self);
    hint = gtk_label_new("Click selects a line; double-click, F9 or the gutter toggles its "
                         "breakpoint; scroll to browse");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(self->follow_pc));
    gtk_box_append(GTK_BOX(row), labeled("Jump to", GTK_WIDGET(self->jump)));
    gtk_box_append(GTK_BOX(row), hint);

    g_signal_connect(click, "pressed", G_CALLBACK(on_disasm_click), self);
    gtk_widget_add_controller(GTK_WIDGET(self->disasm), GTK_EVENT_CONTROLLER(click));
    g_signal_connect(scrollc, "scroll", G_CALLBACK(on_disasm_scroll), self);
    gtk_widget_add_controller(GTK_WIDGET(self->disasm), scrollc);
    /* the wheel is ours; EXTERNAL (unlike NEVER) does not ask for a window
     * tall enough to show every one of the DISASM_WINDOW lines */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_EXTERNAL);

    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), scroll);
    self->disasm_top = 0;
    self->selected = -1;
    return padded(box);
}

static GtkWidget *build_search_bar(DbgWin *self)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *next = tool_button("Find _Next", "Next match after the highlight", G_CALLBACK(on_search), self);
    GtkWidget *prev = tool_button("Find _Previous", "Previous match before the highlight", G_CALLBACK(on_search), self);
    static const char *const kinds[] = { "Hex bytes", "Text", NULL };

    g_object_set_data(G_OBJECT(prev), "back", GINT_TO_POINTER(1));
    self->srch_kind = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(kinds));
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->srch_kind), "Hex bytes (3E 01) or text, one character per byte");
    self->srch_entry = GTK_ENTRY(gtk_entry_new());
    gtk_widget_set_hexpand(GTK_WIDGET(self->srch_entry), TRUE);
    g_object_set_data(G_OBJECT(self->srch_entry), "next-button", next);
    g_signal_connect(self->srch_entry, "activate", G_CALLBACK(on_search_entry_activate), self);
    self->srch_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(self->srch_note), "dim-label");
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Search:"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->srch_kind));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->srch_entry));
    gtk_box_append(GTK_BOX(bar), next);
    gtk_box_append(GTK_BOX(bar), prev);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->srch_note));
    return bar;
}

static GtkWidget *build_memory(DbgWin *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = mono_view(&self->mem_view, FALSE);
    GtkWidget *page_up, *down, *poke;
    GtkEventController *scrollc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);

    self->mem_goto = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(self->mem_goto), 14);
    gtk_entry_set_placeholder_text(self->mem_goto, "$C000, label, lo-hi");
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->mem_goto),
        "An address, a label, an expression (hl+4) or a range; the bytes are marked");
    g_signal_connect(self->mem_goto, "activate", G_CALLBACK(on_mem_goto), self);
    self->mem_write = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->mem_write, "address byte...  e.g. C000 41 42");
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->mem_write),
        "Writes the bytes from the address on, through the slot map (stopped). RAM takes them; ROM ignores them.");
    gtk_widget_set_hexpand(GTK_WIDGET(self->mem_write), TRUE);
    g_signal_connect(self->mem_write, "activate", G_CALLBACK(on_mem_write), self);
    poke = tool_button("Po_ke", "Write the bytes", G_CALLBACK(on_mem_write), self);
    g_signal_handlers_disconnect_by_func(poke, G_CALLBACK(on_mem_write), self);
    g_signal_connect_swapped(poke, "clicked", G_CALLBACK(gtk_widget_activate), self->mem_write);
    page_up = gtk_button_new_with_label("Page Up");
    down = gtk_button_new_with_label("Page Down");
    g_object_set_data(G_OBJECT(page_up), "delta", GINT_TO_POINTER(-1));
    g_object_set_data(G_OBJECT(down), "delta", GINT_TO_POINTER(1));
    g_signal_connect(page_up, "clicked", G_CALLBACK(on_mem_page), self);
    g_signal_connect(down, "clicked", G_CALLBACK(on_mem_page), self);
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Address:"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->mem_goto));
    gtk_box_append(GTK_BOX(bar), page_up);
    gtk_box_append(GTK_BOX(bar), down);
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Poke:"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->mem_write));
    gtk_box_append(GTK_BOX(bar), poke);

    {
        char fill[16], ink[16];
        g_snprintf(fill, sizeof fill, "#%06x", accent_fill());
        g_snprintf(ink, sizeof ink, "#%06x", accent_ink());
        self->tag_mem_mark = gtk_text_buffer_create_tag(gtk_text_view_get_buffer(self->mem_view), "mark",
                                                     "background", fill, "foreground", ink, NULL);
    }
    g_signal_connect(scrollc, "scroll", G_CALLBACK(on_mem_scroll), self);
    gtk_widget_add_controller(GTK_WIDGET(self->mem_view), scrollc);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_EXTERNAL);

    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), build_search_bar(self));
    gtk_box_append(GTK_BOX(box), scroll);
    return padded(box);
}

static GtkWidget *build_breaks(DbgWin *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *form = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    static const char *const types[] = { "Execute", "Read", "Write", "Access", "Port In", "Port Out", NULL };

    self->bp_type = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(types));
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->bp_type),
        "Execute stops before an instruction; Read, Write and Access stop after the instruction "
        "that touched the bytes; Access is read or write; a port is 0-255");
    self->bp_addr = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->bp_addr, "address, label, lo-hi, expression, or a port");
    gtk_widget_set_hexpand(GTK_WIDGET(self->bp_addr), TRUE);
    g_signal_connect(self->bp_addr, "activate", G_CALLBACK(on_bp_add), self);
    self->bp_cond = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->bp_cond, "condition (optional): value != old, a == $12, [hl] > 5");
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->bp_cond),
        "An expression: a f b c d e h l af bc de hl ix iy sp pc, flags cf zf sf pf nf hf, [addr], {addr}, "
        "labels, frame, cycles, vline; on an access `value`, `addr` and `old` (the byte before a write). "
        "An openMSX Tcl expression such as [reg A] == 5 also works.");
    gtk_widget_set_hexpand(GTK_WIDGET(self->bp_cond), TRUE);
    g_signal_connect(self->bp_cond, "activate", G_CALLBACK(on_bp_add), self);
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(self->bp_type));
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(self->bp_addr));
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(self->bp_cond));
    gtk_box_append(GTK_BOX(form), tool_button("_Add", "Add the breakpoint", G_CALLBACK(on_bp_add), self));

    self->bp_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->bp_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(self->bp_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    self->bp_msg = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(self->bp_msg), "msx-accent-text");
    gtk_label_set_xalign(self->bp_msg, 0);
    gtk_label_set_wrap(self->bp_msg, TRUE);
    gtk_widget_set_visible(GTK_WIDGET(self->bp_msg), FALSE);
    self->bp_stop = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(self->bp_stop, 0);
    gtk_widget_add_css_class(GTK_WIDGET(self->bp_stop), "dim-label");
    gtk_widget_set_hexpand(GTK_WIDGET(self->bp_stop), TRUE);
    gtk_box_append(GTK_BOX(foot), GTK_WIDGET(self->bp_stop));
    gtk_box_append(GTK_BOX(foot), tool_button("Clear _Hits", "Zero the hit counts", G_CALLBACK(on_bp_clear_hits), self));
    gtk_box_append(GTK_BOX(foot), tool_button("_Remove All", "Remove every breakpoint", G_CALLBACK(on_bp_clear), self));

    gtk_box_append(GTK_BOX(box), form);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->bp_msg));
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), foot);
    return padded(box);
}

static GtkWidget *build_watches(DbgWin *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *form = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();

    self->wt_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->wt_entry, "address, label, expression, or lo-hi");
    gtk_widget_set_hexpand(GTK_WIDGET(self->wt_entry), TRUE);
    g_signal_connect(self->wt_entry, "activate", G_CALLBACK(on_wt_entry_activate), self);
    gtk_box_append(GTK_BOX(form), gtk_label_new("Watch:"));
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(self->wt_entry));
    gtk_box_append(GTK_BOX(form), tool_button("_Add", "Watch it", G_CALLBACK(on_wt_add), self));

    self->wt_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->wt_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(self->wt_list));
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), form);
    gtk_box_append(GTK_BOX(box), scroll);
    return padded(box);
}

static GtkWidget *build_cart(DbgWin *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *left = mono_view(&self->cart_text, TRUE);
    GtkWidget *right = mono_view(&self->io_text, FALSE);
    gtk_box_append(GTK_BOX(box), headed("Machine and memory map", left));
    gtk_box_append(GTK_BOX(box), headed("Slots, mapper, PPI, keyboard and joysticks", right));
    return padded(box);
}

static GtkWidget *build_hist(DbgWin *self, int idx)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = mono_view(&self->h_text[idx], FALSE);
    GtkWidget *clear = gtk_button_new_with_mnemonic("_Clear");
    static const char *const filters[] = { "All instructions", "Branches only", "In range", NULL };

    make_log(self->h_text[idx]);
    self->h_rec[idx] = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Record"));
    gtk_widget_add_css_class(GTK_WIDGET(self->h_rec[idx]), "msx-toggle");
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->h_rec[idx]),
                                "Record every instruction the CPU runs (the machine runs a little slower)");
    g_signal_connect(self->h_rec[idx], "toggled", G_CALLBACK(on_hist_rec), self);
    self->h_filter[idx] = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(filters));
    g_signal_connect(self->h_filter[idx], "notify::selected", G_CALLBACK(on_hist_changed), self);
    self->h_range[idx] = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->h_range[idx], "range: lo-hi or a label");
    g_signal_connect(self->h_range[idx], "activate", G_CALLBACK(on_hist_changed), self);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_hist_clear), self);
    self->h_note[idx] = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(self->h_note[idx]), "dim-label");
    gtk_widget_set_hexpand(GTK_WIDGET(self->h_note[idx]), TRUE);
    gtk_label_set_xalign(self->h_note[idx], 1.0f);
    self->h_seen[idx] = (uint64_t)-1;

    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->h_rec[idx]));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->h_filter[idx]));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->h_range[idx]));
    gtk_box_append(GTK_BOX(bar), clear);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->h_note[idx]));
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), scroll);
    return padded(box);
}

static GtkWidget *build_symbols(DbgWin *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();

    self->sym_filter = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->sym_filter, "filter by name");
    gtk_widget_set_hexpand(GTK_WIDGET(self->sym_filter), TRUE);
    g_signal_connect(self->sym_filter, "changed", G_CALLBACK(on_sym_filter_changed), self);
    self->sym_count = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(self->sym_count), "dim-label");
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Filter:"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->sym_filter));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->sym_count));
    self->symlist = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->symlist, GTK_SELECTION_NONE);
    g_signal_connect(self->symlist, "row-activated", G_CALLBACK(on_sym_row_activated), self);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(self->symlist));
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), scroll);
    self->sym_built = -1;     /* the first refresh always fills the list */
    return padded(box);
}

/* The packets left (a note and Clear over them), the cartridge's link and
 * counts right. */
static GtkWidget *build_fujinet(DbgWin *self)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    GtkWidget *start, *end;

    self->fuji_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(self->fuji_note), "dim-label");
    gtk_label_set_xalign(self->fuji_note, 0);
    gtk_label_set_wrap(self->fuji_note, TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(self->fuji_note), TRUE);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_fuji_clear), self);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(self->fuji_note));
    gtk_box_append(GTK_BOX(row), clear);
    gtk_box_append(GTK_BOX(left), row);
    gtk_box_append(GTK_BOX(left), mono_view(&self->fuji_text, FALSE));
    make_log(self->fuji_text);

    start = headed("FujiBus packets (SLIP-decoded)", left);
    end = headed("FujiNet cartridge", mono_view(&self->fuji_state, FALSE));
    gtk_paned_set_start_child(GTK_PANED(paned), start);
    gtk_paned_set_end_child(GTK_PANED(paned), end);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(start, 620, -1);
    gtk_widget_set_size_request(end, 340, -1);
    self->fuji_count = -1;
    return padded(paned);
}

/* A tab from an environment variable (MSX_DEBUGGER_TAB, MSX_POPOUT): a
 * number, a tab's name or the start of one (any case), or an older name
 * (cpu, registers, vdp, io, slots, psg). For tests and captures. */
static int tab_from_env(const char *var)
{
    const char *tab = g_getenv(var);
    char *end;
    long count;
    size_t idx;
    if (!tab || !*tab) return -1;
    count = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (count >= 0 && count < TAB_COUNT) ? (int)count : -1;
    for (idx = 0; idx < G_N_ELEMENTS(k_tab_aliases); idx++)
        if (g_ascii_strcasecmp(tab, k_tab_aliases[idx].name) == 0)
            return k_tab_aliases[idx].tab;
    for (idx = 0; idx < TAB_COUNT; idx++)
        if (g_ascii_strncasecmp(tab, k_tab_names[idx], strlen(tab)) == 0)
            return (int)idx;
    return -1;
}

static void attach_and_refresh(DbgWin *self)
{
    msxdebug_attach(self->dbg);     /* stops the machine */
    self->running_ticks = 0;
    self->map_valid = FALSE;
    if (self->bp_sig) { g_string_free(self->bp_sig, TRUE); self->bp_sig = NULL; }
    if (self->wt_sig) { g_string_free(self->wt_sig, TRUE); self->wt_sig = NULL; }
    refresh_now(self);
}

/* The capture hooks: MSX_WATCH adds a watch, MSX_WATCH_BREAK a break-on-change
 * on it, MSX_MEM_GOTO shows it in Memory, MSX_HISTORY records from the start.
 * Opening the window again must not stack a second copy. */
static void apply_env_hooks(DbgWin *self)
{
    const char *watch = g_getenv("MSX_WATCH"), *brk = g_getenv("MSX_WATCH_BREAK"),
               *goto_text = g_getenv("MSX_MEM_GOTO");
    uint16_t range_lo, range_hi;
    if (watch && *watch) {
        int count = msxwatch_count(), idx;
        gboolean have = FALSE;
        for (idx = 0; idx < count; idx++)
            if (strcmp(msxwatch_text(idx), watch) == 0) have = TRUE;
        if (!have) msxwatch_add(watch);
    }
    if (brk && *brk && msxsym_parse_range(self->dbg, brk, &range_lo, &range_hi))
        break_on_change(self, range_lo, range_hi);
    if (goto_text && *goto_text && msxsym_parse_range(self->dbg, goto_text, &range_lo, &range_hi))
        show_in_memory(self, range_lo, range_hi);
    if (g_getenv("MSX_HISTORY"))
        msxdebug_history_enable(self->dbg, 1);
}

void msx_debugger_show(GtkWindow *parent, msxsession *session)
{
    DbgWin *self;
    GtkWidget *toolbar, *header, *root, *notebook, *overlay;
    GtkEventController *keys;
    int tab;

    if (g_win) {
        gtk_window_present(g_win->win);
        attach_and_refresh(g_win);
        return;
    }
    install_css();
    self = g_new0(DbgWin, 1);
    self->session = session;
    self->dbg = msxsession_debugger(session);
    self->mem_base = 0;
    g_win = self;

    self->win = GTK_WINDOW(adw_window_new());
    g_object_set_data(G_OBJECT(self->win), "dbgwin", self);
    g_object_weak_ref(G_OBJECT(self->win), dbgwin_free, self);
    gtk_window_set_title(self->win, "Debugger");
    gtk_window_set_default_size(self->win, 1150, 820);
    gtk_window_set_transient_for(self->win, parent);
    gtk_window_set_application(self->win, gtk_window_get_application(parent));
    gtk_window_set_destroy_with_parent(self->win, TRUE);
    g_signal_connect(self->win, "close-request", G_CALLBACK(on_close), self);

    self->pages[TAB_CONSOLE] = build_console(self);
    self->pages[TAB_DISASM] = build_disasm(self);
    self->pages[TAB_MEMORY] = build_memory(self);
    self->pages[TAB_BREAKS] = build_breaks(self);
    self->pages[TAB_WATCHES] = build_watches(self);
    self->pages[TAB_CART] = build_cart(self);
    self->pages[TAB_TRACE] = build_hist(self, 0);
    self->pages[TAB_HISTORY] = build_hist(self, 1);
    self->pages[TAB_VIDEO] = dbg_video_new(self->dbg);
    self->pages[TAB_SOUND] = padded(mono_view(&self->sound_text, FALSE));
    self->pages[TAB_SYMBOLS] = build_symbols(self);
    self->pages[TAB_FUJINET] = build_fujinet(self);

    notebook = gtk_notebook_new();
    self->notebook = GTK_NOTEBOOK(notebook);
    gtk_widget_set_vexpand(notebook, TRUE);
    for (tab = 0; tab < TAB_COUNT; tab++) {
        /* each page keeps its identity when it is moved around */
        g_object_set_data(G_OBJECT(self->pages[tab]), "tab-index", GINT_TO_POINTER(tab + 1));
        gtk_notebook_append_page(self->notebook, self->pages[tab], gtk_label_new(k_tab_names[tab]));
    }
    tab = tab_from_env("MSX_DEBUGGER_TAB");
    if (tab >= 0) gtk_notebook_set_current_page(self->notebook, tab);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), self);
    {
        int cur = gtk_notebook_get_current_page(self->notebook);
        msx_help_set_context(self->win, k_tab_contexts[cur >= 0 && cur < TAB_COUNT ? cur : 0]);
    }

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    toolbar = build_toolbar(self);
    gtk_box_append(GTK_BOX(root), toolbar);
    gtk_box_append(GTK_BOX(root), notebook);
    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(root), build_regs(self));

    overlay = adw_toast_overlay_new();
    self->toasts = ADW_TOAST_OVERLAY(overlay);
    adw_toast_overlay_set_child(self->toasts, root);

    /* the status line -- why the machine stopped -- is the title's subtitle */
    header = adw_header_bar_new();
    self->title = ADW_WINDOW_TITLE(adw_window_title_new("Debugger", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(self->title));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), msx_help_button(NULL));
    {
        GtkWidget *view = adw_toolbar_view_new();
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), overlay);
        adw_window_set_content(ADW_WINDOW(self->win), view);
    }

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), self);
    gtk_widget_add_controller(GTK_WIDGET(self->win), keys);

    self->dark_id = g_signal_connect(adw_style_manager_get_default(), "notify::dark",
                                  G_CALLBACK(on_dark_changed), self);

    apply_env_hooks(self);
    attach_and_refresh(self);
    self->timer = g_timeout_add(TICK_MS, tick_refresh, self);
    gtk_window_present(self->win);
    if (current_tab(self) == TAB_CONSOLE) gtk_widget_grab_focus(GTK_WIDGET(self->console_in));
    tab = tab_from_env("MSX_POPOUT");   /* a tab in a window of its own, for captures */
    if (tab >= 0) pop_out_tab(self, tab);
}

/* F12 and the menu item: show it, or close it if it is showing. */
void msx_debugger_toggle(GtkWindow *parent, msxsession *session)
{
    if (g_win && gtk_widget_get_visible(GTK_WIDGET(g_win->win)))
        gtk_window_close(g_win->win);
    else
        msx_debugger_show(parent, session);
}
