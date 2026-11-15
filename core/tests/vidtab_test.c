/*
 * vidtab_test -- the Video tab's controller (core/debugger/vidtab.c) on made-up
 * captures, no machine: the published frame as the Display view, the Decoded
 * views of the tile modes and of the bitmap modes (Graphic 4, 6 and 7, with
 * the palette and the interleaved banks), the per-line log splitting a frame
 * into bands, the inspector and its break targets, the registers with their
 * change age, the sections' text, the Beam view's stopped-only rule, and then
 * a fuzz of random captures and settings that must neither crash nor produce
 * an impossible picture. Nothing here sets an environment variable (MinGW).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msxvdp.h"
#include "msxvidtab.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "vidtab_test: " __VA_ARGS__); fputc('\n', stderr); failures = 1; } } while (0)

static uint32_t rng_state = 0x1234567u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static msxvid_capture *new_cap(void)
{
    msxvid_capture *cap = calloc(1, sizeof *cap);
    if (!cap) { fprintf(stderr, "vidtab_test: out of memory\n"); exit(1); }
    for (int idx = 0; idx < MSXVID_FB_W * MSXVID_FB_H; idx++)
        cap->pixels[idx] = (uint32_t)(idx * 2654435761u) & 0xFFFFFFu;
    cap->fb_valid = 1;
    cap->vram_size = 0x4000;
    cap->serial = 1;
    cap->frame = 1;
    cap->beam_line = cap->beam_cycle = -1;
    return cap;
}

/* An MSX1 Graphic 1 screen: names at $1800, colours at $2000, patterns at $0000,
 * sprite attributes at $1B00, sprite patterns at $3800, backdrop colour 4.
 * Tile 1 is solid white on black; tile 8 has both colours transparent. */
static msxvid_capture *g1_cap(void)
{
    msxvid_capture *cap = new_cap();
    cap->chip = 0;
    cap->regs[1] = 0x40;
    cap->regs[2] = 0x06;
    cap->regs[3] = 0x80;
    cap->regs[4] = 0x00;
    cap->regs[5] = 0x36;
    cap->regs[6] = 0x07;
    cap->regs[7] = 0x04;
    memset(&cap->vram[8], 0xFF, 8);                 /* tile 1 */
    cap->vram[0x2000 + 0] = 0xF1;                   /* tiles 0-7: white on black */
    cap->vram[0x1800 + 0] = 1;                      /* cell 0 */
    cap->vram[0x1800 + 1] = 8;                      /* cell 1: transparent */
    cap->vram[0x1800 + 8 * 32] = 8;                 /* row 8, column 0: transparent */
    cap->vram[0x2000 + 1] = 0x00;                   /* tiles 8-15: both colours transparent */
    memset(&cap->vram[0x3800], 0xFF, 8);            /* sprite pattern 0 */
    cap->vram[0x1B00 + 0] = 40;                     /* sprite 0 y (the line is y + 1) */
    cap->vram[0x1B00 + 1] = 100;                    /* x */
    cap->vram[0x1B00 + 2] = 0;                      /* pattern */
    cap->vram[0x1B00 + 3] = 8;                      /* medium red */
    cap->vram[0x1B00 + 4] = 0xD0;                   /* end of the list */
    cap->log.valid = 0;
    return cap;
}

static unsigned refresh_fed(msxvid_tab *tab, const msxvid_capture *cap, int stopped)
{
    msxvid_tab_feed(tab, cap, stopped);
    return msxvid_tab_refresh(tab, 1);
}

static uint32_t pixel_at(const uint32_t *pic, int wide, int posx, int posy)
{
    return pic[(size_t)posy * (size_t)wide + (size_t)posx] & 0xFFFFFFu;
}

static void test_names(void)
{
    int count = 0;
    const msxvid_overlay_info *ovl = msxvid_overlays(&count);
    CHECK(count == 6 && ovl[0].bit == MSXVID_OV_GRID, "overlay table");
    CHECK(!strcmp(msxvid_view_key(MSXVID_V_BEAM), "beam") && !strcmp(msxvid_view_name(MSXVID_V_VALUES), "Values"), "view names");
    CHECK(!strcmp(msxvid_source_key(MSXVID_SRC_SPRITES), "sprites"), "source keys");
    CHECK(msxvid_section_from_name("reg") == MSXVID_S_REGISTERS, "section prefix");
    CHECK(msxvid_section_from_name("regs") == MSXVID_S_REGISTERS, "regs alias");
    CHECK(msxvid_section_from_name("vr") == MSXVID_S_VRAM, "vram section");
    CHECK(msxvid_section_from_name("nope") == -1 && msxvid_section_from_name("") == -1, "unknown section");
    CHECK(msxvid_tab_env_section(NULL) == 0, "env section of NULL");
}

