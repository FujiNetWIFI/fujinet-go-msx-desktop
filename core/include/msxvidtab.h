/*
 * msxvidtab -- the Video tab's controller. All four frontends draw the same
 * tab from it, so what they own is widgets: a view and zoom chooser, overlay
 * toggles, a picture, the inspector and the sections. Modelled on the INTV
 * controller (intvvidtab.h), so a frontend that has one ports the other by
 * changing names:
 *
 *   - the machine is only COPIED: a finished frame (msxvidcap.h) is captured
 *     once per frame on the openMSX thread, and a parked machine is read
 *     live. Decoding, scaling and overlays run on the caller's (UI) thread;
 *   - the picture is rebuilt only when something it shows changed, into one
 *     reused buffer;
 *   - hover and pin are state here, so a mouse move never re-renders the
 *     picture: draw the marks from msxvid_tab_marks() over the cached image;
 *   - pixels are 0xFFRRGGBB (opaque), whatever the toolkit does with alpha;
 *   - one table of overlays and one env-var parser, so the frontends cannot
 *     disagree.
 *
 * A VDP draws every line from the registers as they stand when the line is
 * drawn. The per-line log of a captured frame (every register and palette
 * write with the line it happened at) therefore says what each line was made
 * from: the Decoded views are drawn band by band (a split screen, a raster
 * palette change or a mode switch mid-frame shows as bands), the Lines section
 * lists the bands, and the Registers section shows how many times and where in
 * the frame each register was written.
 *
 * Views: Display is the frame the machine published (640 x 480, the border
 * included); Decoded redraws one source from VRAM and the registers: the
 * screen (the name table in the tile modes, the page in Graphic 4-7), the
 * pattern sheet, the sprites or the palette; Beam is the Display with the lines
 * the beam has not reached yet dimmed (stopped only); Values shows each name
 * table entry (or, in a bitmap mode, each byte of the page) as a flat false
 * colour of its value.
 *
 * Not decoded: Text 2, YJK/YAE colour, sprite mode 2 (V9938 Graphic 3-7),
 * Multicolor patterns beyond their name table, horizontal scroll (R#26/27).
 * They show the published frame in the Display view and say so elsewhere.
 *
 * UI thread only. Not thread-safe.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSXVIDTAB_H
#define MSXVIDTAB_H

#include <stdint.h>

#include "msxdebug.h"
#include "msxvidcap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct msxvid_tab msxvid_tab;

enum msxvid_view {
    MSXVID_V_DISPLAY = 0,       /* the published frame, 640 x 480 */
    MSXVID_V_DECODED,           /* one source rebuilt from VRAM and the registers (see msxvid_source) */
    MSXVID_V_BEAM,              /* the Display, the lines below the beam dimmed (stopped) */
    MSXVID_V_VALUES,            /* each name table entry / page byte as a flat false colour */
    MSXVID_V_COUNT
};

/* What the Decoded view redraws. */
enum msxvid_source {
    MSXVID_SRC_SCREEN = 0,      /* the name table in its mode (256 x 192), or the page in Graphic 4-7 */
    MSXVID_SRC_PATTERNS,        /* the pattern generator table, 32 tiles a row (one bank in Graphic 2) */
    MSXVID_SRC_SPRITES,         /* the 32 sprites at native size, 8 x 4 */
    MSXVID_SRC_PALETTE,         /* the 16 colours */
    MSXVID_SRC_COUNT
};

