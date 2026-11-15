/*
 * vidtab.c -- the Video tab's controller (core/include/msxvidtab.h).
 *
 * Everything here works from an msxvid_capture: the published frame, the VDP's
 * registers, palette and VRAM at the frame's end, and the per-line log of the
 * register and palette writes. The Decoded views rebuild their picture from
 * VRAM and the registers through the msxvdpview renderers, band by band where
 * the log says the registers changed during the frame, so on an unchanged
 * frame the tiles are the tiles the machine drew.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "msxvidtab.h"

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../openmsx/msx_host.hh"
#include "compat.h"
#include "msxvdp.h"

#define MAXPIX 6000000L         /* the largest picture we will build */
#define MAX_REGS 64
#define MAX_TIMING 20
#define MAX_BANDS 64
#define MAX_BAND_RENDERS 32
#define HEAT_FRAMES 24          /* changes are drawn this many captures */
#define NT_MAX 1920
#define REG_LAST_CONTROL 27
#define SPRITES 32
#define SPRITE_TILE 16
#define SPRITE_PITCH 16

static const int k_zooms[] = { 0, 1, 2, 3, 4, 6, 8, 12, 16 };

enum { BK_FRAME = 0, BK_SCREEN, BK_VALUES, BK_PATTERNS, BK_SPRITES, BK_PALETTE };   /* what the base picture is */

#define COL_BLACK 0xff000000u
#define COL_PANEL 0xff202428u
#define NO_ADDR 0xFFFFFFFFu

typedef struct {
    int first, last;                /* display lines, inclusive */
    uint8_t regs[32];
    uint16_t pal[16];               /* 0x0GRB */
    int writes;
} band_rec;

/* where the display sits in the 640 x 480 published frame */
typedef struct {
    int lines;                      /* display lines: 192 or 212 */
    int wide;                       /* VDP pixels across: 256 or 512 */
    int xoff, xscale;               /* frame x of VDP x = 0, frame pixels per VDP pixel */
    int zero;                       /* absolute line of display line 0 */
    int top;                        /* absolute line of frame row 0 */
    int pal;
} geom_rec;

struct msxvid_tab {
    msxdebug *dbg;
    int view, source, zoom, vieww, viewh, bank;
    unsigned ovl;
    int env_section, capture_on;
    uint32_t vram_base;

    /* the machine's state as presented */
    msxvid_capture *cap;            /* working capture */
    msxvid_capture *last;           /* last captured by the openMSX thread */
    msxvdp_snapshot *snap;          /* the VDP part of cap in msxvdpview's form */
    msxvdp_mode_info mode;
    uint64_t last_serial;
    int have, has_fb, has_live, stopped, fed;
    uint64_t sig;
    int live_valid, have_last_live;     /* cap holds a live read of the parked machine ... */
    unsigned live_gen;                  /* ... taken at this debugger generation ... */
    uint64_t last_live_serial;          /* ... and this captured frame */

    /* the bands of the frame */
    band_rec bands[MAX_BANDS];
    int nbands;
    geom_rec geom;

    /* what changed lately */
    uint8_t prev_regs[64];
    int reg_age[64];
    int tracked;
    uint8_t prev_nt[NT_MAX];
    int heat[NT_MAX];
    int nt_cells, nt_cols;
    uint32_t nt_base;

    /* the picture */
    uint32_t *base, *pic;
    size_t base_cap, pic_cap;
    int basew, baseh, picw, pich, effzoom, kind;
    int tile_w;                     /* width of one name table cell in the picture's pixels (6 in Text 1, else 8) */
    int dirty;
    char why[200];

    /* tables */
    msxvid_reg regs[MAX_REGS];
    char reg_names[MAX_REGS][24];
    int nregs;
    msxvid_swatch swat[16];
    char swat_names[16][32];
    msxvid_band lines[MAX_BANDS];
    int nlines;
    char status[400];
    char timing_buf[MAX_TIMING][112];
    const char *timing_ptr[MAX_TIMING];
    int ntiming;
    int reg_writes[64], reg_first[64];

    /* pixels */
    int hovx, hovy, pinx, piny;     /* -1: none */
    msxvid_pixel hover, pin;
};

/* ---- tables of names ----------------------------------------------------------------- */

static const msxvid_overlay_info k_overlays[] = {
    { MSXVID_OV_GRID,    "grid",    "Tile grid" },
    { MSXVID_OV_SPRITES, "sprites", "Sprite boxes" },
    { MSXVID_OV_BORDER,  "border",  "Border and blanking" },
    { MSXVID_OV_BEAM,    "beam",    "Beam" },
    { MSXVID_OV_HEAT,    "heat",    "Recent writes" },
    { MSXVID_OV_SPLITS,  "splits",  "Register changes" },
};
#define N_OVERLAYS ((int)(sizeof k_overlays / sizeof *k_overlays))

const msxvid_overlay_info *msxvid_overlays(int *count)
{
    if (count) *count = N_OVERLAYS;
    return k_overlays;
}

static const char *const k_view_names[] = { "Display", "Decoded", "Beam", "Values" };
static const char *const k_view_keys[] = { "display", "decoded", "beam", "values" };
static const char *const k_src_names[] = { "Screen", "Patterns", "Sprites", "Palette" };
static const char *const k_src_keys[] = { "screen", "patterns", "sprites", "palette" };
static const char *const k_sec_names[] = { "Pixel", "Registers", "Colours", "Lines", "Timing", "VRAM" };
static const char *const k_sec_keys[] = { "pixel", "registers", "colours", "lines", "timing", "vram" };

const char *msxvid_view_name(int val) { return val >= 0 && val < MSXVID_V_COUNT ? k_view_names[val] : ""; }
const char *msxvid_view_key(int val) { return val >= 0 && val < MSXVID_V_COUNT ? k_view_keys[val] : ""; }
const char *msxvid_source_name(int val) { return val >= 0 && val < MSXVID_SRC_COUNT ? k_src_names[val] : ""; }
const char *msxvid_source_key(int val) { return val >= 0 && val < MSXVID_SRC_COUNT ? k_src_keys[val] : ""; }
const char *msxvid_section_name(int str) { return str >= 0 && str < MSXVID_S_COUNT ? k_sec_names[str] : ""; }
const char *msxvid_section_key(int str) { return str >= 0 && str < MSXVID_S_COUNT ? k_sec_keys[str] : ""; }

int msxvid_section_from_name(const char *name)
{
    if (!name || !*name) return -1;
    if (!strcasecmp(name, "regs")) return MSXVID_S_REGISTERS;
    for (int sec = 0; sec < MSXVID_S_COUNT; sec++)
        if (!strncasecmp(name, k_sec_keys[sec], strlen(name))) return sec;
    return -1;
}

const int *msxvid_zooms(int *count)
{
    if (count) *count = (int)(sizeof k_zooms / sizeof *k_zooms);
    return k_zooms;
}

static const char *colour_name(int index)
{
    static const char *const names[16] = {
        "Transparent", "Black", "Medium green", "Light green", "Dark blue", "Light blue",
        "Dark red", "Cyan", "Medium red", "Light red", "Dark yellow", "Light yellow",
        "Dark green", "Magenta", "Grey", "White" };
    return names[index & 15];
}

/* ---- small helpers ---------------------------------------------------------------------- */

static uint64_t fnv(uint64_t high, const void *pixp, size_t num)
{
    const uint8_t *blu = pixp;
    while (num--) { high ^= *blu++; high *= 1099511628211ull; }
    return high;
}

static uint32_t blend(uint32_t dst, uint32_t rgb, int a256)
{
    const uint32_t dstr = (dst >> 16) & 255, dstg = (dst >> 8) & 255, dstb = dst & 255;
    const uint32_t srcr = (rgb >> 16) & 255, srcg = (rgb >> 8) & 255, srcb = rgb & 255;
    const uint32_t ret = (dstr * (uint32_t)(256 - a256) + srcr * (uint32_t)a256) >> 8;
    const uint32_t grn = (dstg * (uint32_t)(256 - a256) + srcg * (uint32_t)a256) >> 8;
    const uint32_t blu = (dstb * (uint32_t)(256 - a256) + srcb * (uint32_t)a256) >> 8;
    return 0xff000000u | ret << 16 | grn << 8 | blu;
}

static int fits_zoom(int wide, int high, int scale)
{
    return (long)wide * scale * (long)high * scale <= MAXPIX;
}

static void why_set(msxvid_tab *tab, const char *str)
{
    snprintf(tab->why, sizeof tab->why, "%s", str);
}

static int ensure(uint32_t **buf, size_t *cap, size_t need)
{
    if (need > *cap) {
        uint32_t *grown = realloc(*buf, need * sizeof **buf);
        if (!grown) return 0;
        *buf = grown;
        *cap = need;
    }
    return 1;
}

static void rgba_to_pixels(const uint8_t *rgba, uint32_t *dst, size_t count)
{
    for (size_t item = 0; item < count; item++)
        dst[item] = 0xff000000u | (uint32_t)rgba[item * 4] << 16 | (uint32_t)rgba[item * 4 + 1] << 8 | rgba[item * 4 + 2];
}

static uint32_t rgb_of(const uint8_t rgb[3])
{
    return 0xff000000u | (uint32_t)rgb[0] << 16 | (uint32_t)rgb[1] << 8 | rgb[2];
}

/* the false colour of a byte: red climbs with the value, green is a triangle
 * of it, blue falls with the high nibble */
static uint32_t value_colour(unsigned byte)
{
    const int low = (int)(byte & 255), high = (int)(byte >> 4);
    const int tri = low < 128 ? low * 2 : (255 - low) * 2;
    return 0xff000000u | (uint32_t)(40 + low * 215 / 255) << 16 |
           (uint32_t)(40 + tri * 160 / 255) << 8 | (uint32_t)(255 - high * 200 / 15);
}

/* palette words 0x0GRB <-> the "VDP palette" debuggable's two bytes */
static uint16_t grb_of_bytes(const uint8_t *entry)
{
    return (uint16_t)(((entry[1] & 7) << 8) | (entry[0] & 0x77));
}

static void bytes_of_grb(uint16_t grb, uint8_t *entry)
{
    entry[0] = (uint8_t)(grb & 0x77);
    entry[1] = (uint8_t)((grb >> 8) & 7);
}

static int vdp_is_msx1(const msxvid_capture *cap) { return cap->chip == MSXVDP_CHIP_V9918; }

/* ---- geometry: the display's place in the published frame ------------------------------ */

static void compute_geom(const msxvid_tab *tab, geom_rec *out)
{
    const msxvid_capture *cap = tab->cap;
    const msxvid_log *log = &cap->log;
    const uint8_t *regs = log->valid ? log->regs : cap->regs;
    int pal = log->valid ? log->pal : 0;

    /* The same arithmetic as openMSX's VDP::scheduleDisplayStart and
     * SDLRasterizer::frameStart: 3 lines of sync + 13 of top erase + the top
     * border (9 NTSC / 36 PAL, 10 more in 192-line mode) + the vertical
     * adjust; and frame row 0 is absolute line 18 (NTSC) / 45 (PAL). */
    out->lines = (!vdp_is_msx1(cap) && (regs[9] & 0x80)) ? 212 : 192;
    out->wide = tab->mode.line_width;
    out->xoff = 64;
    out->xscale = out->wide == 512 ? 1 : 2;
    out->pal = pal;
    out->zero = 3 + 13 + (pal ? 36 : 9) + ((regs[9] & 0x80) && !vdp_is_msx1(cap) ? 0 : 10) + ((regs[18] >> 4) ^ 7);
    out->top = pal ? 45 : 18;
}

static int frame_y(const geom_rec *geo, int line) { return 2 * (geo->zero - geo->top + line); }

/* ---- the bands of the frame ---------------------------------------------------------------- */

static int band_key_differs(const band_rec *left, const uint8_t *regs, const uint16_t *pal)
{
    static const int k_regs[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 14, 18, 23, 25 };
    for (size_t item = 0; item < sizeof k_regs / sizeof *k_regs; item++)
        if (left->regs[k_regs[item]] != regs[k_regs[item]]) return 1;
    return memcmp(left->pal, pal, sizeof left->pal) != 0;
}

/* Walk the display lines through the log: a new band starts where a register
 * the decode depends on, or a palette entry, differs from the line before. */