static void test_display_and_g1(msxvid_tab *tab)
{
    msxvid_capture *cap = g1_cap();
    int wide = 0, high = 0;
    const uint32_t *pic;
    unsigned flags;

    msxvid_tab_set_viewport(tab, 640, 480);
    msxvid_tab_set_zoom(tab, 1);
    msxvid_tab_set_view(tab, MSXVID_V_DISPLAY);
    msxvid_tab_set_overlays(tab, 0);
    flags = refresh_fed(tab, cap, 0);
    CHECK((flags & MSXVID_TAB_PICTURE) && !(flags & MSXVID_TAB_OFF), "display refresh flags %u", flags);
    pic = msxvid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == 640 && high == 480, "display picture size %dx%d", wide, high);
    if (pic) {
        int same = 1;
        for (int idx = 0; idx < 640 * 480 && same; idx++) same = (pic[idx] & 0xFFFFFFu) == cap->pixels[idx] && (pic[idx] >> 24) == 0xFF;
        CHECK(same, "the Display view is not the published frame");
    }
    CHECK(strstr(msxvid_tab_status(tab), "Graphic 1"), "status: %s", msxvid_tab_status(tab));
    CHECK(strstr(msxvid_tab_status(tab), "V9918"), "status chip: %s", msxvid_tab_status(tab));

    /* Decoded Screen: 256 x 192, tile 1 white in the first cell, the rest of the black-on-white... */
    msxvid_tab_set_view(tab, MSXVID_V_DECODED);
    msxvid_tab_set_source(tab, MSXVID_SRC_SCREEN);
    msxvid_tab_set_overlays(tab, 0);
    refresh_fed(tab, cap, 0);
    pic = msxvid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == 256 && high == 192, "decoded screen size %dx%d", wide, high);
    if (pic) {
        CHECK(pixel_at(pic, wide, 3, 3) == 0xFFFFFF, "tile 1 is not white: %06X", pixel_at(pic, wide, 3, 3));
        /* tile 8: colours 0 and 0, both transparent: the backdrop (colour 4, dark blue) shows */
        CHECK(pixel_at(pic, wide, 12, 3) == 0x5455ED, "transparent tile is not the backdrop: %06X", pixel_at(pic, wide, 12, 3));
    }

    /* Fit picks the largest zoom the viewport takes: 2x of 256 x 192 in 640 x 480 */
    msxvid_tab_set_zoom(tab, 0);
    refresh_fed(tab, cap, 0);
    pic = msxvid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == 512 && high == 384 && msxvid_tab_effective_zoom(tab) == 2, "fit zoom %dx%d (%d)", wide, high, msxvid_tab_effective_zoom(tab));
    msxvid_tab_set_zoom(tab, 1);
    refresh_fed(tab, cap, 0);

    /* the inspector on the decoded tile */
    msxvid_tab_hover(tab, 3, 3);
    {
        const msxvid_pixel *pixp = msxvid_tab_pixel(tab);
        CHECK(pixp && pixp->valid && pixp->kind == MSXVID_PK_TILE, "hover kind");
        if (pixp) {
            CHECK(pixp->cell == 0 && pixp->name == 1, "cell %d name %d", pixp->cell, pixp->name);
            CHECK(pixp->nbreak == 3 && pixp->break_kind[0] == MSXVID_BK_VRAM && pixp->break_lo[0] == 0x1800 && pixp->break_hi[0] == 0x1800,
                  "name table break target: n=%d lo=%X", pixp->nbreak, pixp->break_lo[0]);
            CHECK(pixp->break_lo[1] == 8 && pixp->break_hi[1] == 15, "pattern break target %X-%X", pixp->break_lo[1], pixp->break_hi[1]);
            CHECK(pixp->break_lo[2] == 0x2000 && pixp->break_hi[2] == 0x2000, "colour break target %X", pixp->break_lo[2]);
            CHECK(strstr(pixp->text, "tile 1"), "inspector text: %s", pixp->text);
            CHECK(pixp->markw == 8 && pixp->markh == 8 && pixp->markx == 0, "mark %d,%d %dx%d", pixp->markx, pixp->marky, pixp->markw, pixp->markh);
        }
    }
    CHECK(!msxvid_tab_can_break(tab), "can break without a debugger");
    CHECK(msxvid_tab_break_on_pixel(tab) == -1, "break without a debugger");

    /* a sprite hit: sprite 0 at x=100, y=41 */
    msxvid_tab_hover(tab, 102, 43);
    {
        const msxvid_pixel *pixp = msxvid_tab_pixel(tab);
        CHECK(pixp && pixp->kind == MSXVID_PK_SPRITE && pixp->sprite == 0, "sprite hover");
        if (pixp) {
            CHECK(pixp->colour == 8, "sprite colour %d", pixp->colour);
            CHECK(pixp->nbreak == 2 && pixp->break_lo[0] == 0x1B00 && pixp->break_hi[0] == 0x1B03, "sprite attribute break target");
        }
    }
    /* pin and unpin by clicking the same thing twice */
    msxvid_tab_pin(tab, 3, 3);
    CHECK(msxvid_tab_pixel(tab) && msxvid_tab_pixel(tab)->cell == 0, "pin");
    {
        int hover[4], pin[4];
        msxvid_tab_hover(tab, 3, 3);
        msxvid_tab_unpin(tab);
        msxvid_tab_pin(tab, 3, 3);
        CHECK((msxvid_tab_marks(tab, hover, pin) & 2) && pin[2] == 8, "pin mark");
        msxvid_tab_unpin(tab);
        CHECK(!(msxvid_tab_marks(tab, hover, pin) & 2), "unpin");
    }
    msxvid_tab_hover(tab, -1, -1);

    /* Values: cell 0 and cell 1 hold different bytes, so different false colours; cells 2.. hold 0 */
    msxvid_tab_set_view(tab, MSXVID_V_VALUES);
    refresh_fed(tab, cap, 0);
    pic = msxvid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == 256 && high == 192, "values size %dx%d", wide, high);
    if (pic) {
        CHECK(pixel_at(pic, wide, 3, 3) != pixel_at(pic, wide, 11, 3), "values: cells 0 and 1 look alike");
        CHECK(pixel_at(pic, wide, 19, 3) == pixel_at(pic, wide, 27, 3), "values: two zero cells differ");
    }

    /* Patterns and Sprites and Palette sources */
    msxvid_tab_set_view(tab, MSXVID_V_DECODED);
    msxvid_tab_set_source(tab, MSXVID_SRC_PATTERNS);
    refresh_fed(tab, cap, 0);
    pic = msxvid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == 256 && high == 64, "patterns size %dx%d", wide, high);
    msxvid_tab_set_source(tab, MSXVID_SRC_SPRITES);
    refresh_fed(tab, cap, 0);
    pic = msxvid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == 128 && high == 64, "sprites size %dx%d", wide, high);
    if (pic) CHECK(pixel_at(pic, wide, 2, 2) == 0xFC5554, "sprite 0 is medium red: %06X", pixel_at(pic, wide, 2, 2));
    msxvid_tab_set_source(tab, MSXVID_SRC_PALETTE);
    refresh_fed(tab, cap, 0);
    pic = msxvid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == MSXVDP_PAL_W && high == MSXVDP_PAL_H, "palette size %dx%d", wide, high);

    free(cap);
}