/* Overlays: one table, bit per entry. */
enum {
    MSXVID_OV_GRID    = 1u << 0,    /* the 8 x 8 tile grid (Display, Screen, Values, Beam) */
    MSXVID_OV_SPRITES = 1u << 1,    /* each sprite's box (Display, Beam, Screen) */
    MSXVID_OV_BORDER  = 1u << 2,    /* the border and blanking, tinted (Display, Beam) */
    MSXVID_OV_BEAM    = 1u << 3,    /* where the beam is (stopped) */
    MSXVID_OV_HEAT    = 1u << 4,    /* name table entries that changed lately */
    MSXVID_OV_SPLITS  = 1u << 5,    /* the lines where the registers changed (Display, Screen, Beam) */
    MSXVID_OV_ALL     = 0x3Fu
};
typedef struct {
    unsigned bit;
    const char *name;           /* the env name: grid, sprites, ... */
    const char *label;          /* the checkbox text */
} msxvid_overlay_info;
const msxvid_overlay_info *msxvid_overlays(int *count);
const char *msxvid_view_name(int view);             /* "Display", ... */
const char *msxvid_view_key(int view);              /* "display", ... (env) */
const char *msxvid_source_name(int source);         /* "Screen", ... */
const char *msxvid_source_key(int source);          /* "screen", ... */

/* Right-hand sections. */
enum { MSXVID_S_PIXEL = 0, MSXVID_S_REGISTERS, MSXVID_S_COLOURS, MSXVID_S_LINES,
       MSXVID_S_TIMING, MSXVID_S_VRAM, MSXVID_S_COUNT };
const char *msxvid_section_name(int section);
const char *msxvid_section_key(int section);

/* dbg is the debugger the tab asks whether the machine is parked and adds
 * write breakpoints to; NULL (tests) makes every Break on... return -1 and the
 * machine count as running. The tab asks the host to capture frames until it
 * is freed. */
msxvid_tab *msxvid_tab_new(msxdebug *dbg);
void        msxvid_tab_free(msxvid_tab *tab);

/* ---- what to show (they mark the picture stale) ------------------------------------ */
void     msxvid_tab_set_view(msxvid_tab *tab, int view);
int      msxvid_tab_view(const msxvid_tab *tab);
void     msxvid_tab_set_source(msxvid_tab *tab, int source);
int      msxvid_tab_source(const msxvid_tab *tab);
void     msxvid_tab_set_overlays(msxvid_tab *tab, unsigned ovl);
unsigned msxvid_tab_overlays(const msxvid_tab *tab);
/* 1, 2, 3, 4, 6, 8, 12, 16, or 0 for "fit the viewport" (the largest of
 * those that fits both ways) */
void     msxvid_tab_set_zoom(msxvid_tab *tab, int zoom);
int      msxvid_tab_zoom(const msxvid_tab *tab);
/* the room the picture has, for Fit; call when the widget resizes */
void     msxvid_tab_set_viewport(msxvid_tab *tab, int wide, int high);
/* the zoom steps the chooser offers (0 = Fit first) */
const int *msxvid_zooms(int *count);
/* The Patterns source in Graphic 2/3 shows one of three 256-tile banks. */
void     msxvid_tab_set_bank(msxvid_tab *tab, int bank);
int      msxvid_tab_bank(const msxvid_tab *tab);
/* The first byte of the VRAM section's hex dump (the VRAM pane); set by the
 * pane's address box, and by a pin on a pixel it is made from. */
void     msxvid_tab_set_vram_base(msxvid_tab *tab, uint32_t addr);
uint32_t msxvid_tab_vram_base(const msxvid_tab *tab);

/*
 * The scripted-capture variables, read once by msxvid_tab_new:
 *   MSX_DEBUGGER_VIDEO_VIEW      display decoded beam values
 *   MSX_DEBUGGER_VIDEO_SOURCE    screen patterns sprites palette   (the Decoded view's source)
 *   MSX_DEBUGGER_VIDEO_OVERLAYS  grid,sprites,border,beam,heat,splits | all | none
 *   MSX_DEBUGGER_VIDEO_ZOOM      1..16 or fit
 *   MSX_DEBUGGER_VIDEO_SECTION   pixel registers colours lines timing vram   (names, never indexes)
 * Returns the starting section, MSXVID_S_PIXEL when unset.
 */
int msxvid_tab_env_section(const msxvid_tab *tab);

/* ---- refresh ------------------------------------------------------------------------ */
#define MSXVID_TAB_PICTURE 1u   /* repaint the picture (msxvid_tab_picture) */
#define MSXVID_TAB_TABLES  2u   /* refill the status line and the sections */
#define MSXVID_TAB_OFF     4u   /* no machine: show msxvid_tab_why() instead of a picture */