static void compute_bands(msxvid_tab *tab)
{
    const msxvid_capture *cap = tab->cap;
    const msxvid_log *log = &cap->log;
    uint8_t regs[32];
    uint16_t pal[16];
    uint32_t event = 0;

    tab->nbands = 0;
    if (log->valid) {
        memcpy(regs, log->regs, sizeof regs);
        memcpy(pal, log->palette, sizeof pal);
    } else {
        memcpy(regs, cap->regs, sizeof regs);
        for (int item = 0; item < 16; item++) pal[item] = grb_of_bytes(&cap->palette[item * 2]);
    }
    for (int line = 0; line < tab->geom.lines; line++) {
        const int absolute = tab->geom.zero + line;
        int writes = 0;
        while (log->valid && event < log->event_count && log->events[event].line <= absolute) {
            const msxvid_event *evt = &log->events[event++];
            if (evt->kind == MSXVID_EV_REG && evt->index < 32) { regs[evt->index] = (uint8_t)evt->value; writes++; }
            else if (evt->kind == MSXVID_EV_PALETTE && evt->index < 16) { pal[evt->index] = evt->value; writes++; }
        }
        if (tab->nbands == 0 || (band_key_differs(&tab->bands[tab->nbands - 1], regs, pal) && tab->nbands < MAX_BANDS)) {
            band_rec *band = &tab->bands[tab->nbands++];
            memcpy(band->regs, regs, sizeof band->regs);
            memcpy(band->pal, pal, sizeof band->pal);
            band->first = line;
            band->last = line;
            band->writes = writes;
        } else {
            tab->bands[tab->nbands - 1].last = line;
        }
    }
    if (tab->nbands == 0) {         /* no lines at all: keep one so callers never index nothing */
        band_rec *band = &tab->bands[tab->nbands++];
        memcpy(band->regs, regs, sizeof band->regs);
        memcpy(band->pal, pal, sizeof band->pal);
        band->first = band->last = 0;
        band->writes = 0;
    }
}

/* ---- new / free / settings -------------------------------------------------------------- */

msxvid_tab *msxvid_tab_new(msxdebug *dbg)
{
    msxvid_tab *tab = calloc(1, sizeof *tab);
    const char *envv;
    if (!tab) return NULL;
    tab->dbg = dbg;
    tab->cap = calloc(1, sizeof *tab->cap);
    tab->last = calloc(1, sizeof *tab->last);
    tab->snap = calloc(1, sizeof *tab->snap);
    if (!tab->cap || !tab->last || !tab->snap) {
        msxvid_tab_free(tab);
        return NULL;
    }
    tab->view = MSXVID_V_DISPLAY;
    tab->source = MSXVID_SRC_SCREEN;
    tab->ovl = MSXVID_OV_SPRITES;
    tab->zoom = 0;
    tab->vieww = MSXVID_FB_W;
    tab->viewh = MSXVID_FB_H;
    tab->hovx = tab->hovy = tab->pinx = tab->piny = -1;
    tab->dirty = 1;

    if ((envv = getenv("MSX_DEBUGGER_VIDEO_VIEW")) && *envv) {
        for (int item = 0; item < MSXVID_V_COUNT; item++)
            if (!strncasecmp(envv, k_view_keys[item], strlen(envv))) { tab->view = item; break; }
    }
    if ((envv = getenv("MSX_DEBUGGER_VIDEO_SOURCE")) && *envv) {
        for (int item = 0; item < MSXVID_SRC_COUNT; item++)
            if (!strncasecmp(envv, k_src_keys[item], strlen(envv))) { tab->source = item; break; }
    }
    if ((envv = getenv("MSX_DEBUGGER_VIDEO_OVERLAYS"))) {
        char buf[200], *tok;
        unsigned ovl = 0;
        snprintf(buf, sizeof buf, "%s", envv);
        for (tok = strtok(buf, ", "); tok; tok = strtok(NULL, ", ")) {
            if (!strcasecmp(tok, "all")) ovl |= MSXVID_OV_ALL;
            else if (!strcasecmp(tok, "none")) ovl = 0;
            else
                for (int item = 0; item < N_OVERLAYS; item++)
                    if (!strcasecmp(tok, k_overlays[item].name)) ovl |= k_overlays[item].bit;
        }
        tab->ovl = ovl;
    }
    if ((envv = getenv("MSX_DEBUGGER_VIDEO_ZOOM")) && *envv) {
        if (!strcasecmp(envv, "fit")) tab->zoom = 0;
        else {
            const int scale = atoi(envv);
            for (size_t item = 1; item < sizeof k_zooms / sizeof *k_zooms; item++)
                if (k_zooms[item] == scale) tab->zoom = scale;
        }
    }
    tab->env_section = MSXVID_S_PIXEL;
    if ((envv = getenv("MSX_DEBUGGER_VIDEO_SECTION")) && *envv) {
        const int sec = msxvid_section_from_name(envv);
        if (sec >= 0) tab->env_section = sec;
    }
    msxhost_capture_enable(1);
    tab->capture_on = 1;
    return tab;
}

void msxvid_tab_free(msxvid_tab *tab)
{
    if (!tab) return;
    if (tab->capture_on)
        msxhost_capture_enable(0);
    free(tab->cap);
    free(tab->last);
    free(tab->snap);
    free(tab->base);
    free(tab->pic);
    free(tab);
}

void msxvid_tab_set_view(msxvid_tab *tab, int val)
{
    if (tab && val >= 0 && val < MSXVID_V_COUNT && val != tab->view) { tab->view = val; tab->dirty = 1; }
}
int msxvid_tab_view(const msxvid_tab *tab) { return tab ? tab->view : 0; }
void msxvid_tab_set_source(msxvid_tab *tab, int val)
{
    if (tab && val >= 0 && val < MSXVID_SRC_COUNT && val != tab->source) { tab->source = val; tab->dirty = 1; }
}
int msxvid_tab_source(const msxvid_tab *tab) { return tab ? tab->source : 0; }
void msxvid_tab_set_overlays(msxvid_tab *tab, unsigned ovl)
{
    if (tab && (ovl & MSXVID_OV_ALL) != tab->ovl) { tab->ovl = ovl & MSXVID_OV_ALL; tab->dirty = 1; }
}
unsigned msxvid_tab_overlays(const msxvid_tab *tab) { return tab ? tab->ovl : 0; }
void msxvid_tab_set_zoom(msxvid_tab *tab, int scale)
{
    if (tab && scale >= 0 && scale <= 16 && scale != tab->zoom) { tab->zoom = scale; tab->dirty = 1; }
}
int msxvid_tab_zoom(const msxvid_tab *tab) { return tab ? tab->zoom : 0; }
void msxvid_tab_set_bank(msxvid_tab *tab, int bank)
{
    if (tab && bank >= 0 && bank < 3 && bank != tab->bank) { tab->bank = bank; tab->dirty = 1; }
}
int msxvid_tab_bank(const msxvid_tab *tab) { return tab ? tab->bank : 0; }
void msxvid_tab_set_vram_base(msxvid_tab *tab, uint32_t addr)
{
    if (tab) tab->vram_base = addr & ~15u;
}
uint32_t msxvid_tab_vram_base(const msxvid_tab *tab) { return tab ? tab->vram_base : 0; }
void msxvid_tab_set_viewport(msxvid_tab *tab, int wide, int high)
{
    if (!tab) return;
    if (wide < 1) wide = 1;
    if (high < 1) high = 1;
    if (wide != tab->vieww || high != tab->viewh) {
        tab->vieww = wide; tab->viewh = high;
        if (tab->zoom == 0) tab->dirty = 1;
    }
}
int msxvid_tab_env_section(const msxvid_tab *tab) { return tab ? tab->env_section : 0; }

/* ---- feeding ----------------------------------------------------------------------------- */

/* Rebuild what the decoders read from the working capture. */
static void present(msxvid_tab *tab)
{
    const msxvid_capture *cap = tab->cap;
    msxvdp_snapshot *snap = tab->snap;
    const uint32_t size = cap->vram_size ? cap->vram_size : 0x4000;

    memset(snap->status, 0, sizeof snap->status);
    memcpy(snap->regs, cap->regs, sizeof snap->regs);
    memcpy(snap->status, cap->status, sizeof snap->status);
    memcpy(snap->palette, cap->palette, sizeof snap->palette);
    memcpy(snap->vram, cap->vram, size < MSXVDP_VRAM_MAX ? size : MSXVDP_VRAM_MAX);
    snap->vram_size = size < MSXVDP_VRAM_MAX ? size : MSXVDP_VRAM_MAX;
    snap->chip = (msxvdp_chip)cap->chip;
    snap->addr = 0;
    msxvdp_decode_mode(snap, &tab->mode);
    compute_geom(tab, &tab->geom);
    compute_bands(tab);
}

void msxvid_tab_feed(msxvid_tab *tab, const msxvid_capture *cap, int stopped)
{
    if (!tab || !cap) return;
    memcpy(tab->cap, cap, sizeof *tab->cap);
    tab->fed = 1;
    tab->have = 1;
    tab->has_fb = cap->fb_valid;
    tab->stopped = stopped != 0;
    tab->has_live = tab->stopped;
    present(tab);
}

/* Take what the machine has. 0 if there is no machine. */
static int acquire(msxvid_tab *tab, int force)
{
    if (tab->fed)
        return tab->have;
    tab->have = 0;
    if (!msxhost_is_running())
        return 0;
    msxhost_capture_get(tab->last, &tab->last_serial);
    tab->has_fb = tab->last->serial != 0 && tab->last->fb_valid;
    tab->stopped = tab->dbg && msxdebug_is_stopped(tab->dbg);
    tab->has_live = tab->stopped;

    /* A parked machine does not change on its own: read it once per stop (the
     * debugger's generation moves when anything ran or was edited) and keep it. */
    if (tab->stopped && !force && tab->live_valid && tab->have_last_live &&
        msxdebug_generation(tab->dbg) == tab->live_gen && tab->last->serial == tab->last_live_serial) {
        tab->have = 1;
        return 1;
    }
    tab->live_valid = 0;
    if (tab->has_fb)
        memcpy(tab->cap, tab->last, sizeof *tab->cap);
    if (tab->stopped) {
        /* a parked machine is ours to read: the registers, palette and VRAM
         * as they stand, and the frame it last published */
        if (!tab->has_fb) tab->cap->fb_valid = 0;
        msxhost_capture_fill(tab->cap);
        tab->live_valid = 1;
        tab->have_last_live = 1;
        tab->live_gen = msxdebug_generation(tab->dbg);
        tab->last_live_serial = tab->last->serial;
    } else if (!tab->has_fb) {
        return 0;
    }
    present(tab);
    tab->have = 1;
    return 1;
}

static uint64_t signature(const msxvid_tab *tab)
{
    const msxvid_capture *cap = tab->cap;
    uint64_t high = 1469598103934665603ull;
    high = fnv(high, cap->regs, sizeof cap->regs);
    high = fnv(high, cap->status, sizeof cap->status);
    high = fnv(high, cap->palette, sizeof cap->palette);
    high = fnv(high, cap->vram, cap->vram_size < MSXVID_VRAM_MAX ? cap->vram_size : MSXVID_VRAM_MAX);
    high = fnv(high, &cap->serial, sizeof cap->serial);
    high = fnv(high, &cap->frame, sizeof cap->frame);
    high = fnv(high, &cap->beam_line, sizeof cap->beam_line);
    high = fnv(high, &cap->beam_cycle, sizeof cap->beam_cycle);
    high = fnv(high, &cap->live, sizeof cap->live);
    high = fnv(high, &cap->log.frame, sizeof cap->log.frame);
    high = fnv(high, &cap->log.event_count, sizeof cap->log.event_count);
    high = fnv(high, &tab->has_live, sizeof tab->has_live);
    high = fnv(high, &tab->has_fb, sizeof tab->has_fb);
    return high;
}

/* The name table: where it is and how many entries the mode has. */
static void name_table(const msxvid_tab *tab, uint32_t *base, int *cols, int *rows)
{
    const msxvdp_snapshot *snap = tab->snap;
    switch (tab->mode.base) {
    case MSXVDP_MODE_TEXT1:
        *base = (uint32_t)(snap->regs[2] & 0x0F) << 10; *cols = 40; *rows = 24; break;
    case MSXVDP_MODE_TEXT2:
        *base = (uint32_t)(snap->regs[2] & 0x7C) << 10; *cols = 80; *rows = 24; break;
    case MSXVDP_MODE_GRAPHIC1: case MSXVDP_MODE_GRAPHIC2: case MSXVDP_MODE_GRAPHIC3:
    case MSXVDP_MODE_MULTICOLOR:
        *base = (uint32_t)(snap->regs[2] & 0x0F) << 10; *cols = 32; *rows = 24; break;
    default:
        *base = 0; *cols = 0; *rows = 0; break;
    }
}