static void test_registers(msxvid_tab *tab)
{
    msxvid_capture *cap = g1_cap();
    const msxvid_reg *regs;
    int count;

    msxvid_tab_set_view(tab, MSXVID_V_DISPLAY);
    refresh_fed(tab, cap, 0);
    count = msxvid_tab_regs(tab, &regs);
    CHECK(count == 28, "an MSX1 VDP has R#0-27 only: %d", count);
    if (count == 28) {
        CHECK(!strcmp(regs[7].name, "R#7") && !strcmp(regs[7].value, "04"), "R#7 reads %s", regs[7].value);
        CHECK(regs[7].age >= MSXVID_REG_FADE, "a register that never changed has age %d", regs[7].age);
    }
    /* a new capture with R#7 changed, with its write in the log */
    cap->regs[7] = 0x0A;
    cap->serial++;
    cap->frame++;
    cap->log.valid = 1;
    cap->log.lines_per_frame = 262;
    memcpy(cap->log.regs, cap->regs, 32);
    cap->log.regs[7] = 0x04;
    cap->log.event_count = 1;
    cap->log.events[0].line = 100;
    cap->log.events[0].kind = MSXVID_EV_REG;
    cap->log.events[0].index = 7;
    cap->log.events[0].value = 0x0A;
    msxvid_tab_feed(tab, cap, 0);
    msxvid_tab_refresh(tab, 1);
    count = msxvid_tab_regs(tab, &regs);
    CHECK(count == 28 && regs[7].age == 0, "R#7 should read as just changed");
    CHECK(count == 28 && regs[7].writes == 1 && regs[7].first_line == 100, "R#7 writes %d first %d", regs[7].writes, regs[7].first_line);
    {
        char buf[8192];
        int len = msxvid_tab_text(tab, MSXVID_S_REGISTERS, buf, sizeof buf);
        CHECK(len > 100 && strstr(buf, "R#7") && strstr(buf, "first at line 100"), "registers text");
    }

    /* the bands: the backdrop changed at absolute line 100; the first display line is 42 */
    {
        const msxvid_band *lines;
        const int bands = msxvid_tab_lines(tab, &lines);
        CHECK(bands == 2, "two bands expected, got %d", bands);
        if (bands == 2) {
            CHECK(lines[0].first == 0 && lines[0].last == 57 && lines[1].first == 58 && lines[1].last == 191,
                  "band edges %d-%d / %d-%d", lines[0].first, lines[0].last, lines[1].first, lines[1].last);
            CHECK(lines[0].backdrop == 4 && lines[1].backdrop == 10, "backdrops %d %d", lines[0].backdrop, lines[1].backdrop);
            CHECK(lines[1].abs_first == 100 && lines[1].writes == 1, "band 2 starts at frame line %d with %d writes", lines[1].abs_first, lines[1].writes);
        }
    }
    /* the Decoded screen paints the transparent tile with each band's backdrop */
    {
        int wide, high;
        const uint32_t *pic;
        msxvid_tab_set_view(tab, MSXVID_V_DECODED);
        msxvid_tab_set_source(tab, MSXVID_SRC_SCREEN);
        msxvid_tab_set_overlays(tab, 0);
        msxvid_tab_refresh(tab, 1);
        pic = msxvid_tab_picture(tab, &wide, &high);
        CHECK(pic != NULL, "split screen picture");
        if (pic) {
            CHECK(pixel_at(pic, wide, 12, 3) == 0x5455ED, "top band backdrop (dark blue): %06X", pixel_at(pic, wide, 12, 3));
            CHECK(pixel_at(pic, wide, 3, 8 * 8 + 3) == 0xD4C154, "lower band backdrop (dark yellow): %06X", pixel_at(pic, wide, 3, 8 * 8 + 3));
        }
        msxvid_tab_set_overlays(tab, MSXVID_OV_SPLITS | MSXVID_OV_GRID | MSXVID_OV_SPRITES | MSXVID_OV_HEAT);
        msxvid_tab_refresh(tab, 1);
        CHECK(msxvid_tab_picture(tab, &wide, &high) != NULL, "overlays picture");
    }
    free(cap);
}