/* Take the machine's state, update the trackers, and say what to redraw.
 * force: redraw everything. */
unsigned msxvid_tab_refresh(msxvid_tab *tab, int force);

/* For tests: use this capture instead of the machine. stopped: the machine
 * counts as parked, so the Beam view works (cap->live says the VDP part is a
 * parked machine's, with cap->beam_line the beam). */
void msxvid_tab_feed(msxvid_tab *tab, const msxvid_capture *cap, int stopped);

/* ---- the picture ------------------------------------------------------------------- */
/*
 * The rendered picture, 0xFFRRGGBB, valid until the next refresh or set_*. NULL
 * when this view cannot be drawn: msxvid_tab_why() says why, and the tab
 * should show that text.
 */
const uint32_t *msxvid_tab_picture(msxvid_tab *tab, int *wide, int *high);
const char     *msxvid_tab_why(const msxvid_tab *tab);
int             msxvid_tab_effective_zoom(const msxvid_tab *tab);

/* ---- the tables -------------------------------------------------------------------- */
const char *msxvid_tab_status(const msxvid_tab *tab);        /* two lines, '\n' between */

/* VDP registers: R#0-27 and the command engine's R#32-46 (the pairs SX, SY,
 * DX, DY, NX, NY as one entry each). */
typedef struct {
    const char *name;           /* "R#0", "R#32-33 SX", ... */
    char value[24];             /* "05", "0123" */
    char meaning[160];
    int age;                    /* refreshes since it last changed (MSXVID_REG_FADE+: not recent) */
    int writes;                 /* writes to it in the captured frame, same value or not */
    int first_line;             /* the absolute line of the first one, -1 when none */
} msxvid_reg;
#define MSXVID_REG_FADE 12
int msxvid_tab_regs(const msxvid_tab *tab, const msxvid_reg **out);

/* The 16 colours. */
typedef struct {
    int index;                  /* 0..15 */
    const char *name;           /* "Medium green (2)" */
    uint32_t rgb;               /* 0xFFRRGGBB */
    long uses;                  /* pixels of the published frame with exactly this colour */
    char role[48];              /* "backdrop (R#7)", "" when unused by the registers */
    int inspected;              /* the inspected pixel has it */
    int changed;                /* a palette write in the captured frame touched it */
} msxvid_swatch;
int msxvid_tab_swatches(const msxvid_tab *tab, const msxvid_swatch **out);

/* The Lines table: one entry for each band of display lines drawn from the
 * same register and palette values (a program that never changes them in the
 * frame has one). */
typedef struct {
    int first, last;            /* display lines, inclusive (0 = the first line of the screen) */
    int abs_first;              /* the same in absolute frame lines */
    char mode[24];              /* "Graphic 2" */
    unsigned name_table, pattern_table, colour_table, sprite_attr, sprite_pattern;   /* VRAM addresses */
    int backdrop;               /* the colour index of the backdrop */
    int scroll_y;               /* R#23 */
    int writes;                 /* register and palette writes that took effect on its first line */
    char text[160];             /* ready-made line */
} msxvid_band;
int msxvid_tab_lines(const msxvid_tab *tab, const msxvid_band **out);

/* the timing section, as ready-made "label\tvalue" lines */
int  msxvid_tab_timing(const msxvid_tab *tab, const char *const **lines);
/* where the beam is in the whole frame, 0..1000 (0 when unknown) */
int  msxvid_tab_beam_permille(const msxvid_tab *tab);

/* A section as plain text (the console's `video [section]` shares it). */
int  msxvid_tab_text(msxvid_tab *tab, int section, char *dst, int dstsz);
/* A section by name or prefix ("pixel", "reg", ...); -1 if none. */
int  msxvid_section_from_name(const char *name);
/* The console's `video`: attaches to the machine for the call, no tab needed.
 * section NULL or "" is the status and the registers. Returns the length. */
int  msxvid_tab_text_for(msxdebug *dbg, const char *section, char *dst, int dstsz);