/* What changed since the capture before (registers, name table entries), for
 * the heat overlay and the Registers section. Called once per new capture. */
static void track(msxvid_tab *tab)
{
    const msxvid_capture *cap = tab->cap;
    uint32_t base;
    int cols, rows, cells;

    for (int reg = 0; reg < 64; reg++) {
        if (!tab->tracked) tab->reg_age[reg] = MSXVID_REG_FADE;
        else if (cap->regs[reg] != tab->prev_regs[reg]) tab->reg_age[reg] = 0;
        else if (tab->reg_age[reg] < MSXVID_REG_FADE) tab->reg_age[reg]++;
        tab->prev_regs[reg] = cap->regs[reg];
    }
    memset(tab->reg_writes, 0, sizeof tab->reg_writes);
    for (int reg = 0; reg < 64; reg++) tab->reg_first[reg] = -1;
    if (cap->log.valid)
        for (uint32_t item = 0; item < cap->log.event_count; item++) {
            const msxvid_event *evt = &cap->log.events[item];
            if (evt->kind == MSXVID_EV_PALETTE || evt->index >= 64) continue;
            tab->reg_writes[evt->index]++;
            if (tab->reg_first[evt->index] < 0) tab->reg_first[evt->index] = evt->line;
        }

    name_table(tab, &base, &cols, &rows);
    cells = cols * rows;
    if (cells > NT_MAX) cells = NT_MAX;
    if (cells != tab->nt_cells || base != tab->nt_base || cols != tab->nt_cols) {
        memset(tab->heat, 0, sizeof tab->heat);
        tab->nt_cells = cells;
        tab->nt_base = base;
        tab->nt_cols = cols;
        for (int item = 0; item < cells; item++) tab->prev_nt[item] = cap->vram[(base + (uint32_t)item) % MSXVID_VRAM_MAX];
        tab->tracked = 1;
        return;
    }
    for (int item = 0; item < cells; item++) {
        const uint8_t now = cap->vram[(base + (uint32_t)item) % MSXVID_VRAM_MAX];
        if (tab->tracked && now != tab->prev_nt[item]) tab->heat[item] = HEAT_FRAMES;
        else if (tab->heat[item] > 0) tab->heat[item]--;
        tab->prev_nt[item] = now;
    }
    tab->tracked = 1;
}

/* ---- the base picture (1x) -------------------------------------------------------------------- */

static int is_tile_mode(const msxvdp_mode_info *mode)
{
    return mode->base == MSXVDP_MODE_GRAPHIC1 || mode->base == MSXVDP_MODE_GRAPHIC2 ||
           mode->base == MSXVDP_MODE_GRAPHIC3 || mode->base == MSXVDP_MODE_MULTICOLOR ||
           mode->base == MSXVDP_MODE_TEXT1;
}

/* a snapshot with the registers and palette of one band */
static void apply_band(msxvid_tab *tab, const band_rec *band)
{
    msxvdp_snapshot *snap = tab->snap;
    memcpy(snap->regs, band->regs, sizeof band->regs);
    for (int item = 0; item < 16; item++) bytes_of_grb(band->pal[item], &snap->palette[item * 2]);
}

static void restore_state(msxvid_tab *tab)
{
    memcpy(tab->snap->regs, tab->cap->regs, sizeof tab->snap->regs);
    memcpy(tab->snap->palette, tab->cap->palette, sizeof tab->snap->palette);
}

/* The Screen source: each band rendered with its own registers, so a split
 * screen, a scroll or a palette change down the frame shows as it was drawn. */
static int build_screen(msxvid_tab *tab)
{
    msxvdp_mode_info mode0;
    uint8_t *rgba, *scratch;
    int wide = 256, high = 192, tile, renders = 0;
    uint32_t *dst;

    apply_band(tab, &tab->bands[0]);
    msxvdp_decode_mode(tab->snap, &mode0);
    tile = is_tile_mode(&mode0);
    if (!tile && !msxvdp_bitmap_geometry(tab->snap, &wide, &high, NULL, NULL)) {
        restore_state(tab);
        if (mode0.base == MSXVDP_MODE_TEXT2)
            why_set(tab, "Text 2 (80 columns) is not decoded. The Display view shows the frame.");
        else if (mode0.yjk)
            why_set(tab, "YJK/YAE colour is not decoded. The Display view shows the frame.");
        else
            why_set(tab, "This mode is not decoded. The Display view shows the frame.");
        return 0;
    }
    rgba = malloc((size_t)512 * 256 * 4);
    scratch = malloc((size_t)512 * 256 * 4);
    if (!rgba || !scratch || !ensure(&tab->base, &tab->base_cap, (size_t)wide * (size_t)high)) {
        free(rgba); free(scratch);
        restore_state(tab);
        return 0;
    }
    dst = tab->base;
    for (int item = 0; item < wide * high; item++) dst[item] = COL_PANEL;
    for (int bnd = 0; bnd < tab->nbands; bnd++) {
        const band_rec *band = &tab->bands[bnd];
        msxvdp_mode_info mode;
        int bandw = 0, bandh = 0, rendered;
        apply_band(tab, band);
        msxvdp_decode_mode(tab->snap, &mode);
        if (renders >= MAX_BAND_RENDERS) break;
        renders++;
        if (tile && is_tile_mode(&mode)) {
            msxvdp_render_nametable(tab->snap, scratch);
            bandw = 256; bandh = 192; rendered = 1;
        } else if (!tile && !is_tile_mode(&mode)) {
            rendered = msxvdp_render_bitmap(tab->snap, scratch, &bandw, &bandh);
        } else {
            rendered = 0;
        }
        if (!rendered || bandw != wide) continue;     /* a band in another kind of mode stays the panel colour */
        for (int line = band->first; line <= band->last && line < high && line < bandh; line++)
            rgba_to_pixels(scratch + (size_t)line * (size_t)wide * 4, dst + (size_t)line * (size_t)wide, (size_t)wide);
    }
    free(rgba); free(scratch);
    restore_state(tab);
    tab->basew = wide; tab->baseh = high;
    tab->tile_w = mode0.base == MSXVDP_MODE_TEXT1 ? 6 : 8;
    tab->kind = BK_SCREEN;
    return 1;
}

static int build_decoded(msxvid_tab *tab)
{
    uint8_t *rgba;
    const msxvdp_snapshot *snap = tab->snap;

    switch (tab->source) {
    case MSXVID_SRC_PATTERNS:
        if (!is_tile_mode(&tab->mode)) {
            why_set(tab, tab->mode.is_bitmap ? "A bitmap mode has no pattern table: its page is the picture."
                                             : "This mode's patterns are not decoded.");
            return 0;
        }
        rgba = malloc((size_t)256 * 64 * 4);
        if (!rgba || !ensure(&tab->base, &tab->base_cap, (size_t)256 * 64)) { free(rgba); return 0; }
        msxvdp_render_patterns(snap, tab->bank, rgba);
        rgba_to_pixels(rgba, tab->base, (size_t)256 * 64);
        free(rgba);
        tab->basew = 256; tab->baseh = 64;
        tab->kind = BK_PATTERNS;
        return 1;
    case MSXVID_SRC_SPRITES:
        if (tab->mode.sprite_mode != 1) {
            why_set(tab, tab->mode.sprite_mode == 2 ? "Sprite mode 2 (Graphic 3 and the bitmap modes) is not decoded."
                                                    : "This mode has no sprites.");
            return 0;
        }
        rgba = malloc((size_t)128 * 64 * 4);
        if (!rgba || !ensure(&tab->base, &tab->base_cap, (size_t)128 * 64)) { free(rgba); return 0; }
        msxvdp_render_sprites(snap, rgba, NULL);
        for (int item = 0; item < 128 * 64; item++) {
            const uint8_t *src = &rgba[item * 4];
            tab->base[item] = src[3] ? (0xff000000u | (uint32_t)src[0] << 16 | (uint32_t)src[1] << 8 | src[2]) : COL_PANEL;
        }
        free(rgba);
        tab->basew = 128; tab->baseh = 64;
        tab->kind = BK_SPRITES;
        return 1;
    case MSXVID_SRC_PALETTE:
        rgba = malloc((size_t)MSXVDP_PAL_W * MSXVDP_PAL_H * 4);
        if (!rgba || !ensure(&tab->base, &tab->base_cap, (size_t)MSXVDP_PAL_W * MSXVDP_PAL_H)) { free(rgba); return 0; }
        msxvdp_render_palette(snap, rgba);
        rgba_to_pixels(rgba, tab->base, (size_t)MSXVDP_PAL_W * MSXVDP_PAL_H);
        free(rgba);
        tab->basew = MSXVDP_PAL_W; tab->baseh = MSXVDP_PAL_H;
        tab->kind = BK_PALETTE;
        return 1;
    default:
        return build_screen(tab);
    }
}

/* The false colour of each name table entry, or of each byte of the page. */
static int build_values(msxvid_tab *tab)
{
    const msxvdp_snapshot *snap = tab->snap;
    uint32_t base;
    int cols, rows, wide, high, pitch;
    const uint32_t size = snap->vram_size ? snap->vram_size : 1;

    name_table(tab, &base, &cols, &rows);
    if (cols > 0) {
        const int cell_w = tab->mode.base == MSXVDP_MODE_TEXT1 ? 6 : tab->mode.base == MSXVDP_MODE_TEXT2 ? 6 : 8;
        if (!ensure(&tab->base, &tab->base_cap, (size_t)cols * (size_t)cell_w * (size_t)rows * 8)) return 0;
        for (int cell = 0; cell < cols * rows; cell++) {
            const uint32_t col = value_colour(snap->vram[(base + (uint32_t)cell) % size]);
            const int orgx = (cell % cols) * cell_w, orgy = (cell / cols) * 8;
            for (int pixy = 0; pixy < 8; pixy++)
                for (int pixx = 0; pixx < cell_w; pixx++)
                    tab->base[(size_t)(orgy + pixy) * (size_t)(cols * cell_w) + (size_t)(orgx + pixx)] = col;
        }
        tab->basew = cols * cell_w; tab->baseh = rows * 8;
        tab->tile_w = cell_w;
        tab->kind = BK_VALUES;
        return 1;
    }
    if (msxvdp_bitmap_geometry(snap, &wide, &high, &base, &pitch)) {
        const int planar = tab->mode.is_planar, span = wide / pitch;
        if (!ensure(&tab->base, &tab->base_cap, (size_t)wide * (size_t)high)) return 0;
        for (int line = 0; line < high; line++)
            for (int byte = 0; byte < pitch; byte++) {
                uint32_t vaddr = base + (uint32_t)((line + snap->regs[23]) & 255) * (uint32_t)pitch + (uint32_t)byte;
                const uint32_t col = value_colour(snap->vram[(planar ? (((vaddr & 1u) << 16) | (vaddr >> 1)) : vaddr) % size]);
                for (int pixx = 0; pixx < span; pixx++)
                    tab->base[(size_t)line * (size_t)wide + (size_t)(byte * span + pixx)] = col;
            }
        tab->basew = wide; tab->baseh = high;
        tab->tile_w = span;
        tab->kind = BK_VALUES;
        return 1;
    }
    why_set(tab, "This mode has no name table or page to show.");
    return 0;
}

static int build_base(msxvid_tab *tab)
{
    const msxvid_capture *cap = tab->cap;

    if (!tab->has_fb && !tab->has_live) {
        why_set(tab, "No frame has been published yet. Let the machine run for a moment.");
        return 0;
    }
    switch (tab->view) {
    case MSXVID_V_DISPLAY:
    case MSXVID_V_BEAM: {
        int below = 1 << 30;
        if (!cap->fb_valid) {
            why_set(tab, "No frame has been published yet. Let the machine run for a moment.");
            return 0;
        }
        if (tab->view == MSXVID_V_BEAM) {
            if (!tab->has_live) {
                why_set(tab, "The Beam view needs the machine stopped: it shows the published frame with the lines the beam has not reached yet dimmed.");
                return 0;
            }
            if (cap->beam_line >= 0) below = (cap->beam_line - tab->geom.top + 1) * 2;
        }
        if (!ensure(&tab->base, &tab->base_cap, (size_t)MSXVID_FB_W * MSXVID_FB_H)) return 0;
        for (int posy = 0; posy < MSXVID_FB_H; posy++)
            for (int posx = 0; posx < MSXVID_FB_W; posx++) {
                const uint32_t col = 0xff000000u | cap->pixels[(size_t)posy * MSXVID_FB_W + (size_t)posx];
                tab->base[(size_t)posy * MSXVID_FB_W + (size_t)posx] = posy < below ? col : blend(col, 0x000000, 150);
            }
        tab->basew = MSXVID_FB_W; tab->baseh = MSXVID_FB_H;
        tab->kind = BK_FRAME;
        return 1;
    }
    case MSXVID_V_DECODED:
        return build_decoded(tab);
    default:
        return build_values(tab);
    }
}