/* Graphic 4: 256 x 212, 4 bits a pixel, R#2 picks the page, the palette is RAM */
static msxvid_capture *g4_cap(void)
{
    msxvid_capture *cap = new_cap();
    cap->chip = 1;
    cap->has_palette = 1;
    cap->vram_size = 0x20000;
    cap->regs[0] = 0x06;
    cap->regs[1] = 0x60;
    cap->regs[2] = 0x1F;
    cap->regs[9] = 0x80;
    cap->palette[1 * 2] = 0x70;            /* colour 1: red 7 */
    cap->palette[2 * 2 + 1] = 0x07;        /* colour 2: green 7 */
    cap->palette[3 * 2] = 0x07;            /* colour 3: blue 7 */
    cap->vram[0] = 0x12;                   /* pixels 0, 1 */
    cap->vram[1] = 0x30;                   /* pixels 2, 3 */
    cap->vram[128 * 5] = 0x21;             /* line 5 */
    return cap;
}

static void test_bitmap(msxvid_tab *tab)
{
    msxvid_capture *cap = g4_cap();
    int wide, high;
    const uint32_t *pic;

    msxvid_tab_set_view(tab, MSXVID_V_DECODED);
    msxvid_tab_set_source(tab, MSXVID_SRC_SCREEN);
    msxvid_tab_set_overlays(tab, 0);
    refresh_fed(tab, cap, 0);
    pic = msxvid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == 256 && high == 212, "graphic 4 size %dx%d", wide, high);
    CHECK(strstr(msxvid_tab_status(tab), "Graphic 4") && strstr(msxvid_tab_status(tab), "V9938"), "status: %s", msxvid_tab_status(tab));
    if (pic) {
        CHECK(pixel_at(pic, wide, 0, 0) == 0xFF0000, "pixel 0 should be red: %06X", pixel_at(pic, wide, 0, 0));
        CHECK(pixel_at(pic, wide, 1, 0) == 0x00FF00, "pixel 1 should be green: %06X", pixel_at(pic, wide, 1, 0));
        CHECK(pixel_at(pic, wide, 2, 0) == 0x0000FF, "pixel 2 should be blue: %06X", pixel_at(pic, wide, 2, 0));
        CHECK(pixel_at(pic, wide, 0, 5) == 0x00FF00 && pixel_at(pic, wide, 1, 5) == 0xFF0000, "line 5");
    }
    msxvid_tab_hover(tab, 1, 0);
    {
        const msxvid_pixel *pixp = msxvid_tab_pixel(tab);
        CHECK(pixp && pixp->kind == MSXVID_PK_BITMAP && pixp->colour == 2, "bitmap hover colour %d", pixp ? pixp->colour : -9);
        CHECK(pixp && pixp->nbreak == 1 && pixp->break_lo[0] == 0 && pixp->vram_addr == 0, "bitmap break target");
    }
    /* vertical scroll R#23 moves the page up */
    cap->regs[23] = 5;
    cap->serial++;
    refresh_fed(tab, cap, 0);
    pic = msxvid_tab_picture(tab, &wide, &high);
    if (pic) CHECK(pixel_at(pic, wide, 0, 0) == 0x00FF00, "scrolled page: %06X", pixel_at(pic, wide, 0, 0));
    cap->regs[23] = 0;

    /* the Patterns source says why there are none */
    msxvid_tab_set_source(tab, MSXVID_SRC_PATTERNS);
    refresh_fed(tab, cap, 0);
    CHECK(msxvid_tab_picture(tab, &wide, &high) == NULL && strstr(msxvid_tab_why(tab), "bitmap"), "patterns in a bitmap mode: %s", msxvid_tab_why(tab));
    msxvid_tab_set_source(tab, MSXVID_SRC_SCREEN);

    /* Values: the page's bytes, two pixels wide */
    msxvid_tab_set_view(tab, MSXVID_V_VALUES);
    refresh_fed(tab, cap, 0);
    pic = msxvid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == 256 && high == 212, "graphic 4 values size %dx%d", wide, high);
    if (pic) CHECK(pixel_at(pic, wide, 0, 0) == pixel_at(pic, wide, 1, 0) && pixel_at(pic, wide, 1, 0) != pixel_at(pic, wide, 2, 0), "byte blocks");
    msxvid_tab_set_view(tab, MSXVID_V_DECODED);

    /* Graphic 7: one byte a pixel, GGGRRRBB */
    cap->regs[0] = 0x0E;
    cap->regs[2] = 0x1F;
    memset(cap->vram, 0, 0x20000);
    cap->vram[0] = 0xE0;                   /* even logical bytes live in the first 64K */
    cap->vram[0x10000] = 0x03;             /* logical byte 1 */
    cap->serial++;
    refresh_fed(tab, cap, 0);
    pic = msxvid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == 256 && high == 212, "graphic 7 size %dx%d", wide, high);
    if (pic) {
        CHECK(pixel_at(pic, wide, 0, 0) == 0x00FF00, "graphic 7 green: %06X", pixel_at(pic, wide, 0, 0));
        CHECK(pixel_at(pic, wide, 1, 0) == 0x0000FF, "graphic 7 blue (bank 2): %06X", pixel_at(pic, wide, 1, 0));
    }
    msxvid_tab_hover(tab, 1, 0);
    {
        const msxvid_pixel *pixp = msxvid_tab_pixel(tab);
        CHECK(pixp && pixp->vram_addr == 0x10000 && pixp->break_lo[0] == 0x10000, "graphic 7 physical address %X", pixp ? pixp->vram_addr : 0);
    }

    /* Graphic 6: 512 wide, 4 bits, interleaved */
    cap->regs[0] = 0x0A;
    memset(cap->vram, 0, 0x20000);
    cap->vram[0] = 0x12;
    cap->vram[0x10000] = 0x34;             /* logical byte 1: pixels 2 and 3 */
    cap->serial++;
    refresh_fed(tab, cap, 0);
    pic = msxvid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == 512 && high == 212, "graphic 6 size %dx%d", wide, high);
    if (pic) {
        CHECK(pixel_at(pic, wide, 3, 0) == 0, "graphic 6 colour 4 is black in this palette: %06X", pixel_at(pic, wide, 3, 0));
        CHECK(pixel_at(pic, wide, 0, 0) == 0xFF0000 && pixel_at(pic, wide, 1, 0) == 0x00FF00 && pixel_at(pic, wide, 2, 0) == 0x0000FF, "graphic 6 pixels");
    }

    /* Graphic 5: 512 wide, 2 bits */
    cap->regs[0] = 0x08;                   /* M5 alone is Graphic 5 */
    memset(cap->vram, 0, 0x20000);
    cap->vram[0] = 0x1B;                   /* pixels 0..3: colours 0, 1, 2, 3 */
    cap->serial++;
    refresh_fed(tab, cap, 0);
    pic = msxvid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == 512 && high == 212, "graphic 5 size %dx%d", wide, high);
    if (pic) CHECK(pixel_at(pic, wide, 1, 0) == 0xFF0000 && pixel_at(pic, wide, 3, 0) == 0x0000FF, "graphic 5 pixels %06X %06X",
                   pixel_at(pic, wide, 1, 0), pixel_at(pic, wide, 3, 0));

    /* Text 2 is not decoded, and says so; the Display view still works */
    cap->regs[0] = 0x04;
    cap->regs[1] = 0x50;
    cap->serial++;
    refresh_fed(tab, cap, 0);
    CHECK(msxvid_tab_picture(tab, &wide, &high) == NULL && strstr(msxvid_tab_why(tab), "Text 2"), "text 2: %s", msxvid_tab_why(tab));
    msxvid_tab_set_view(tab, MSXVID_V_DISPLAY);
    refresh_fed(tab, cap, 0);
    CHECK(msxvid_tab_picture(tab, &wide, &high) != NULL, "display in text 2");
    free(cap);
}