/* ---- hover, pin ---------------------------------------------------------------------- */
enum { MSXVID_PK_NONE = 0, MSXVID_PK_BORDER, MSXVID_PK_TILE, MSXVID_PK_BITMAP,
       MSXVID_PK_SPRITE, MSXVID_PK_PATTERN, MSXVID_PK_PALETTE, MSXVID_PK_VALUE };
/* What Break on writes watches for each target. */
enum { MSXVID_BK_VRAM = 0, MSXVID_BK_PALETTE, MSXVID_BK_REGISTER };

typedef struct {
    int valid;
    int picx, picy;             /* picture pixel (base picture, before the zoom) */
    int kind;                   /* MSXVID_PK_* */
    int markx, marky, markw, markh;     /* the object under the pointer, in picture pixels */
    int vdp_x, vdp_y;           /* the VDP pixel (256 or 512 wide, the display's lines), -1 when the view has none */
    int line;                   /* the absolute frame line, -1 when none */
    int colour;                 /* palette index of the pixel, -1 if none */
    int cell;                   /* name table entry under it, -1 */
    int name;                   /* the tile it shows, -1 */
    int sprite;                 /* the front sprite covering it, -1 */
    unsigned sprite_mask;       /* every sprite box that holds it, bit per sprite */
    unsigned vram_addr;         /* the VRAM address the pixel is made from (name entry, page byte, ...), 0xFFFFFFFF if none */
    int nbreak;                 /* write breakpoint targets (up to 3 ranges) */
    unsigned break_lo[3], break_hi[3];
    int break_kind[3];          /* MSXVID_BK_*: lo..hi is a physical VRAM range, a palette entry or a register */
    uint32_t rgb;               /* 0xFFRRGGBB */
    char text[640];             /* ready-made inspector text */
} msxvid_pixel;

/* x, y in picture pixels at the current zoom; -1,-1 when the pointer left.
 * Returns 1 when the inspector changed. */
int  msxvid_tab_hover(msxvid_tab *tab, int posx, int posy);
void msxvid_tab_pin(msxvid_tab *tab, int posx, int posy);      /* a click; the same cell unpins */
void msxvid_tab_unpin(msxvid_tab *tab);
/* the pinned pixel if any, else the hovered one, else NULL */
const msxvid_pixel *msxvid_tab_pixel(const msxvid_tab *tab);
/* rectangles x,y,w,h in picture pixels (zoom applied) for the marks drawn over
 * the picture; bit0 of the result: hover valid, bit1: pin valid */
int  msxvid_tab_marks(const msxvid_tab *tab, int hover[4], int pin[4]);
/*
 * "Break on writes": breakpoints on what the inspected pixel is made from.
 * What the engine can watch is the CPU's port writes, so:
 *   - VRAM (a name table entry, a pattern or colour byte, a sprite attribute or
 *     pattern, a bitmap byte): an OUT to the data port $98 whose VRAM address
 *     latch is in the range. The latch is read when the watchpoint fires, the
 *     moment the CPU's OUT executes; the VDP stores the byte a few cycles
 *     later. The address is the physical one (Graphic 6/7 interleave their
 *     banks), 17 bits with R#14. Writes by the command engine (HMMV, LMMM, ...)
 *     do not go through the port and are NOT seen.
 *   - a palette entry (V9938 and later): an OUT to the palette data port $9A
 *     (any entry: the port has no address, so every palette write breaks).
 *   - a register (the backdrop, the mode bits): an OUT to the control port
 *     $99 whose byte is $80 + the register. The first byte of a pair, which
 *     is data, can look the same (a rare false stop).
 * -1 when the pixel has no such target or without a debugger. Not added twice
 * (the same port and condition is found again). Returns the first breakpoint's
 * id, or -1. */
int  msxvid_tab_break_on_pixel(msxvid_tab *tab);
/* 1 when "Break on writes" has something to break on now */
int  msxvid_tab_can_break(const msxvid_tab *tab);

#ifdef __cplusplus
}
#endif

#endif