/* ---- overlays ---------------------------------------------------------------------------- */

typedef struct { uint32_t *pixp; int wide, high; } canvas;

static void px_blend(canvas *cnv, int posx, int posy, uint32_t rgb, int alpha)
{
    if (posx >= 0 && posy >= 0 && posx < cnv->wide && posy < cnv->high)
        cnv->pixp[(size_t)posy * (size_t)cnv->wide + (size_t)posx] = blend(cnv->pixp[(size_t)posy * (size_t)cnv->wide + (size_t)posx], rgb, alpha);
}
static void hline(canvas *cnv, int posy, int minx, int maxx, uint32_t rgb, int alpha)
{
    for (int posx = minx; posx < maxx; posx++) px_blend(cnv, posx, posy, rgb, alpha);
}
static void vline(canvas *cnv, int posx, int starty, int endy, uint32_t rgb, int alpha)
{
    for (int posy = starty; posy < endy; posy++) px_blend(cnv, posx, posy, rgb, alpha);
}
static void fill_rect(canvas *cnv, int posx, int posy, int wide, int high, uint32_t rgb, int alpha)
{
    for (int offy = 0; offy < high; offy++)
        for (int offx = 0; offx < wide; offx++) px_blend(cnv, posx + offx, posy + offy, rgb, alpha);
}
static void frame_rect(canvas *cnv, int posx, int posy, int wide, int high, int thick, uint32_t rgb, int alpha)
{
    for (int edge = 0; edge < thick; edge++) {
        hline(cnv, posy + edge, posx, posx + wide, rgb, alpha);
        hline(cnv, posy + high - 1 - edge, posx, posx + wide, rgb, alpha);
        vline(cnv, posx + edge, posy, posy + high, rgb, alpha);
        vline(cnv, posx + wide - 1 - edge, posy, posy + high, rgb, alpha);
    }
}

/* ---- sprites (mode 1: the TMS9918A-compatible ones) ---------------------------------------- */

typedef struct {
    int live;                   /* before the end marker */
    int xleft, ytop, size;             /* VDP pixels, size = the box edge */
    int pattern, colour, early;
} sprite_rec;

static void sprite_geometry(const msxvdp_snapshot *snap, int index, sprite_rec *out)
{
    const uint32_t size = snap->vram_size ? snap->vram_size : 1;
    const uint32_t sat = (uint32_t)(snap->regs[5] & 0x7F) << 7;
    const int size16 = (snap->regs[1] & 2) != 0, mag = (snap->regs[1] & 1) != 0;
    int live = 1;

    for (int item = 0; item < index && live; item++)
        if (snap->vram[(sat + (uint32_t)item * 4) % size] == 0xD0) live = 0;
    {
        const uint8_t *entry = &snap->vram[(sat + (uint32_t)index * 4) % size];
        int ypos = entry[0];
        memset(out, 0, sizeof *out);
        if (entry[0] == 0xD0) live = 0;
        if (ypos >= 0xE0) ypos -= 256;
        out->live = live;
        out->ytop = ypos + 1;
        out->early = (entry[3] & 0x80) != 0;
        out->xleft = entry[1] - (out->early ? 32 : 0);
        out->pattern = size16 ? (entry[2] & 0xFC) : entry[2];
        out->colour = entry[3] & 15;
        out->size = (size16 ? 16 : 8) * (mag ? 2 : 1);
    }
}

/* the bit of the sprite's pattern at box offset (offx, offy): 1 lit */
static int sprite_bit(const msxvdp_snapshot *snap, const sprite_rec *spr, int offx, int offy, uint32_t *addr)
{
    const uint32_t size = snap->vram_size ? snap->vram_size : 1;
    const uint32_t spg = (uint32_t)(snap->regs[6] & 0x3F) << 11;
    const int size16 = (snap->regs[1] & 2) != 0, mag = (snap->regs[1] & 1) != 0;
    const int stepx = mag ? offx / 2 : offx, stepy = mag ? offy / 2 : offy;
    /* a 16 x 16 sprite is four 8 x 8 patterns: upper left, lower left, upper right, lower right */
    const uint32_t quad = size16 ? (uint32_t)((stepx >> 3) * 2 + (stepy >> 3)) : 0u;
    const uint32_t vaddr = spg + ((uint32_t)spr->pattern + quad) * 8 + (uint32_t)(stepy & 7);
    if (addr) *addr = vaddr;
    return (snap->vram[vaddr % size] & (0x80 >> (stepx & 7))) != 0;
}

/* ---- drawing the overlays ------------------------------------------------------------------ */

typedef struct { int orgx, stepx, orgy, stepy; } mapping;     /* picture pixel = origin + VDP coordinate * scale */

static mapping picture_mapping(const msxvid_tab *tab)
{
    mapping map = { 0, 1, 0, 1 };
    if (tab->kind == BK_FRAME) {
        map.orgx = tab->geom.xoff; map.stepx = tab->geom.xscale;
        map.orgy = frame_y(&tab->geom, 0); map.stepy = 2;
    }
    return map;
}

static void draw_overlays(msxvid_tab *tab, canvas *cnv, int scale)
{
    const msxvid_capture *cap = tab->cap;
    const geom_rec *geo = &tab->geom;
    const int framed = tab->kind == BK_FRAME;
    const int screened = tab->kind == BK_SCREEN;
    const int valued = tab->kind == BK_VALUES;
    const mapping map = picture_mapping(tab);

    if (!framed && !screened && !valued)
        return;
    if (framed && (tab->ovl & MSXVID_OV_BORDER)) {
        const uint32_t tint = 0xff6040c0;
        const int left = map.orgx * scale, top = map.orgy * scale;
        const int wide = geo->wide * map.stepx * scale, high = geo->lines * map.stepy * scale;
        fill_rect(cnv, 0, 0, MSXVID_FB_W * scale, top, tint, 70);
        fill_rect(cnv, 0, top + high, MSXVID_FB_W * scale, MSXVID_FB_H * scale - top - high, tint, 70);
        fill_rect(cnv, 0, top, left, high, tint, 70);
        fill_rect(cnv, left + wide, top, MSXVID_FB_W * scale - left - wide, high, tint, 70);
    }
    if ((tab->ovl & MSXVID_OV_HEAT) && !valued && tab->nt_cols > 0 && tab->tracked) {
        const int cell_w = tab->mode.base == MSXVDP_MODE_TEXT1 ? 6 : 8;
        const int xorg = tab->mode.base == MSXVDP_MODE_TEXT1 ? 8 : 0;
        for (int cell = 0; cell < tab->nt_cells; cell++)
            if (tab->heat[cell] > 0)
                fill_rect(cnv, (map.orgx + (xorg + (cell % tab->nt_cols) * cell_w) * map.stepx) * scale,
                          (map.orgy + (cell / tab->nt_cols) * 8 * map.stepy) * scale,
                          cell_w * map.stepx * scale, 8 * map.stepy * scale, 0xffff8020, 60 + tab->heat[cell] * 6);
    }
    if ((tab->ovl & MSXVID_OV_HEAT) && valued && tab->nt_cols > 0 && tab->tracked) {
        for (int cell = 0; cell < tab->nt_cells; cell++)
            if (tab->heat[cell] > 0)
                fill_rect(cnv, (cell % tab->nt_cols) * tab->tile_w * scale, (cell / tab->nt_cols) * 8 * scale,
                          tab->tile_w * scale, 8 * scale, 0xffff8020, 60 + tab->heat[cell] * 6);
    }
    if ((tab->ovl & MSXVID_OV_GRID) && scale >= 1 && (framed || screened || valued) && tab->nt_cols > 0) {
        const int cell_w = valued ? tab->tile_w : (tab->mode.base == MSXVDP_MODE_TEXT1 ? 6 : 8);
        const int xorg = (!valued && tab->mode.base == MSXVDP_MODE_TEXT1) ? 8 : 0;
        const int rows = tab->nt_cells / tab->nt_cols;
        const mapping use = valued ? (mapping){ 0, 1, 0, 1 } : map;
        for (int col = 0; col <= tab->nt_cols; col++)
            vline(cnv, (use.orgx + (xorg + col * cell_w) * use.stepx) * scale - (col == tab->nt_cols), use.orgy * scale,
                  (use.orgy + rows * 8 * use.stepy) * scale, 0xffffffff, 60);
        for (int row = 0; row <= rows; row++)
            hline(cnv, (use.orgy + row * 8 * use.stepy) * scale - (row == rows), (use.orgx + xorg * use.stepx) * scale,
                  (use.orgx + (xorg + tab->nt_cols * cell_w) * use.stepx) * scale, 0xffffffff, 60);
    }
    if ((tab->ovl & MSXVID_OV_SPLITS) && (framed || screened) && tab->nbands > 1) {
        for (int bnd = 1; bnd < tab->nbands; bnd++) {
            const int posy = (map.orgy + tab->bands[bnd].first * map.stepy) * scale;
            hline(cnv, posy, map.orgx * scale, (map.orgx + geo->wide * map.stepx) * scale, 0xff40ff80, 220);
        }
    }
    if ((tab->ovl & MSXVID_OV_SPRITES) && (framed || screened) && tab->mode.sprite_mode == 1) {
        for (int item = 0; item < SPRITES; item++) {
            sprite_rec spr;
            sprite_geometry(tab->snap, item, &spr);
            if (!spr.live) break;
            frame_rect(cnv, (map.orgx + spr.xleft * map.stepx) * scale, (map.orgy + spr.ytop * map.stepy) * scale,
                       spr.size * map.stepx * scale, spr.size * map.stepy * scale, scale < 3 ? 1 : 2,
                       0xff000000u | (uint32_t)(60 + (item * 37) % 196) << 16 | (uint32_t)(255 - (item * 53) % 180) << 8 | (uint32_t)(90 + (item * 71) % 166), 220);
        }
    }
    if ((tab->ovl & MSXVID_OV_BEAM) && tab->has_live && framed && cap->beam_line >= 0) {
        const int posy = (cap->beam_line - geo->top) * 2;
        if (posy >= 0 && posy < MSXVID_FB_H) hline(cnv, posy * scale, 0, tab->basew * scale, 0xffff4040, 230);
    }
}

static int pick_zoom(const msxvid_tab *tab)
{
    int scale = tab->zoom;
    if (scale == 0) {
        scale = 1;
        for (size_t item = 1; item < sizeof k_zooms / sizeof *k_zooms; item++)
            if (tab->basew * k_zooms[item] <= tab->vieww && tab->baseh * k_zooms[item] <= tab->viewh)
                scale = k_zooms[item];
    }
    while (scale > 1 && !fits_zoom(tab->basew, tab->baseh, scale)) {
        int lower = 1;
        for (size_t item = 1; item < sizeof k_zooms / sizeof *k_zooms; item++)
            if (k_zooms[item] < scale) lower = k_zooms[item];
        scale = lower;
    }
    return scale;
}

static void build_picture(msxvid_tab *tab)
{
    int scale;
    canvas cnv;

    tab->why[0] = '\0';
    tab->picw = tab->pich = 0;
    if (!build_base(tab))
        return;
    scale = pick_zoom(tab);
    tab->effzoom = scale;
    tab->picw = tab->basew * scale;
    tab->pich = tab->baseh * scale;
    if (!ensure(&tab->pic, &tab->pic_cap, (size_t)tab->picw * (size_t)tab->pich)) {
        tab->picw = tab->pich = 0;
        why_set(tab, "Out of memory.");
        return;
    }
    if (scale == 1)
        memcpy(tab->pic, tab->base, (size_t)tab->basew * (size_t)tab->baseh * sizeof *tab->pic);
    else
        for (int posy = 0; posy < tab->pich; posy++) {
            const uint32_t *src = &tab->base[(size_t)(posy / scale) * (size_t)tab->basew];
            uint32_t *dst = &tab->pic[(size_t)posy * (size_t)tab->picw];
            for (int posx = 0; posx < tab->picw; posx++)
                dst[posx] = src[posx / scale];
        }
    cnv.pixp = tab->pic; cnv.wide = tab->picw; cnv.high = tab->pich;
    draw_overlays(tab, &cnv, scale);
}