static void test_beam_and_sections(msxvid_tab *tab)
{
    msxvid_capture *cap = g1_cap();
    int wide, high;
    const uint32_t *pic;
    char buf[8192];

    msxvid_tab_set_zoom(tab, 1);
    msxvid_tab_set_view(tab, MSXVID_V_BEAM);
    refresh_fed(tab, cap, 0);
    CHECK(msxvid_tab_picture(tab, &wide, &high) == NULL && strstr(msxvid_tab_why(tab), "stopped"), "beam while running: %s", msxvid_tab_why(tab));

    cap->live = 1;
    cap->beam_line = 100;
    cap->beam_cycle = 300;
    refresh_fed(tab, cap, 1);
    pic = msxvid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == 640 && high == 480, "beam picture %dx%d", wide, high);
    if (pic) {
        /* frame row 0 is absolute line 18 (NTSC); the beam is on line 100, so rows up to (100 - 18 + 1) * 2 are bright */
        const int row_in = (100 - 18) * 2, row_out = (100 - 18 + 5) * 2;
        CHECK(pixel_at(pic, wide, 7, row_in) == (cap->pixels[(size_t)row_in * 640 + 7] & 0xFFFFFF), "beam: a line above it is dimmed");
        CHECK(pixel_at(pic, wide, 7, row_out) != (cap->pixels[(size_t)row_out * 640 + 7] & 0xFFFFFF), "beam: a line below it is not dimmed");
    }
    msxvid_tab_set_overlays(tab, MSXVID_OV_ALL);
    refresh_fed(tab, cap, 1);
    CHECK(msxvid_tab_picture(tab, &wide, &high) != NULL, "all overlays");
    CHECK(msxvid_tab_beam_permille(tab) > 300 && msxvid_tab_beam_permille(tab) < 400, "beam permille %d", msxvid_tab_beam_permille(tab));
    CHECK(strstr(msxvid_tab_status(tab), "beam at line 100"), "status beam: %s", msxvid_tab_status(tab));

    for (int sec = 0; sec < MSXVID_S_COUNT; sec++) {
        const int len = msxvid_tab_text(tab, sec, buf, (int)sizeof buf);
        CHECK(len > 0 && (int)strlen(buf) == len, "section %s text length %d", msxvid_section_name(sec), len);
    }
    CHECK(msxvid_tab_text(tab, MSXVID_S_TIMING, buf, (int)sizeof buf) > 0 && strstr(buf, "NTSC") && strstr(buf, "Beam: line 100"), "timing text: %s", buf);
    msxvid_tab_set_vram_base(tab, 0x1800);
    msxvid_tab_text(tab, MSXVID_S_VRAM, buf, (int)sizeof buf);
    CHECK(strncmp(buf, "01800  01 08", 12) == 0, "vram hex dump: %.40s", buf);
    {
        const char *const *timing;
        const int count = msxvid_tab_timing(tab, &timing);
        CHECK(count >= 6 && strchr(timing[0], '\t'), "timing lines");
    }
    {
        const msxvid_swatch *swat;
        CHECK(msxvid_tab_swatches(tab, &swat) == 16 && swat[4].role[0] != '\0', "swatches: backdrop role");
    }
    CHECK(msxvid_tab_text_for(NULL, "timing", buf, (int)sizeof buf) > 0, "text_for without a machine");
    CHECK(strstr(buf, "No machine") != NULL, "text_for without a machine says so: %s", buf);
    CHECK(msxvid_tab_text_for(NULL, "bogus", buf, (int)sizeof buf) > 0 && strstr(buf, "unknown section"), "text_for unknown section");
    free(cap);
}

/* random captures and settings must never crash or build an impossible picture */
static void fuzz(msxvid_tab *tab)
{
    msxvid_capture *cap = new_cap();
    const int zooms[] = { 0, 1, 2, 3, 4, 6, 8, 12, 16 };

    cap->vram_size = 0x20000;
    for (int round = 0; round < 1500; round++) {
        int wide, high;
        const uint32_t *pic;
        for (int idx = 0; idx < 64; idx++) cap->regs[idx] = (uint8_t)rnd();
        if (rnd() % 3) {
            /* a plausible mode */
            static const uint8_t modes[] = { 0x00, 0x00, 0x02, 0x04, 0x06, 0x08, 0x0A, 0x0E, 0x02 };
            cap->regs[0] = modes[rnd() % sizeof modes];
            cap->regs[1] = (uint8_t)(0x40 | ((rnd() % 3) << 3) | (rnd() & 3));
        }
        for (int idx = 0; idx < 32; idx++) cap->palette[idx] = (uint8_t)rnd();
        for (int idx = 0; idx < 4096; idx++) cap->vram[rnd() % 0x20000] = (uint8_t)rnd();
        cap->chip = (int)(rnd() % 3);
        cap->has_palette = cap->chip != 0;
        cap->vram_size = (rnd() & 1) ? 0x4000 : (rnd() & 1) ? 0x20000 : 0x30000;
        cap->serial++;
        cap->frame++;
        cap->fb_valid = (int)(rnd() % 5 != 0);
        cap->log.valid = (int)(rnd() % 4 != 0);
        cap->log.pal = (int)(rnd() & 1);
        cap->log.lines_per_frame = cap->log.pal ? 313 : 262;
        memcpy(cap->log.regs, cap->regs, 32);
        for (int idx = 0; idx < 16; idx++) cap->log.palette[idx] = (uint16_t)(rnd() & 0x777);
        cap->log.event_count = rnd() % 300;
        for (uint32_t idx = 0; idx < cap->log.event_count; idx++) {
            msxvid_event *evt = &cap->log.events[idx];
            evt->line = (uint16_t)(idx * 313 / (cap->log.event_count ? cap->log.event_count : 1));
            evt->column = (uint16_t)(rnd() % 1368);
            evt->kind = (uint8_t)(rnd() % 3);
            evt->index = (uint8_t)(rnd() % 48);
            evt->value = (uint16_t)rnd();
        }
        cap->beam_line = (rnd() & 1) ? (int)(rnd() % 400) - 5 : -1;
        cap->beam_cycle = (int)(rnd() % 1400);
        cap->live = cap->beam_line >= 0;
        msxvid_tab_set_view(tab, (int)(rnd() % MSXVID_V_COUNT));
        msxvid_tab_set_source(tab, (int)(rnd() % MSXVID_SRC_COUNT));
        msxvid_tab_set_zoom(tab, zooms[rnd() % (sizeof zooms / sizeof *zooms)]);
        msxvid_tab_set_overlays(tab, rnd() & MSXVID_OV_ALL);
        msxvid_tab_set_bank(tab, (int)(rnd() % 3));
        msxvid_tab_set_viewport(tab, (int)(rnd() % 1500), (int)(rnd() % 1200));
        refresh_fed(tab, cap, cap->live);
        pic = msxvid_tab_picture(tab, &wide, &high);
        if (pic) {
            CHECK(wide > 0 && high > 0 && (long)wide * high <= 6000000L, "picture %dx%d out of bounds", wide, high);
            for (int tries = 0; tries < 6; tries++) {
                msxvid_tab_hover(tab, (int)(rnd() % (unsigned)(wide + 4)) - 2, (int)(rnd() % (unsigned)(high + 4)) - 2);
                if (msxvid_tab_pixel(tab)) {
                    const msxvid_pixel *pixp = msxvid_tab_pixel(tab);
                    CHECK(pixp->nbreak >= 0 && pixp->nbreak <= 3 && pixp->text[0], "inspector state");
                }
            }
            msxvid_tab_pin(tab, (int)(rnd() % (unsigned)wide), (int)(rnd() % (unsigned)high));
        } else {
            CHECK(msxvid_tab_why(tab)[0] != '\0', "no picture and no reason (round %d)", round);
        }
        {
            char buf[8192];
            int hover[4], pin[4];
            msxvid_tab_marks(tab, hover, pin);
            for (int sec = 0; sec < MSXVID_S_COUNT; sec++) msxvid_tab_text(tab, sec, buf, (int)sizeof buf);
            msxvid_tab_text(tab, MSXVID_S_REGISTERS, buf, 40);          /* a short buffer must truncate cleanly */
        }
        if (round % 50 == 0) msxvid_tab_unpin(tab);
    }
    free(cap);
}