/* ---- the inspector ----------------------------------------------------------------------- */

static void add_break(msxvid_pixel *outp, int kind, unsigned lowest, unsigned highest)
{
    if (outp->nbreak < 3) {
        outp->break_kind[outp->nbreak] = kind;
        outp->break_lo[outp->nbreak] = lowest;
        outp->break_hi[outp->nbreak] = highest;
        outp->nbreak++;
    }
}

static unsigned phys_addr(const msxvid_tab *tab, uint32_t logical)
{
    return tab->mode.is_planar ? (((logical & 1u) << 16) | (logical >> 1)) : logical;
}

typedef struct {
    int valid;
    int col, row, cols;
    int name;
    uint32_t name_addr, pattern_addr, tile_lo, tile_hi, colour_addr;
    int forec, backc, lit, colour;
    int cell_x, cell_w;         /* the cell's VDP x and width */
} tile_rec;

/* the tile under VDP pixel (vx, vy) in a name table mode */
static void tile_at(const msxvid_tab *tab, int vdpx, int vdpy, tile_rec *out)
{
    const msxvdp_snapshot *snap = tab->snap;
    const uint32_t size = snap->vram_size ? snap->vram_size : 1;
    uint32_t nametab;
    int cols, rows, col, row, name, xin, cell_w, xorg;
    const int base = tab->mode.base;
    const int graphic2 = base == MSXVDP_MODE_GRAPHIC2 || base == MSXVDP_MODE_GRAPHIC3;

    memset(out, 0, sizeof *out);
    out->name = -1;
    out->colour = -1;
    name_table(tab, &nametab, &cols, &rows);
    if (cols == 0 || !is_tile_mode(&tab->mode)) return;
    cell_w = base == MSXVDP_MODE_TEXT1 ? 6 : 8;
    xorg = base == MSXVDP_MODE_TEXT1 ? 8 : 0;
    if (vdpx < xorg || vdpx >= xorg + cols * cell_w || vdpy < 0 || vdpy >= rows * 8) return;
    col = (vdpx - xorg) / cell_w;
    row = vdpy / 8;
    xin = (vdpx - xorg) % cell_w;
    name = snap->vram[(nametab + (uint32_t)(row * cols + col)) % size];
    out->valid = 1;
    out->col = col; out->row = row; out->cols = cols;
    out->name = name;
    out->name_addr = nametab + (uint32_t)(row * cols + col);
    out->cell_x = xorg + col * cell_w; out->cell_w = cell_w;
    {
        const uint32_t patgen = graphic2 ? (uint32_t)(snap->regs[4] & 0x04) << 11 : (uint32_t)(snap->regs[4] & 0x07) << 11;
        const uint32_t coltab = graphic2 ? (uint32_t)(snap->regs[3] & 0x80) << 6 : (uint32_t)snap->regs[3] << 6;
        const uint32_t pg_mask = ((uint32_t)(snap->regs[4] & 0x03) << 8) | 0xFF;
        const uint32_t ct_mask = ((uint32_t)(snap->regs[3] & 0x7F) << 3) | 0x07;
        uint8_t bits = 0, colour = 0;
        if (graphic2) {
            const uint32_t bank = (uint32_t)(row / 8) * 256;
            out->tile_lo = patgen + ((bank + (uint32_t)name) & pg_mask) * 8;
            out->tile_hi = out->tile_lo + 7;
            out->pattern_addr = out->tile_lo + (uint32_t)(vdpy & 7);
            out->colour_addr = coltab + (((bank + (uint32_t)name) & ct_mask) * 8) + (uint32_t)(vdpy & 7);
            bits = snap->vram[out->pattern_addr % size];
            colour = snap->vram[out->colour_addr % size];
            out->forec = colour >> 4; out->backc = colour & 15;
            out->lit = (bits & (0x80 >> xin)) != 0;
        } else if (base == MSXVDP_MODE_MULTICOLOR) {
            out->tile_lo = patgen + (uint32_t)name * 8;
            out->tile_hi = out->tile_lo + 7;
            out->pattern_addr = out->tile_lo + (uint32_t)(row & 3) * 2 + (uint32_t)((vdpy & 7) >> 2);
            colour = snap->vram[out->pattern_addr % size];
            out->colour_addr = out->pattern_addr;
            out->forec = xin < 4 ? colour >> 4 : colour & 15;
            out->backc = out->forec;
            out->lit = 1;
        } else {
            out->tile_lo = patgen + (uint32_t)name * 8;
            out->tile_hi = out->tile_lo + 7;
            out->pattern_addr = out->tile_lo + (uint32_t)(vdpy & 7);
            bits = snap->vram[out->pattern_addr % size];
            if (base == MSXVDP_MODE_TEXT1) {
                out->colour_addr = NO_ADDR;
                out->forec = snap->regs[7] >> 4; out->backc = snap->regs[7] & 15;
            } else {
                out->colour_addr = coltab + (uint32_t)name / 8;
                colour = snap->vram[out->colour_addr % size];
                out->forec = colour >> 4; out->backc = colour & 15;
            }
            out->lit = (bits & (0x80 >> xin)) != 0;
        }
        out->colour = out->lit ? out->forec : out->backc;
        if (out->colour == 0) out->colour = snap->regs[7] & 15;     /* transparent: the backdrop shows */
    }
}

typedef struct {
    int valid;
    int vdpx, vdpy;
    uint32_t logical, phys;
    uint8_t byte;
    int colour;
} bitmap_rec;

static void bitmap_at(const msxvid_tab *tab, int vdpx, int vdpy, bitmap_rec *out)
{
    const msxvdp_snapshot *snap = tab->snap;
    const uint32_t size = snap->vram_size ? snap->vram_size : 1;
    uint32_t base;
    int wide, high, pitch;

    memset(out, 0, sizeof *out);
    out->colour = -1;
    if (!msxvdp_bitmap_geometry(snap, &wide, &high, &base, &pitch)) return;
    if (vdpx < 0 || vdpy < 0 || vdpx >= wide || vdpy >= high) return;
    {
        const int bits = wide == 256 && pitch == 256 ? 8 : (wide == 512 && pitch == 128 ? 2 : 4);
        const int per_byte = 8 / bits;
        const uint32_t logical = base + (uint32_t)((vdpy + snap->regs[23]) & 255) * (uint32_t)pitch + (uint32_t)(vdpx / per_byte);
        out->valid = 1;
        out->vdpx = vdpx; out->vdpy = vdpy;
        out->logical = logical;
        out->phys = phys_addr(tab, logical);
        out->byte = snap->vram[out->phys % size];
        if (bits == 8) out->colour = -1;     /* Graphic 7: the byte is the colour */
        else out->colour = (out->byte >> (8 - bits * (1 + vdpx % per_byte))) & ((1 << bits) - 1);
    }
}

static int sprites_at(const msxvid_tab *tab, int vdpx, int vdpy, int *front, int *front_pixel_lit, uint32_t *mask, uint32_t *pat_addr)
{
    int any = 0;
    *front = -1; *mask = 0; *front_pixel_lit = 0; *pat_addr = NO_ADDR;
    if (tab->mode.sprite_mode != 1) return 0;
    for (int item = 0; item < SPRITES; item++) {
        sprite_rec spr;
        sprite_geometry(tab->snap, item, &spr);
        if (!spr.live) break;
        if (vdpx < spr.xleft || vdpx >= spr.xleft + spr.size || vdpy < spr.ytop || vdpy >= spr.ytop + spr.size) continue;
        *mask |= 1u << item;
        any = 1;
        {
            uint32_t vaddr;
            const int lit = sprite_bit(tab->snap, &spr, vdpx - spr.xleft, vdpy - spr.ytop, &vaddr);
            if (*front < 0 && lit && spr.colour) { *front = item; *front_pixel_lit = 1; *pat_addr = vaddr; }
        }
    }
    return any;
}

static int text_add(char *dst, size_t size, int num, const char *fmt, ...)
{
    va_list args;
    int add;
    if (num >= (int)size - 1) return num;
    va_start(args, fmt);
    add = vsnprintf(dst + num, size - (size_t)num, fmt, args);
    va_end(args);
    if (add < 0) return num;
    return num + add < (int)size ? num + add : (int)size - 1;
}

static const char *mode_class(const msxvid_tab *tab)
{
    return tab->mode.is_bitmap ? "bitmap" : tab->mode.is_text ? "text" : "tile";
}

/* The VDP pixel (vx, vy) of the display: what it is made of, appended to the
 * pixel's text, and the break targets. line is the display line. */
static int describe_vdp_pixel(msxvid_tab *tab, int vdpx, int vdpy, msxvid_pixel *outp, int num)
{
    const msxvdp_snapshot *snap = tab->snap;
    const geom_rec *geo = &tab->geom;
    char *dst = outp->text;
    const size_t size = sizeof outp->text;
    int front, lit;
    uint32_t mask, spr_addr;

    outp->vdp_x = vdpx; outp->vdp_y = vdpy;
    outp->line = geo->zero + vdpy;
    if (vdpx < 0 || vdpy < 0 || vdpx >= geo->wide || vdpy >= geo->lines) {
        outp->kind = MSXVID_PK_BORDER;
        num = text_add(dst, size, num, "Border or blanking (line %d of the frame): the backdrop colour %s (%d) from R#7\n",
                       outp->line, colour_name(snap->regs[7] & 15), snap->regs[7] & 15);
        outp->colour = snap->regs[7] & 15;
        add_break(outp, MSXVID_BK_REGISTER, 7, 7);
        return num;
    }
    num = text_add(dst, size, num, "VDP pixel %d,%d, display line %d = frame line %d (%s mode)\n", vdpx, vdpy, vdpy, outp->line, mode_class(tab));
    sprites_at(tab, vdpx, vdpy, &front, &lit, &mask, &spr_addr);
    if (tab->mode.sprite_mode == 2)
        num = text_add(dst, size, num, "(Sprite mode 2: this mode's sprites are not decoded, so one may cover this pixel)\n");
    outp->sprite = front;
    outp->sprite_mask = mask;
    if (front >= 0) {
        sprite_rec spr;
        sprite_geometry(snap, front, &spr);
        outp->kind = MSXVID_PK_SPRITE;
        outp->colour = spr.colour;
        outp->markx = spr.xleft; outp->marky = spr.ytop; outp->markw = spr.size; outp->markh = spr.size;
        outp->vram_addr = spr_addr;
        num = text_add(dst, size, num, "Sprite %d in front (attribute y=%d x=%d pattern %d colour %s (%d)), pattern byte at $%04X\n",
                       front, spr.ytop - 1, spr.xleft + (spr.early ? 32 : 0), spr.pattern, colour_name(spr.colour), spr.colour, spr_addr);
        if (mask & ~(1u << front)) {
            num = text_add(dst, size, num, "Also under the pointer:");
            for (int item = 0; item < SPRITES; item++)
                if ((mask & (1u << item)) && item != front) num = text_add(dst, size, num, " %d", item);
            num = text_add(dst, size, num, "\n");
        }
        {
            const unsigned sat = (unsigned)(snap->regs[5] & 0x7F) << 7;
            add_break(outp, MSXVID_BK_VRAM, sat + (unsigned)front * 4, sat + (unsigned)front * 4 + 3);
            add_break(outp, MSXVID_BK_VRAM, spr_addr & ~7u, (spr_addr & ~7u) + 7);
        }
        return num;
    }
    if (is_tile_mode(&tab->mode)) {
        tile_rec tile;
        tile_at(tab, vdpx, vdpy, &tile);
        if (tile.valid) {
            outp->kind = MSXVID_PK_TILE;
            outp->cell = tile.row * tile.cols + tile.col;
            outp->name = tile.name;
            outp->colour = tile.colour;
            outp->vram_addr = tile.name_addr;
            outp->markx = tile.cell_x; outp->marky = tile.row * 8; outp->markw = tile.cell_w; outp->markh = 8;
            num = text_add(dst, size, num, "Name table entry %d (column %d, row %d) at $%04X holds tile %d ($%02X)\n",
                           outp->cell, tile.col, tile.row, tile.name_addr, tile.name, tile.name);
            if (tab->mode.base == MSXVDP_MODE_MULTICOLOR)
                num = text_add(dst, size, num, "Colour block %s (%d) from the pattern at $%04X\n",
                               colour_name(tile.forec), tile.forec, tile.pattern_addr);
            else
                num = text_add(dst, size, num, "Pattern row at $%04X (the tile's 8 bytes $%04X-$%04X), pixel %s\n",
                               tile.pattern_addr, tile.tile_lo, tile.tile_hi, tile.lit ? "set" : "clear");
            if (tile.colour_addr != NO_ADDR && tab->mode.base != MSXVDP_MODE_MULTICOLOR)
                num = text_add(dst, size, num, "Colours from $%04X: foreground %s (%d), background %s (%d)\n",
                               tile.colour_addr, colour_name(tile.forec), tile.forec, colour_name(tile.backc), tile.backc);
            else if (tab->mode.base == MSXVDP_MODE_TEXT1)
                num = text_add(dst, size, num, "Text colours from R#7: foreground %s (%d), background %s (%d)\n",
                               colour_name(tile.forec), tile.forec, colour_name(tile.backc), tile.backc);
            num = text_add(dst, size, num, "Colour %s (%d)\n", colour_name(tile.colour), tile.colour);
            add_break(outp, MSXVID_BK_VRAM, tile.name_addr, tile.name_addr);
            add_break(outp, MSXVID_BK_VRAM, tile.tile_lo, tile.tile_hi);
            if (tile.colour_addr != NO_ADDR) {
                const int whole = tab->mode.base == MSXVDP_MODE_GRAPHIC2 || tab->mode.base == MSXVDP_MODE_GRAPHIC3;
                add_break(outp, MSXVID_BK_VRAM, whole ? (tile.colour_addr & ~7u) : tile.colour_addr,
                          whole ? (tile.colour_addr & ~7u) + 7 : tile.colour_addr);
            }
            return num;
        }
        outp->kind = MSXVID_PK_BORDER;
        outp->colour = snap->regs[7] & 15;
        num = text_add(dst, size, num, "Inside the display but outside the text columns: the backdrop %s (%d)\n",
                       colour_name(outp->colour), outp->colour);
        add_break(outp, MSXVID_BK_REGISTER, 7, 7);
        return num;
    }
    {
        bitmap_rec bitmap;
        bitmap_at(tab, vdpx, vdpy, &bitmap);
        if (bitmap.valid) {
            outp->kind = MSXVID_PK_BITMAP;
            outp->colour = bitmap.colour;
            outp->vram_addr = bitmap.phys;
            outp->markx = vdpx; outp->marky = vdpy; outp->markw = 1; outp->markh = 1;
            num = text_add(dst, size, num, "Page byte at logical $%05X (physical $%05X) = $%02X\n", bitmap.logical, bitmap.phys, bitmap.byte);
            if (bitmap.colour >= 0)
                num = text_add(dst, size, num, "Colour %s (%d)\n", colour_name(bitmap.colour), bitmap.colour);
            else
                num = text_add(dst, size, num, "Graphic 7 colour: green %d, red %d, blue %d (of 7, 7, 3)\n",
                               bitmap.byte >> 5, (bitmap.byte >> 2) & 7, bitmap.byte & 3);
            add_break(outp, MSXVID_BK_VRAM, bitmap.phys, bitmap.phys);
            return num;
        }
    }
    num = text_add(dst, size, num, "This mode is not decoded.\n");
    return num;
}

static void locate(msxvid_tab *tab, int posx, int posy, msxvid_pixel *outp)
{
    const msxvdp_snapshot *snap = tab->snap;
    const int scale = tab->effzoom ? tab->effzoom : 1;
    int picx, picy, num;

    memset(outp, 0, sizeof *outp);
    outp->vdp_x = outp->vdp_y = outp->line = outp->colour = outp->cell = outp->name = outp->sprite = -1;
    outp->vram_addr = NO_ADDR;
    if (!tab->picw || posx < 0 || posy < 0 || posx >= tab->picw || posy >= tab->pich)
        return;
    picx = posx / scale; picy = posy / scale;
    outp->valid = 1;
    outp->picx = picx; outp->picy = picy;
    outp->markx = picx; outp->marky = picy; outp->markw = 1; outp->markh = 1;
    outp->rgb = tab->base ? tab->base[(size_t)picy * (size_t)tab->basew + (size_t)picx] : COL_BLACK;

    num = snprintf(outp->text, sizeof outp->text, "Picture pixel %d,%d\n", picx, picy);
    switch (tab->kind) {
    case BK_FRAME:
    case BK_SCREEN: {
        const mapping map = picture_mapping(tab);
        /* floor division: the border has negative VDP coordinates */
        const int vdpx = (picx - map.orgx) >= 0 ? (picx - map.orgx) / map.stepx : -(((map.orgx - picx) + map.stepx - 1) / map.stepx);
        const int vdpy = (picy - map.orgy) >= 0 ? (picy - map.orgy) / map.stepy : -(((map.orgy - picy) + map.stepy - 1) / map.stepy);
        int mark_x, mark_y, mark_w, mark_h;
        num = describe_vdp_pixel(tab, vdpx, vdpy, outp, num);
        /* the marks were set in VDP pixels; the picture wants its own */
        if (outp->kind == MSXVID_PK_BORDER || outp->vdp_x < 0 || outp->vdp_y < 0 || outp->vdp_x >= tab->geom.wide || outp->vdp_y >= tab->geom.lines) {
            mark_x = picx; mark_y = picy; mark_w = 1; mark_h = 1;
        } else {
            mark_x = map.orgx + outp->markx * map.stepx; mark_y = map.orgy + outp->marky * map.stepy;
            mark_w = outp->markw * map.stepx; mark_h = outp->markh * map.stepy;
        }
        outp->markx = mark_x; outp->marky = mark_y; outp->markw = mark_w; outp->markh = mark_h;
        break;
    }
    case BK_VALUES: {
        uint32_t base;
        int cols, rows;
        name_table(tab, &base, &cols, &rows);
        outp->kind = MSXVID_PK_VALUE;
        if (cols > 0) {
            const int col = picx / tab->tile_w, row = picy / 8;
            const uint32_t addr = base + (uint32_t)(row * cols + col);
            const uint32_t size = snap->vram_size ? snap->vram_size : 1;
            outp->cell = row * cols + col;
            outp->vram_addr = addr;
            outp->markx = col * tab->tile_w; outp->marky = row * 8; outp->markw = tab->tile_w; outp->markh = 8;
            outp->name = snap->vram[addr % size];
            num = text_add(outp->text, sizeof outp->text, num, "Name table entry %d (column %d, row %d) at $%04X = $%02X (%d)\n",
                           outp->cell, col, row, addr, outp->name, outp->name);
            add_break(outp, MSXVID_BK_VRAM, addr, addr);
        } else {
            int wide, high, pitch;
            uint32_t page;
            if (msxvdp_bitmap_geometry(snap, &wide, &high, &page, &pitch)) {
                const int span = tab->tile_w ? tab->tile_w : 1;
                const uint32_t logical = page + (uint32_t)((picy + snap->regs[23]) & 255) * (uint32_t)pitch + (uint32_t)(picx / span);
                const uint32_t phys = phys_addr(tab, logical);
                const uint32_t size = snap->vram_size ? snap->vram_size : 1;
                outp->vram_addr = phys;
                outp->markx = (picx / span) * span; outp->marky = picy; outp->markw = span; outp->markh = 1;
                num = text_add(outp->text, sizeof outp->text, num, "Page byte at logical $%05X (physical $%05X) = $%02X\n",
                               logical, phys, snap->vram[phys % size]);
                add_break(outp, MSXVID_BK_VRAM, phys, phys);
            }
        }
        break;
    }
    case BK_PATTERNS: {
        const int tile = (picy / 8) * 32 + picx / 8;
        const int graphic2 = tab->mode.base == MSXVDP_MODE_GRAPHIC2 || tab->mode.base == MSXVDP_MODE_GRAPHIC3;
        const uint32_t patgen = graphic2 ? (uint32_t)(snap->regs[4] & 0x04) << 11 : (uint32_t)(snap->regs[4] & 0x07) << 11;
        const uint32_t pg_mask = graphic2 ? ((uint32_t)(snap->regs[4] & 0x03) << 8) | 0xFF : 0xFF;
        const uint32_t entry = ((uint32_t)(graphic2 ? tab->bank : 0) * 256 + (uint32_t)tile) & pg_mask;
        const uint32_t addr = patgen + entry * 8;
        outp->kind = MSXVID_PK_PATTERN;
        outp->name = tile;
        outp->vram_addr = addr;
        outp->markx = (picx / 8) * 8; outp->marky = (picy / 8) * 8; outp->markw = 8; outp->markh = 8;
        num = text_add(outp->text, sizeof outp->text, num, "Tile %d ($%02X)%s: pattern bytes $%04X-$%04X\n", tile, tile,
                       graphic2 ? " (bank shown)" : "", addr, addr + 7);
        add_break(outp, MSXVID_BK_VRAM, addr, addr + 7);
        break;
    }
    case BK_SPRITES: {
        const int idx = (picy / SPRITE_PITCH) * 8 + picx / SPRITE_PITCH;
        sprite_rec spr;
        outp->kind = MSXVID_PK_SPRITE;
        outp->sprite = idx;
        outp->markx = (picx / SPRITE_PITCH) * SPRITE_PITCH; outp->marky = (picy / SPRITE_PITCH) * SPRITE_PITCH;
        outp->markw = SPRITE_TILE; outp->markh = SPRITE_TILE;
        if (idx >= 0 && idx < SPRITES) {
            const unsigned sat = (unsigned)(snap->regs[5] & 0x7F) << 7;
            sprite_geometry(snap, idx, &spr);
            num = text_add(outp->text, sizeof outp->text, num, "Sprite %d: y=%d x=%d pattern %d colour %s (%d)%s%s\n", idx,
                           spr.ytop - 1, spr.xleft + (spr.early ? 32 : 0), spr.pattern, colour_name(spr.colour), spr.colour,
                           spr.early ? ", early clock" : "", spr.live ? "" : ", past the end marker (not shown)");
            outp->vram_addr = sat + (unsigned)idx * 4;
            add_break(outp, MSXVID_BK_VRAM, sat + (unsigned)idx * 4, sat + (unsigned)idx * 4 + 3);
        }
        break;
    }
    case BK_PALETTE: {
        const int idx = (picy / MSXVDP_PAL_CELL) * MSXVDP_PAL_COLS + picx / MSXVDP_PAL_CELL;
        uint8_t col[16][3];
        msxvdp_colours(snap, col);
        outp->kind = MSXVID_PK_PALETTE;
        outp->colour = idx & 15;
        outp->markx = (picx / MSXVDP_PAL_CELL) * MSXVDP_PAL_CELL; outp->marky = (picy / MSXVDP_PAL_CELL) * MSXVDP_PAL_CELL;
        outp->markw = MSXVDP_PAL_CELL; outp->markh = MSXVDP_PAL_CELL;
        num = text_add(outp->text, sizeof outp->text, num, "Colour %s (%d): R %d, G %d, B %d%s\n", colour_name(idx), idx,
                       col[idx & 15][0], col[idx & 15][1], col[idx & 15][2],
                       snap->chip == MSXVDP_CHIP_V9918 ? " (fixed by the TMS9918A)" : "");
        if (snap->chip != MSXVDP_CHIP_V9918) add_break(outp, MSXVID_BK_PALETTE, (unsigned)idx, (unsigned)idx);
        break;
    }
    default:
        break;
    }
    if (outp->colour >= 0 && outp->kind != MSXVID_PK_VALUE) {
        uint8_t col[16][3];
        msxvdp_colours(snap, col);
        if (tab->kind != BK_FRAME && tab->kind != BK_SCREEN) outp->rgb = rgb_of(col[outp->colour & 15]);
    }
}

int msxvid_tab_hover(msxvid_tab *tab, int posx, int posy)
{
    msxvid_pixel before;
    if (!tab) return 0;
    before = tab->hover;
    tab->hovx = posx; tab->hovy = posy;
    locate(tab, posx, posy, &tab->hover);
    return tab->hover.valid != before.valid || tab->hover.picx != before.picx || tab->hover.picy != before.picy ||
           strcmp(tab->hover.text, before.text) != 0;
}