/* Run by ctest with the MSX_DEBUGGER_VIDEO_* variables set (MinGW has no setenv, so the
 * environment comes from the test's properties): the tab reads them at creation. */
static int env_main(void)
{
    msxvid_tab *tab = msxvid_tab_new(NULL);
    if (!tab) return 1;
    CHECK(msxvid_tab_view(tab) == MSXVID_V_BEAM, "MSX_DEBUGGER_VIDEO_VIEW: view %d", msxvid_tab_view(tab));
    CHECK(msxvid_tab_source(tab) == MSXVID_SRC_PALETTE, "MSX_DEBUGGER_VIDEO_SOURCE: source %d", msxvid_tab_source(tab));
    CHECK(msxvid_tab_overlays(tab) == (MSXVID_OV_GRID | MSXVID_OV_SPLITS), "MSX_DEBUGGER_VIDEO_OVERLAYS: %u", msxvid_tab_overlays(tab));
    CHECK(msxvid_tab_zoom(tab) == 4, "MSX_DEBUGGER_VIDEO_ZOOM: %d", msxvid_tab_zoom(tab));
    CHECK(msxvid_tab_env_section(tab) == MSXVID_S_TIMING, "MSX_DEBUGGER_VIDEO_SECTION: %d", msxvid_tab_env_section(tab));
    msxvid_tab_free(tab);
    if (!failures) printf("vidtab_test: env ok\n");
    return failures;
}

int main(int argc, char **argv)
{
    msxvid_tab *tab;

    if (argc > 1 && !strcmp(argv[1], "env")) return env_main();
    test_names();
    tab = msxvid_tab_new(NULL);
    if (!tab) { fprintf(stderr, "vidtab_test: no tab\n"); return 1; }
    CHECK(msxvid_tab_view(tab) == MSXVID_V_DISPLAY, "default view");
    test_display_and_g1(tab);
    test_registers(tab);
    test_bitmap(tab);
    test_beam_and_sections(tab);
    fuzz(tab);
    msxvid_tab_free(tab);
    msxvid_tab_free(NULL);
    if (!failures) printf("vidtab_test: ok\n");
    return failures;
}