void msxvid_tab_pin(msxvid_tab *tab, int posx, int posy)
{
    msxvid_pixel probe;
    if (!tab) return;
    locate(tab, posx, posy, &probe);
    if (tab->pin.valid && probe.valid && tab->pin.markx == probe.markx && tab->pin.marky == probe.marky &&
        tab->pin.markw == probe.markw && tab->pin.markh == probe.markh) {
        msxvid_tab_unpin(tab);
        return;
    }
    tab->pinx = posx; tab->piny = posy;
    tab->pin = probe;
    if (probe.valid && probe.vram_addr != NO_ADDR) tab->vram_base = probe.vram_addr & ~15u;
}

void msxvid_tab_unpin(msxvid_tab *tab)
{
    if (!tab) return;
    tab->pinx = tab->piny = -1;
    memset(&tab->pin, 0, sizeof tab->pin);
}

const msxvid_pixel *msxvid_tab_pixel(const msxvid_tab *tab)
{
    if (!tab) return NULL;
    if (tab->pin.valid) return &tab->pin;
    if (tab->hover.valid) return &tab->hover;
    return NULL;
}

int msxvid_tab_marks(const msxvid_tab *tab, int hover[4], int pin[4])
{
    const int scale = tab && tab->effzoom ? tab->effzoom : 1;
    int ret = 0;
    if (!tab) return 0;
    if (tab->hover.valid && hover) {
        hover[0] = tab->hover.markx * scale; hover[1] = tab->hover.marky * scale;
        hover[2] = tab->hover.markw * scale; hover[3] = tab->hover.markh * scale;
        ret |= 1;
    }
    if (tab->pin.valid && pin) {
        pin[0] = tab->pin.markx * scale; pin[1] = tab->pin.marky * scale;
        pin[2] = tab->pin.markw * scale; pin[3] = tab->pin.markh * scale;
        ret |= 2;
    }
    return ret;
}

/* ---- Break on writes ------------------------------------------------------------------------- */

int msxvid_tab_can_break(const msxvid_tab *tab)
{
    const msxvid_pixel *pixp = msxvid_tab_pixel(tab);
    return tab && tab->dbg && pixp && pixp->nbreak > 0;
}

/* A Tcl procedure answering "which physical VRAM address would the CPU's next
 * data-port write touch": R#14's three high bits and the VRAM pointer, in the
 * interleaved order Graphic 6 and 7 store them. Defined once in openMSX's
 * interpreter (it outlives machine switches), so a breakpoint condition stays
 * short. */
static void define_vram_probe(void)
{
    msxhost_execute_sync(
        "proc ::fngo_vaddr {} { set a [expr {(([debug read \"VDP regs\" 14] & 7) << 14) "
        "| [debug read \"VRAM pointer\" 0] | ([debug read \"VRAM pointer\" 1] << 8)}]; "
        "if {([debug read \"VDP regs\" 0] & 10) == 10} { set a [expr {(($a << 16) | ($a >> 1)) & 0x1FFFF}] }; "
        "return $a }", NULL, 0);
}

static int break_exists(msxdebug *dbg, int port, const char *cond)
{
    msxdebug_breakpoint list[64];
    const int count = msxdebug_breakpoint_list(dbg, list, 64);
    for (int item = 0; item < count && item < 64; item++)
        if (list[item].type == MSXDEBUG_BP_OUT && list[item].start == port && list[item].end == port &&
            strcmp(list[item].condition, cond) == 0)
            return list[item].id;
    return -1;
}

int msxvid_tab_break_on_pixel(msxvid_tab *tab)
{
    const msxvid_pixel *pixp = msxvid_tab_pixel(tab);
    int first = -1;
    if (!tab || !tab->dbg || !pixp || pixp->nbreak == 0) return -1;
    define_vram_probe();
    for (int item = 0; item < pixp->nbreak; item++) {
        char cond[128], err[128];
        int port, bpid;
        const unsigned lowest = pixp->break_lo[item], highest = pixp->break_hi[item];
        switch (pixp->break_kind[item]) {
        case MSXVID_BK_PALETTE:
            port = 0x9A; cond[0] = '\0';
            break;
        case MSXVID_BK_REGISTER:
            port = 0x99;
            /* the engine tries a condition once outside a watchpoint, where $wp_last_value
             * does not exist yet: hence the info exists */
            snprintf(cond, sizeof cond, "[info exists ::wp_last_value]&&$::wp_last_value==%u", 0x80u + lowest);
            break;
        default:
            port = 0x98;
            if (lowest == highest) snprintf(cond, sizeof cond, "[::fngo_vaddr]==%u", lowest);
            else snprintf(cond, sizeof cond, "[::fngo_vaddr]>=%u&&[::fngo_vaddr]<=%u", lowest, highest);
            break;
        }
        bpid = break_exists(tab->dbg, port, cond);
        if (bpid < 0) bpid = msxdebug_breakpoint_add(tab->dbg, MSXDEBUG_BP_OUT, (uint16_t)port, (uint16_t)port, cond, err, (int)sizeof err);
        if (first < 0) first = bpid;
    }
    return first;
}

/* ---- the tables ---------------------------------------------------------------------------- */

static const char *mode_title(const msxvid_tab *tab)
{
    return tab->mode.mode_name ? tab->mode.mode_name : "?";
}

static void fill_status(msxvid_tab *tab)
{
    const msxvid_capture *cap = tab->cap;
    const msxvdp_snapshot *snap = tab->snap;
    int num = 0;
    static const char *const chips[] = { "V9918 (MSX1)", "V9938 (MSX2)", "V9958 (MSX2+)" };

    num = snprintf(tab->status, sizeof tab->status, "%s, %s, %d lines, %s %d Hz, display %s",
                   mode_title(tab), chips[cap->chip >= 0 && cap->chip < 3 ? cap->chip : 0], tab->geom.lines,
                   tab->geom.pal ? "PAL" : "NTSC", tab->geom.pal ? 50 : 60, (snap->regs[1] & 0x40) ? "on" : "BLANKED");
    if (tab->mode.sprite_mode)
        num += snprintf(tab->status + num, sizeof tab->status - (size_t)num, ", sprites %s%s",
                        (snap->regs[1] & 2) ? "16x16" : "8x8", (snap->regs[1] & 1) ? " magnified" : "");
    num += snprintf(tab->status + num, sizeof tab->status - (size_t)num, "\n");
    if (tab->has_live)
        num += snprintf(tab->status + num, sizeof tab->status - (size_t)num, "Stopped%s; frame %llu published%s",
                        cap->beam_line >= 0 ? "" : " (beam position unknown)", (unsigned long long)cap->frame,
                        cap->beam_line >= 0 ? "" : "");
    else
        num += snprintf(tab->status + num, sizeof tab->status - (size_t)num, "Running; frame %llu, %u register/palette writes logged%s",
                        (unsigned long long)cap->frame, (unsigned)cap->log.event_count,
                        cap->log.dropped ? " (some dropped)" : "");
    if (tab->has_live && cap->beam_line >= 0)
        snprintf(tab->status + num, sizeof tab->status - (size_t)num, "; beam at line %d of %d, clock %d",
                 cap->beam_line, cap->log.valid ? cap->log.lines_per_frame : (tab->geom.pal ? 313 : 262),
                 cap->beam_cycle);
}

const char *msxvid_tab_status(const msxvid_tab *tab) { return tab ? tab->status : ""; }

static void reg_entry(msxvid_tab *tab, const char *name, int first, int count, const char *meaning)
{
    msxvid_reg *reg = &tab->regs[tab->nregs];
    char *buf = tab->reg_names[tab->nregs];
    int age = MSXVID_REG_FADE, writes = 0, line = -1, num = 0;

    snprintf(buf, sizeof tab->reg_names[0], "%s", name);
    reg->name = buf;
    for (int item = 0; item < count; item++) {
        const int index = first + item;
        num += snprintf(reg->value + num, sizeof reg->value - (size_t)num, "%02X", tab->cap->regs[index]);
        if (tab->reg_age[index] < age) age = tab->reg_age[index];
        writes += tab->reg_writes[index];
        if (tab->reg_first[index] >= 0 && (line < 0 || tab->reg_first[index] < line)) line = tab->reg_first[index];
    }
    reg->age = age;
    reg->writes = writes;
    reg->first_line = line;
    snprintf(reg->meaning, sizeof reg->meaning, "%s", meaning);
    tab->nregs++;
}

static void fill_regs(msxvid_tab *tab)
{
    char buf[160], name[24];
    static const struct { int first, count; const char *name; } k_cmd[] = {
        { 32, 2, "SX" }, { 34, 2, "SY" }, { 36, 2, "DX" }, { 38, 2, "DY" }, { 40, 2, "NX" }, { 42, 2, "NY" },
        { 44, 1, "CLR" }, { 45, 1, "ARG" }, { 46, 1, "CMD" } };

    tab->nregs = 0;
    for (int reg = 0; reg <= REG_LAST_CONTROL; reg++) {
        snprintf(name, sizeof name, "R#%d", reg);
        buf[0] = '\0';
        msxvdp_describe_register(tab->snap, reg, buf, (int)sizeof buf);
        {
            char text[160];
            snprintf(text, sizeof text, "%s: %s", msxvdp_register_name(reg), buf);
            reg_entry(tab, name, reg, 1, text);
        }
    }
    if (tab->cap->chip != MSXVDP_CHIP_V9918) {
        for (size_t item = 0; item < sizeof k_cmd / sizeof *k_cmd; item++) {
            char text[160];
            if (k_cmd[item].count == 2) snprintf(name, sizeof name, "R#%d-%d %s", k_cmd[item].first, k_cmd[item].first + 1, k_cmd[item].name);
            else snprintf(name, sizeof name, "R#%d %s", k_cmd[item].first, k_cmd[item].name);
            if (k_cmd[item].first == 46)
                snprintf(text, sizeof text, "command %X, logical operation %X (V9938 command engine)",
                         tab->cap->regs[46] >> 4, tab->cap->regs[46] & 15);
            else
                snprintf(text, sizeof text, "command engine %s", k_cmd[item].name);
            reg_entry(tab, name, k_cmd[item].first, k_cmd[item].count, text);
        }
    }
}

int msxvid_tab_regs(const msxvid_tab *tab, const msxvid_reg **out)
{
    if (!tab) return 0;
    if (out) *out = tab->regs;
    return tab->nregs;
}

static void fill_swatches(msxvid_tab *tab)
{
    const msxvid_capture *cap = tab->cap;
    uint8_t col[16][3];
    const msxvid_pixel *pixp = msxvid_tab_pixel(tab);
    long uses[16];
    int changed[16];

    msxvdp_colours(tab->snap, col);
    memset(uses, 0, sizeof uses);
    memset(changed, 0, sizeof changed);
    if (cap->fb_valid)
        for (size_t item = 0; item < (size_t)MSXVID_FB_W * MSXVID_FB_H; item++) {
            const uint32_t pix = cap->pixels[item] & 0xFFFFFFu;
            for (int idx = 1; idx < 16; idx++)
                if (pix == ((uint32_t)col[idx][0] << 16 | (uint32_t)col[idx][1] << 8 | col[idx][2])) { uses[idx]++; break; }
        }
    if (cap->log.valid)
        for (uint32_t item = 0; item < cap->log.event_count; item++)
            if (cap->log.events[item].kind == MSXVID_EV_PALETTE && cap->log.events[item].index < 16)
                changed[cap->log.events[item].index] = 1;
    for (int idx = 0; idx < 16; idx++) {
        msxvid_swatch *swat = &tab->swat[idx];
        snprintf(tab->swat_names[idx], sizeof tab->swat_names[0], "%s (%d)", colour_name(idx), idx);
        swat->index = idx;
        swat->name = tab->swat_names[idx];
        swat->rgb = rgb_of(col[idx]);
        swat->uses = uses[idx];
        swat->role[0] = '\0';
        swat->changed = changed[idx];
        swat->inspected = pixp && pixp->colour == idx;
    }
    {
        const int back = tab->snap->regs[7] & 15;
        if (tab->mode.base == MSXVDP_MODE_TEXT1 || tab->mode.base == MSXVDP_MODE_TEXT2) {
            snprintf(tab->swat[tab->snap->regs[7] >> 4].role, sizeof tab->swat[0].role, "text foreground (R#7)");
            if (back != tab->snap->regs[7] >> 4)
                snprintf(tab->swat[back].role, sizeof tab->swat[0].role, "backdrop, text background (R#7)");
        } else {
            snprintf(tab->swat[back].role, sizeof tab->swat[0].role, "backdrop (R#7)");
        }
        if (tab->cap->chip != MSXVDP_CHIP_V9918 && tab->swat[0].role[0] == '\0')
            snprintf(tab->swat[0].role, sizeof tab->swat[0].role, "transparent (shows the backdrop)");
    }
}

int msxvid_tab_swatches(const msxvid_tab *tab, const msxvid_swatch **out)
{
    if (!tab) return 0;
    if (out) *out = tab->swat;
    return 16;
}

static void fill_lines(msxvid_tab *tab)
{
    tab->nlines = 0;
    for (int bnd = 0; bnd < tab->nbands; bnd++) {
        const band_rec *band = &tab->bands[bnd];
        msxvid_band *line = &tab->lines[tab->nlines++];
        memset(line, 0, sizeof *line);
        line->first = band->first;
        line->last = band->last;
        line->abs_first = tab->geom.zero + band->first;
        line->writes = band->writes;
        line->scroll_y = band->regs[23];
        line->backdrop = band->regs[7] & 15;
        {
            msxvdp_mode_info mode;
            apply_band(tab, band);
            msxvdp_decode_mode(tab->snap, &mode);
            snprintf(line->mode, sizeof line->mode, "%s", mode.mode_name ? mode.mode_name : "?");
            line->name_table = (unsigned)(band->regs[2] & (mode.is_bitmap ? 0x7F : mode.base == MSXVDP_MODE_TEXT2 ? 0x7C : 0x0F)) << 10;
            line->pattern_table = (unsigned)(band->regs[4] & 0x3F) << 11;
            line->colour_table = ((unsigned)band->regs[10] & 7) << 14 | (unsigned)band->regs[3] << 6;
            line->sprite_attr = ((unsigned)band->regs[11] & 3) << 15 | (unsigned)(band->regs[5] & 0xFF) << 7;
            line->sprite_pattern = (unsigned)(band->regs[6] & 0x3F) << 11;
            restore_state(tab);
        }
        {
            char scroll[24] = "", mode_name[24];
            snprintf(mode_name, sizeof mode_name, "%s", line->mode);
            if (line->scroll_y) snprintf(scroll, sizeof scroll, ", scroll Y %d", line->scroll_y);
            snprintf(line->text, sizeof line->text, "lines %d-%d (frame %d): %s, names $%04X, patterns $%04X, colours $%04X, sprites $%04X/$%04X, backdrop %d%s",
                     band->first, band->last, line->abs_first, mode_name, line->name_table, line->pattern_table,
                     line->colour_table, line->sprite_attr, line->sprite_pattern, line->backdrop, scroll);
        }
    }
}

int msxvid_tab_lines(const msxvid_tab *tab, const msxvid_band **out)
{
    if (!tab) return 0;
    if (out) *out = tab->lines;
    return tab->nlines;
}

static void timing_add(msxvid_tab *tab, const char *label, const char *fmt, ...)
{
    char value[96];
    va_list args;
    if (tab->ntiming >= MAX_TIMING) return;
    va_start(args, fmt);
    vsnprintf(value, sizeof value, fmt, args);
    va_end(args);
    snprintf(tab->timing_buf[tab->ntiming], sizeof tab->timing_buf[0], "%s\t%s", label, value);
    tab->timing_ptr[tab->ntiming] = tab->timing_buf[tab->ntiming];
    tab->ntiming++;
}

static void fill_timing(msxvid_tab *tab)
{
    const msxvid_capture *cap = tab->cap;
    const geom_rec *geo = &tab->geom;
    const int total = cap->log.valid ? cap->log.lines_per_frame : (geo->pal ? 313 : 262);

    tab->ntiming = 0;
    timing_add(tab, "Standard", "%s, %d Hz, %d lines a frame", geo->pal ? "PAL" : "NTSC", geo->pal ? 50 : 60, total);
    timing_add(tab, "Line", "1368 VDP clocks (21.477 MHz) = 228 Z80 clocks at 3.58 MHz");
    timing_add(tab, "Frame", "%d VDP clocks = %d Z80 clocks", total * 1368, total * 228);
    timing_add(tab, "Display", "%d lines from frame line %d (R#9 %s, vertical adjust %d)", geo->lines, geo->zero,
               geo->lines == 212 ? "212" : "192", (cap->regs[18] >> 4) ^ 7);
    timing_add(tab, "Published frame", "starts at frame line %d (480 rows = 240 lines)", geo->top);
    timing_add(tab, "Vertical blank", "the status flag F is set at line %d; the interrupt follows if R#1 IE0 is on", geo->zero + geo->lines);
    timing_add(tab, "Line interrupt", "R#19 = %d: line %d of the display (R#0 IE1 %s)", cap->regs[19],
               ((cap->regs[19] - cap->regs[23]) & 255), (cap->regs[0] & 0x10) ? "on" : "off");
    timing_add(tab, "Logged frame", "frame %u, %u writes%s", cap->log.frame, (unsigned)cap->log.event_count,
               cap->log.dropped ? ", some dropped" : "");
    if (cap->log.valid && cap->chip != MSXVDP_CHIP_V9918)
        timing_add(tab, "Command engine", "last command %d, now at %d,%d", cap->log.last_command,
                   cap->log.progress_dx, cap->log.progress_dy);
    if (tab->has_live && cap->cur.valid) {
        if (cap->cur.event_count)
            timing_add(tab, "Frame in progress", "%u register/palette writes so far, the last at line %d",
                       (unsigned)cap->cur.event_count, cap->cur.events[cap->cur.event_count - 1].line);
        else
            timing_add(tab, "Frame in progress", "no register or palette writes yet");
    }
    if (tab->has_live) {
        if (cap->beam_line >= 0)
            timing_add(tab, "Beam", "line %d (display line %d), clock %d of 1368", cap->beam_line, cap->beam_line - geo->zero,
                       cap->beam_cycle);
        else
            timing_add(tab, "Beam", "unknown");
    } else {
        timing_add(tab, "Beam", "(stopped only)");
    }
}

int msxvid_tab_timing(const msxvid_tab *tab, const char *const **lines)
{
    if (!tab) return 0;
    if (lines) *lines = tab->timing_ptr;
    return tab->ntiming;
}

int msxvid_tab_beam_permille(const msxvid_tab *tab)
{
    int total, per;
    if (!tab || !tab->has_live || tab->cap->beam_line < 0) return 0;
    total = tab->cap->log.valid ? tab->cap->log.lines_per_frame : (tab->geom.pal ? 313 : 262);
    per = (int)((long)tab->cap->beam_line * 1000 / (total ? total : 1));
    return per > 1000 ? 1000 : per < 0 ? 0 : per;
}

/* ---- refresh --------------------------------------------------------------------------------- */

unsigned msxvid_tab_refresh(msxvid_tab *tab, int force)
{
    unsigned flags = 0;
    uint64_t sig;
    if (!tab) return 0;
    if (!acquire(tab, force)) {
        tab->picw = tab->pich = 0;
        why_set(tab, "No machine is running.");
        tab->status[0] = '\0';
        return MSXVID_TAB_OFF | MSXVID_TAB_PICTURE | MSXVID_TAB_TABLES;
    }
    sig = signature(tab);
    if (force || tab->dirty || sig != tab->sig) {
        if (sig != tab->sig || force) track(tab);
        tab->sig = sig;
        tab->dirty = 0;
        build_picture(tab);
        fill_status(tab);
        fill_regs(tab);
        fill_swatches(tab);
        fill_lines(tab);
        fill_timing(tab);
        if (tab->hovx >= 0) locate(tab, tab->hovx, tab->hovy, &tab->hover);
        if (tab->pinx >= 0) locate(tab, tab->pinx, tab->piny, &tab->pin);
        flags |= MSXVID_TAB_PICTURE | MSXVID_TAB_TABLES;
        if (!tab->picw) flags |= MSXVID_TAB_OFF;
    }
    return flags;
}

const uint32_t *msxvid_tab_picture(msxvid_tab *tab, int *wide, int *high)
{
    if (!tab || !tab->picw) return NULL;
    if (wide) *wide = tab->picw;
    if (high) *high = tab->pich;
    return tab->pic;
}

const char *msxvid_tab_why(const msxvid_tab *tab) { return tab ? tab->why : ""; }
int msxvid_tab_effective_zoom(const msxvid_tab *tab) { return tab ? tab->effzoom : 1; }

/* ---- text -------------------------------------------------------------------------------------- */

int msxvid_tab_text(msxvid_tab *tab, int section, char *dst, int dstsz)
{
    int num = 0;
    const size_t size = (size_t)dstsz;
    if (!tab || !dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    switch (section) {
    case MSXVID_S_REGISTERS:
        num = text_add(dst, size, num, "%s\n", tab->status);
        for (int item = 0; item < tab->nregs; item++) {
            const msxvid_reg *reg = &tab->regs[item];
            num = text_add(dst, size, num, "%-12s %-8s %s", reg->name, reg->value, reg->meaning);
            if (reg->writes) num = text_add(dst, size, num, "  [%d write%s, first at line %d]", reg->writes, reg->writes == 1 ? "" : "s", reg->first_line);
            else if (reg->age < MSXVID_REG_FADE) num = text_add(dst, size, num, "  [changed %d refresh%s ago]", reg->age, reg->age == 1 ? "" : "es");
            num = text_add(dst, size, num, "\n");
        }
        break;
    case MSXVID_S_COLOURS:
        for (int item = 0; item < 16; item++) {
            const msxvid_swatch *swat = &tab->swat[item];
            num = text_add(dst, size, num, "%2d %-20s %06X  %ld px%s%s%s\n", item, swat->name, swat->rgb & 0xFFFFFFu, swat->uses,
                           swat->role[0] ? "  " : "", swat->role, swat->changed ? "  [palette written this frame]" : "");
        }
        break;
    case MSXVID_S_LINES:
        for (int item = 0; item < tab->nlines; item++) num = text_add(dst, size, num, "%s\n", tab->lines[item].text);
        break;
    case MSXVID_S_TIMING:
        for (int item = 0; item < tab->ntiming; item++) {
            const char *label = tab->timing_ptr[item], *value = strchr(label, '\t');
            if (value) num = text_add(dst, size, num, "%.*s: %s\n", (int)(value - label), label, value + 1);
            else num = text_add(dst, size, num, "%s\n", label);
        }
        break;
    case MSXVID_S_VRAM: {
        const int rows = 16;
        char hex[2048];
        msxvdp_format_hex(tab->snap, tab->vram_base, rows, hex, (int)sizeof hex);
        num = text_add(dst, size, num, "%s", hex);
        break;
    }
    default: {
        const msxvid_pixel *pixp = msxvid_tab_pixel(tab);
        if (pixp) num = text_add(dst, size, num, "%s", pixp->text);
        else num = text_add(dst, size, num, "No pixel under the pointer.\n");
        break;
    }
    }
    return num;
}

int msxvid_tab_text_for(msxdebug *dbg, const char *section, char *dst, int dstsz)
{
    msxvid_tab *tab;
    int sec = -1, num = 0, waited;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (section && *section) {
        sec = msxvid_section_from_name(section);
        if (sec < 0) return snprintf(dst, (size_t)dstsz, "unknown section \"%s\" (pixel, registers, colours, lines, timing, vram)\n", section);
    }
    tab = msxvid_tab_new(dbg);
    if (!tab) return snprintf(dst, (size_t)dstsz, "out of memory\n");
    /* a running machine publishes a frame within a moment; a parked one is read at once */
    for (waited = 0; waited < 600; waited += 20) {
        if (msxvid_tab_refresh(tab, 1) & MSXVID_TAB_OFF) {
            msx_sleep_ms(20);
            continue;
        }
        break;
    }
    if (!tab->have) {
        num = snprintf(dst, (size_t)dstsz, "No machine is running, or no frame has been published yet.\n");
    } else if (sec < 0) {
        /* the status is the first line of the registers text */
        num = msxvid_tab_text(tab, MSXVID_S_REGISTERS, dst, dstsz);
    } else {
        num = msxvid_tab_text(tab, sec, dst, dstsz);
    }
    msxvid_tab_free(tab);
    return num;
}
