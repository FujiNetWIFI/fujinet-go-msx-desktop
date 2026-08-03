/*
 * msxvdp: V9918/V9938/V9958 VDP snapshot capture + decoders for the
 * msxdebug engine. See msxvdp.h's header comment for the scope of this
 * pass (register/status/palette/memory in full; bitmap-mode/sprite-mode-2/
 * YJK raster rendering and the command-engine visual pane deferred).
 *
 * Mode decode (msxvdp_decode_mode) mirrors openMSX's own
 * src/video/DisplayMode.hh bit-for-bit -- read directly from that source
 * while writing this, not reconstructed from memory, so it is exactly as
 * authoritative as openMSX's own renderer.
 *
 * Debuggable names confirmed by reading src/video/VDP.cc/VDPVRAM.cc rather
 * than assumed: "VRAM", "physical VRAM", "VDP regs", "VDP status regs",
 * "VDP palette" (VDP::getName() == "VDP" for the sole/primary VDP, which
 * every machine profile this app boots has), and plain "VRAM pointer" --
 * NOT "VDP VRAM pointer" as an earlier note in this project assumed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../openmsx/msx_host.hh"
#include "msxvdp.h"

/* ---- snapshot capture --------------------------------------------------- */

void msxdebug_vdp_snapshot(msxdebug *d, msxvdp_chip chip, msxvdp_snapshot *out)
{
    char sizebuf[16], cmd[64];
    long physsize = 0x4000; /* 16K: a safe MSX1 fallback if "debug size" fails */
    int len = 0;
    (void)d;

    memset(out, 0, sizeof(*out));
    out->chip = chip;

    if (msxhost_execute_sync("debug size {physical VRAM}", sizebuf,
                             sizeof(sizebuf))) {
        long v = strtol(sizebuf, NULL, 10);
        if (v > 0 && v <= MSXVDP_VRAM_MAX) physsize = v;
    }
    out->vram_size = (uint32_t)physsize;

    snprintf(cmd, sizeof(cmd), "debug read_block {physical VRAM} 0 %ld", physsize);
    msxhost_execute_sync_binary(cmd, out->vram, (int)sizeof(out->vram), &len);

    msxhost_execute_sync_binary("debug read_block {VDP regs} 0 64", out->regs,
                                (int)sizeof(out->regs), &len);
    msxhost_execute_sync_binary("debug read_block {VDP status regs} 0 16",
                                out->status, (int)sizeof(out->status), &len);
    if (chip != MSXVDP_CHIP_V9918)
        msxhost_execute_sync_binary("debug read_block {VDP palette} 0 32",
                                    out->palette, (int)sizeof(out->palette), &len);

    {
        /* "VRAM pointer": 2 bytes, low byte then high byte with the top 2
         * bits unused (14 lower bits per its own debuggable description --
         * the 17-bit address the command engine uses internally is not
         * exposed this way; this is the CPU-port address latch). */
        uint8_t ptr[2] = {0, 0};
        if (msxhost_execute_sync_binary("debug read_block {VRAM pointer} 0 2",
                                        ptr, 2, &len) && len >= 2)
            out->addr = (uint32_t)ptr[0] | ((uint32_t)(ptr[1] & 0x3F) << 8);
    }
}

/* ---- register / status / mode text -------------------------------------- */

static void put_rgba(uint8_t *dst, uint8_t r8, uint8_t g8, uint8_t b8)
{
    dst[0] = r8; dst[1] = g8; dst[2] = b8; dst[3] = 0xFF;
}

const char *msxvdp_color_name_v9918(int index)
{
    static const char *const names[16] = {
        "Transparent", "Black",       "Medium Green", "Light Green",
        "Dark Blue",   "Light Blue",  "Dark Red",     "Cyan",
        "Medium Red",  "Light Red",   "Dark Yellow",  "Light Yellow",
        "Dark Green",  "Magenta",     "Gray",         "White",
    };
    return (index >= 0 && index < 16) ? names[index] : "?";
}

void msxvdp_decode_mode(const msxvdp_snapshot *s, msxvdp_mode_info *out)
{
    uint8_t reg25 = (s->chip == MSXVDP_CHIP_V9958) ? s->regs[25] : 0;
    uint8_t byte;

    memset(out, 0, sizeof(*out));
    if ((reg25 & 0x08) == 0) reg25 = 0; /* YJK off => ignore YAE, per DisplayMode.hh */
    byte = (uint8_t)(((reg25 & 0x18) << 2) |      /* YAE YJK */
                    ((s->regs[0] & 0x0E) << 1) |  /* M5..M3 */
                    ((s->regs[1] & 0x08) >> 2) |  /* M2 */
                    ((s->regs[1] & 0x10) >> 4));  /* M1 */
    out->base = (msxvdp_mode)(byte & 0x1F);
    out->yjk = (byte & MSXVDP_MODE_YJK) != 0;
    out->yae = (byte & MSXVDP_MODE_YAE) != 0;
    out->is_v9938_only = (byte & 0x18) != 0;
    out->is_text = (out->base == MSXVDP_MODE_TEXT1 || out->base == MSXVDP_MODE_TEXT2 ||
                    out->base == MSXVDP_MODE_TEXT1Q);
    out->is_bitmap = out->base >= MSXVDP_MODE_GRAPHIC4;
    out->is_planar = (byte & 0x14) == 0x14;
    out->line_width = (byte == MSXVDP_MODE_TEXT2 || byte == MSXVDP_MODE_GRAPHIC5 ||
                       byte == MSXVDP_MODE_GRAPHIC6) ? 512 : 256;
    switch (out->base) {
    case MSXVDP_MODE_GRAPHIC1: case MSXVDP_MODE_MULTICOLOR: case MSXVDP_MODE_GRAPHIC2:
        out->sprite_mode = 1; break;
    case MSXVDP_MODE_MULTIQ:
        out->sprite_mode = (s->chip == MSXVDP_CHIP_V9918) ? 1 : 0; break;
    case MSXVDP_MODE_GRAPHIC3: case MSXVDP_MODE_GRAPHIC4: case MSXVDP_MODE_GRAPHIC5:
    case MSXVDP_MODE_GRAPHIC6: case MSXVDP_MODE_GRAPHIC7:
        out->sprite_mode = 2; break;
    default:
        out->sprite_mode = 0; break;
    }
    out->mode_name = msxvdp_mode_name(out->base, out->yjk, out->yae);
}

const char *msxvdp_mode_name(msxvdp_mode base, int yjk, int yae)
{
    static char buf[32];
    const char *n;
    switch (base) {
    case MSXVDP_MODE_GRAPHIC1:   n = "Graphic 1 (Screen 1)"; break;
    case MSXVDP_MODE_TEXT1:      n = "Text 1 (Screen 0)"; break;
    case MSXVDP_MODE_MULTICOLOR: n = "Multicolor (Screen 3)"; break;
    case MSXVDP_MODE_GRAPHIC2:   n = "Graphic 2 (Screen 2)"; break;
    case MSXVDP_MODE_GRAPHIC3:   n = "Graphic 3 (Screen 4)"; break;
    case MSXVDP_MODE_TEXT2:      n = "Text 2 (Screen 0-width80)"; break;
    case MSXVDP_MODE_GRAPHIC4:   n = "Graphic 4 (Screen 5)"; break;
    case MSXVDP_MODE_GRAPHIC5:   n = "Graphic 5 (Screen 6)"; break;
    case MSXVDP_MODE_GRAPHIC6:   n = "Graphic 6 (Screen 7)"; break;
    case MSXVDP_MODE_GRAPHIC7:   n = "Graphic 7 (Screen 8)"; break;
    case MSXVDP_MODE_TEXT1Q:     n = "undefined (Text1Q)"; break;
    case MSXVDP_MODE_MULTIQ:     n = "undefined (MultiQ)"; break;
    default:                     n = "undefined"; break;
    }
    if (yjk) {
        snprintf(buf, sizeof(buf), "%s %s", n, yae ? "+YAE" : "+YJK");
        return buf;
    }
    return n;
}

const char *msxvdp_register_name(int reg)
{
    static const char *const names[28] = {
        "Mode / EXTVID",        "Mode / BLANK / IE0",     "Name table",
        "Color table",          "Pattern generator",      "Sprite attributes",
        "Sprite patterns",      "Colors (MSX1) / border", "Mode / sprite/color",
        "Line count / EO",      "Color table (high)",     "Sprite attr. (high)",
        "Color burst",          "Blink",                  "VRAM access base",
        "Status reg select",    "H scroll (low)",         "Control / R#pointer",
        "H adjust",             "V adjust",                "Text color 1",
        "Text color 0 / blink", "V scroll",                "H scroll (high)",
        "H display adjust",     "Mode / YJK / YAE / MSK",  "V scroll (high)",
        "Command color 2 -- unused",
    };
    return (reg >= 0 && reg < 28) ? names[reg] : "?";
}

static int describe_r0_r1(const msxvdp_snapshot *s, int reg, char *out, int max)
{
    if (reg == 0)
        return snprintf(out, (size_t)max,
                        "M3=%d M4=%d M5=%d  IE1=%d  EXTVID=%d",
                        (s->regs[0] >> 1) & 1, (s->regs[0] >> 2) & 1,
                        (s->regs[0] >> 3) & 1, (s->regs[0] >> 5) & 1,
                        s->regs[0] & 1);
    return snprintf(out, (size_t)max,
                    "M1=%d M2=%d  BLANK=%s  IE0=%d  SIZE=%s  MAG=%d",
                    (s->regs[1] >> 4) & 1, (s->regs[1] >> 3) & 1,
                    (s->regs[1] & 0x40) ? "on" : "OFF", (s->regs[1] >> 5) & 1,
                    (s->regs[1] & 0x02) ? "16x16" : "8x8", s->regs[1] & 1);
}

int msxvdp_describe_register(const msxvdp_snapshot *s, int reg, char *out,
                             int max)
{
    int n;
    if (reg < 0 || reg > 27 || max <= 0) {
        if (max > 0) out[0] = '\0';
        return 0;
    }
    switch (reg) {
    case 0: case 1:
        n = describe_r0_r1(s, reg, out, max);
        break;
    case 2:
        n = snprintf(out, (size_t)max, "base $%04X",
                    (unsigned)(s->regs[2] & 0x0F) << 10);
        break;
    case 3:
        n = snprintf(out, (size_t)max, "base $%04X (low; R10 supplies bits 14-8)",
                    (unsigned)s->regs[3] << 6);
        break;
    case 4:
        n = snprintf(out, (size_t)max, "base $%04X (low; R4 bit2 + R10 supply high bits in bitmap modes)",
                    (unsigned)(s->regs[4] & 0x07) << 11);
        break;
    case 5:
        n = snprintf(out, (size_t)max, "base $%04X (low; R11 supplies bits 16-15)",
                    (unsigned)(s->regs[5] & 0x7F) << 7);
        break;
    case 6:
        n = snprintf(out, (size_t)max, "base $%04X",
                    (unsigned)(s->regs[6] & 0x3F) << 11);
        break;
    case 7:
        if (s->chip == MSXVDP_CHIP_V9918)
            n = snprintf(out, (size_t)max, "text FG %u, backdrop %u",
                        s->regs[7] >> 4, s->regs[7] & 0x0F);
        else
            n = snprintf(out, (size_t)max, "border color index %u", s->regs[7]);
        break;
    case 8:
        n = snprintf(out, (size_t)max, "sprite-off=%d  color0-on=%d  transparency-off=%d",
                    (s->regs[8] >> 1) & 1, (s->regs[8] >> 5) & 1, (s->regs[8] >> 5) & 1);
        break;
    case 9:
        n = snprintf(out, (size_t)max, "lines=%s  EO=%d  NT=%d(%s)  PAL/NTSC=%s",
                    (s->regs[9] & 0x80) ? "212" : "192", (s->regs[9] >> 1) & 1,
                    s->regs[9] & 1, (s->regs[9] & 1) ? "interlace" : "non-interlace",
                    (s->regs[9] & 0x02) ? "PAL" : "NTSC");
        break;
    case 15:
        n = snprintf(out, (size_t)max, "S#%u selected", s->regs[15] & 0x0F);
        break;
    case 25:
        n = snprintf(out, (size_t)max, "YJK=%d  YAE=%d  MSK=%d  SP2=%d",
                    (s->regs[25] >> 3) & 1, (s->regs[25] >> 4) & 1,
                    (s->regs[25] >> 1) & 1, s->regs[25] & 1);
        break;
    default:
        n = snprintf(out, (size_t)max, "$%02X", s->regs[reg]);
        break;
    }
    return (n < 0) ? 0 : (n >= max ? max - 1 : n);
}

int msxvdp_describe_status(const msxvdp_snapshot *s, char *out, int max)
{
    int n = snprintf(out, (size_t)max,
                     "S#0: F=%d 5S=%d C=%d 5th-sprite=%u | S#1: FH=%d FL=%d BD=%d ID=%u",
                     (s->status[0] & 0x80) ? 1 : 0, (s->status[0] & 0x40) ? 1 : 0,
                     (s->status[0] & 0x20) ? 1 : 0, s->status[0] & 0x1F,
                     (s->status[1] & 0x40) ? 1 : 0, (s->status[1] & 0x10) ? 1 : 0,
                     (s->status[1] & 0x08) ? 1 : 0, s->status[1] & 0x0F);
    return (n < 0) ? 0 : (n >= max ? max - 1 : n);
}

int msxvdp_describe_command_engine(const msxvdp_snapshot *s, char *out, int max)
{
    unsigned sx, sy, dx, dy, nx, ny, cmd, lop;
    int n;

    sx = (unsigned)s->regs[32] | ((unsigned)(s->regs[33] & 0x01) << 8);
    sy = (unsigned)s->regs[34] | ((unsigned)(s->regs[35] & 0x03) << 8);
    dx = (unsigned)s->regs[36] | ((unsigned)(s->regs[37] & 0x01) << 8);
    dy = (unsigned)s->regs[38] | ((unsigned)(s->regs[39] & 0x03) << 8);
    nx = (unsigned)s->regs[40] | ((unsigned)(s->regs[41] & 0x03) << 8);
    ny = (unsigned)s->regs[42] | ((unsigned)(s->regs[43] & 0x03) << 8);
    cmd = (unsigned)(s->regs[46] >> 4);
    lop = (unsigned)(s->regs[46] & 0x0F);

    n = snprintf(out, (size_t)max,
                "SX=%u SY=%u DX=%u DY=%u NX=%u NY=%u CLR=$%02X ARG=$%02X CMD=%u LOP=%u  CE=%d TR=%d",
                sx, sy, dx, dy, nx, ny, s->regs[44], s->regs[45], cmd, lop,
                (s->status[2] & 0x01) ? 1 : 0, (s->status[2] & 0x80) ? 1 : 0);
    return (n < 0) ? 0 : (n >= max ? max - 1 : n);
}

static void appendf(char *out, int max, int *pos, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (*pos >= max - 1) return;
    va_start(ap, fmt);
    n = vsnprintf(out + *pos, (size_t)(max - *pos), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    *pos += (n >= max - *pos) ? (max - *pos - 1) : n;
}

int msxvdp_format_state(const msxvdp_snapshot *s, char *out, int max)
{
    msxvdp_mode_info mi;
    int pos = 0, i;
    static const char *const chip_name[3] = {"V9918 (MSX1)", "V9938 (MSX2)",
                                             "V9958 (MSX2+)"};

    if (max <= 0) return 0;
    out[0] = '\0';
    msxvdp_decode_mode(s, &mi);

    appendf(out, max, &pos, "Chip  %s\n", chip_name[s->chip]);
    appendf(out, max, &pos, "Mode  %s  (%u px wide, sprite mode %d)\n",
            mi.mode_name, mi.line_width, mi.sprite_mode);
    appendf(out, max, &pos, "VRAM address counter $%05X   VRAM size %u bytes\n\n",
            (unsigned)s->addr, (unsigned)s->vram_size);

    appendf(out, max, &pos, "Registers\n");
    for (i = 0; i < 28; i++) {
        char detail[160];
        msxvdp_describe_register(s, i, detail, (int)sizeof(detail));
        appendf(out, max, &pos, "  R%-2d $%02X  %-24s %s\n", i, s->regs[i],
                msxvdp_register_name(i), detail);
    }
    {
        char detail[160];
        msxvdp_describe_status(s, detail, (int)sizeof(detail));
        appendf(out, max, &pos, "\nStatus  %s\n", detail);
    }
    if (s->chip != MSXVDP_CHIP_V9918) {
        char detail[160];
        msxvdp_describe_command_engine(s, detail, (int)sizeof(detail));
        appendf(out, max, &pos, "\nCommand engine (R32-R46)\n  %s\n", detail);
    }
    return pos;
}

int msxvdp_format_hex(const msxvdp_snapshot *s, uint32_t base, int rows,
                      char *out, int max)
{
    int pos = 0, row, col;
    uint32_t size = s->vram_size ? s->vram_size : 1;

    if (max <= 0) return 0;
    out[0] = '\0';
    for (row = 0; row < rows; row++) {
        uint32_t addr = (base + (uint32_t)row * 16) % size;
        appendf(out, max, &pos, "%05X  ", addr);
        for (col = 0; col < 16; col++)
            appendf(out, max, &pos, "%02X ", s->vram[(addr + (uint32_t)col) % size]);
        appendf(out, max, &pos, " ");
        for (col = 0; col < 16; col++) {
            uint8_t c = s->vram[(addr + (uint32_t)col) % size];
            appendf(out, max, &pos, "%c", (c >= 0x20 && c <= 0x7E) ? (char)c : '.');
        }
        appendf(out, max, &pos, "\n");
    }
    return pos;
}

/* ---- poke-line parsing (same grammar as adam-desktop's, 32-bit address) - */

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *skip_seps(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == ',') p++;
    return p;
}

int msxvdp_parse_poke(const char *text, uint32_t *addr, uint8_t *bytes, int max)
{
    const char *p = skip_seps(text);
    unsigned long v = 0;
    int digits = 0, n = 0;

    if (*p == '$') p++;
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    while (hex_digit((unsigned char)*p) >= 0) {
        v = v * 16 + (unsigned long)hex_digit((unsigned char)*p++);
        digits++;
    }
    if (digits == 0 || digits > 6) return -1;
    *addr = (uint32_t)(v & 0x3FFFF);

    p = skip_seps(p);
    if (*p == ':' || *p == '=') p++;
    p = skip_seps(p);

    while (*p) {
        int td = 0;
        while (hex_digit((unsigned char)*p) >= 0) {
            int hi = hex_digit((unsigned char)*p++);
            int lo;
            if (td == 0 && hex_digit((unsigned char)*p) < 0) {
                if (n >= max) return -1;
                bytes[n++] = (uint8_t)hi;
                td += 2;
                break;
            }
            lo = hex_digit((unsigned char)*p);
            if (lo < 0) return -1;
            p++;
            if (n >= max) return -1;
            bytes[n++] = (uint8_t)((hi << 4) | lo);
            td += 2;
        }
        if (td == 0) return -1;
        p = skip_seps(p);
    }
    return n;
}

/* ---- rendering: MSX1-compatible modes only (see msxvdp.h header) -------- */

static void fill_gray(uint8_t *rgba, int w, int h)
{
    int i;
    for (i = 0; i < w * h; i++) put_rgba(rgba + i * 4, 0x60, 0x60, 0x60);
}

void msxvdp_render_nametable(const msxvdp_snapshot *s, uint8_t *rgba)
{
    msxvdp_mode_info mi;
    const uint16_t nt = (uint16_t)((s->regs[2] & 0x0F) << 10);
    int row, col, y, x;

    msxvdp_decode_mode(s, &mi);
    if (mi.base != MSXVDP_MODE_GRAPHIC1 && mi.base != MSXVDP_MODE_GRAPHIC2 &&
        mi.base != MSXVDP_MODE_MULTICOLOR && mi.base != MSXVDP_MODE_TEXT1) {
        fill_gray(rgba, 256, 192);
        return;
    }

    /* MSX1-compatible modes: same table addressing ADAM's vdpview.c
     * validated, just re-derived from this file's own V9938 register
     * snapshot rather than a TMS9918A-only one. Colors resolved straight
     * from the fixed TMS9918A table below via palette RAM if this is a
     * real V9938/58 chip (its power-on default palette matches the
     * TMS9918A colors), else msxvdp_color_name_v9918's implicit RGB set. */
    {
        static const uint8_t tms_rgb[16][3] = {
            {0,0,0},{0,0,0},{0x21,0xC8,0x42},{0x5E,0xDC,0x78},
            {0x54,0x55,0xED},{0x7D,0x76,0xFC},{0xD4,0x52,0x4D},{0x42,0xEB,0xF5},
            {0xFC,0x55,0x54},{0xFF,0x79,0x78},{0xD4,0xC1,0x54},{0xE6,0xCE,0x80},
            {0x21,0xB0,0x3B},{0xC9,0x5B,0xBA},{0xCC,0xCC,0xCC},{0xFF,0xFF,0xFF},
        };
        const int g2 = (mi.base == MSXVDP_MODE_GRAPHIC2);
        const int txt = (mi.base == MSXVDP_MODE_TEXT1);
        const uint16_t pg_base = g2 ? (uint16_t)((s->regs[4] & 0x04) << 11)
                                    : (uint16_t)((s->regs[4] & 0x07) << 11);
        const uint16_t ct_base = g2 ? (uint16_t)((s->regs[3] & 0x80) << 6)
                                    : (uint16_t)(s->regs[3] << 6);
        const uint16_t pg_mask = (uint16_t)(((s->regs[4] & 0x03) << 8) | 0xFF);
        const uint16_t ct_mask = (uint16_t)(((s->regs[3] & 0x7F) << 3) | 0x07);
        const uint8_t backdrop = (uint8_t)(s->regs[7] & 0x0F);

        if (txt) {
            const uint8_t fg_idx = (uint8_t)((s->regs[7] >> 4) & 0x0F);
            for (y = 0; y < 192; y++)
                for (x = 0; x < 256; x++)
                    put_rgba(rgba + (y * 256 + x) * 4, tms_rgb[backdrop][0],
                            tms_rgb[backdrop][1], tms_rgb[backdrop][2]);
            for (row = 0; row < 24; row++) {
                for (col = 0; col < 40; col++) {
                    uint8_t name = s->vram[(nt + row * 40 + col) % s->vram_size];
                    for (y = 0; y < 8; y++) {
                        uint8_t bits = s->vram[(pg_base + name * 8 + y) % s->vram_size];
                        for (x = 0; x < 6; x++) {
                            int px = col * 6 + x + 8, py = row * 8 + y;
                            if (px >= 256) continue;
                            const uint8_t *c = (bits & (0x80 >> x)) ? tms_rgb[fg_idx] : tms_rgb[backdrop];
                            put_rgba(rgba + (py * 256 + px) * 4, c[0], c[1], c[2]);
                        }
                    }
                }
            }
            return;
        }

        for (row = 0; row < 24; row++) {
            int bank_entry = row / 8 * 256;
            for (col = 0; col < 32; col++) {
                uint8_t name = s->vram[(nt + row * 32 + col) % s->vram_size];
                for (y = 0; y < 8; y++) {
                    uint16_t pat_off, col_off;
                    uint8_t bits, color, fg, bg;
                    if (g2) {
                        uint16_t entry = (uint16_t)((bank_entry + name) & pg_mask);
                        uint16_t centry = (uint16_t)((bank_entry + name) & ct_mask);
                        pat_off = (uint16_t)(pg_base + entry * 8 + y);
                        col_off = (uint16_t)(ct_base + centry * 8 + y);
                    } else {
                        pat_off = (uint16_t)(pg_base + name * 8 + y);
                        col_off = (uint16_t)(ct_base + name / 8);
                    }
                    bits = s->vram[pat_off % s->vram_size];
                    color = s->vram[col_off % s->vram_size];
                    fg = (uint8_t)(color >> 4);
                    bg = (uint8_t)(color & 0x0F);
                    for (x = 0; x < 8; x++) {
                        uint8_t ci = (bits & (0x80 >> x)) ? fg : bg;
                        const uint8_t *c = ci ? tms_rgb[ci] : tms_rgb[backdrop];
                        put_rgba(rgba + ((row * 8 + y) * 256 + col * 8 + x) * 4,
                                c[0], c[1], c[2]);
                    }
                }
            }
        }
    }
}

void msxvdp_render_patterns(const msxvdp_snapshot *s, int bank, uint8_t *rgba)
{
    msxvdp_mode_info mi;
    int g2, tile, y, x;
    uint16_t pg_base, pg_mask;

    msxvdp_decode_mode(s, &mi);
    if (mi.base != MSXVDP_MODE_GRAPHIC1 && mi.base != MSXVDP_MODE_GRAPHIC2 &&
        mi.base != MSXVDP_MODE_MULTICOLOR && mi.base != MSXVDP_MODE_TEXT1) {
        fill_gray(rgba, 256, 64);
        return;
    }
    g2 = (mi.base == MSXVDP_MODE_GRAPHIC2);
    pg_base = g2 ? (uint16_t)((s->regs[4] & 0x04) << 11) : (uint16_t)((s->regs[4] & 0x07) << 11);
    pg_mask = g2 ? (uint16_t)(((s->regs[4] & 0x03) << 8) | 0xFF) : 0x00FF;

    for (tile = 0; tile < 256; tile++) {
        int cell_x = (tile % 32) * 8, cell_y = (tile / 32) * 8;
        unsigned idx = ((unsigned)(g2 ? bank & 3 : 0) * 256 + (unsigned)tile) & pg_mask;
        for (y = 0; y < 8; y++) {
            uint8_t bits = s->vram[(pg_base + idx * 8 + y) % s->vram_size];
            for (x = 0; x < 8; x++) {
                uint8_t *px = rgba + ((cell_y + y) * 256 + cell_x + x) * 4;
                uint8_t v = (bits & (0x80 >> x)) ? 0xE6 : 0x20;
                px[0] = px[1] = px[2] = v;
                px[3] = 0xFF;
            }
        }
    }
}

void msxvdp_render_sprites(const msxvdp_snapshot *s, uint8_t *rgba,
                           msxvdp_sprite info[32])
{
    static const uint8_t tms_rgb[16][3] = {
        {0,0,0},{0,0,0},{0x21,0xC8,0x42},{0x5E,0xDC,0x78},
        {0x54,0x55,0xED},{0x7D,0x76,0xFC},{0xD4,0x52,0x4D},{0x42,0xEB,0xF5},
        {0xFC,0x55,0x54},{0xFF,0x79,0x78},{0xD4,0xC1,0x54},{0xE6,0xCE,0x80},
        {0x21,0xB0,0x3B},{0xC9,0x5B,0xBA},{0xCC,0xCC,0xCC},{0xFF,0xFF,0xFF},
    };
    msxvdp_mode_info mi;
    const uint16_t sat = (uint16_t)((s->regs[5] & 0x7F) << 7);
    const uint16_t spg = (uint16_t)((s->regs[6] & 0x3F) << 11);
    const int size16 = (s->regs[1] & 0x02) != 0;
    int i, y, x;

    memset(rgba, 0, (size_t)128 * 64 * 4);
    msxvdp_decode_mode(s, &mi);
    if (mi.sprite_mode != 1) {
        /* Sprite mode 2 (bitmap/Graphic3+ modes) rendering deferred -- see
         * msxvdp.h. */
        return;
    }
    for (i = 0; i < 32; i++) {
        const uint8_t *e = &s->vram[(sat + (uint32_t)i * 4) % s->vram_size];
        int pattern = size16 ? (e[2] & 0xFC) : e[2];
        uint8_t color_idx = (uint8_t)(e[3] & 0x0F);
        int cell_x = (i % 8) * 16, cell_y = (i / 8) * 16;
        int quads = size16 ? 4 : 1, q;

        if (info) {
            info[i].y = e[0];
            info[i].x = e[1];
            info[i].pattern = pattern;
            info[i].color = color_idx;
            info[i].early_clock = (e[3] & 0x80) != 0;
        }
        for (q = 0; q < quads; q++) {
            int qx = size16 ? (q / 2) * 8 : 0;
            int qy = size16 ? (q % 2) * 8 : 0;
            for (y = 0; y < 8; y++) {
                uint8_t bits = s->vram[(spg + (uint32_t)(pattern + q) * 8 + (uint32_t)y) % s->vram_size];
                for (x = 0; x < 8; x++) {
                    if (bits & (0x80 >> x)) {
                        const uint8_t *c = color_idx ? tms_rgb[color_idx] : tms_rgb[14];
                        put_rgba(rgba + ((cell_y + qy + y) * 128 + cell_x + qx + x) * 4,
                                c[0], c[1], c[2]);
                    }
                }
            }
        }
    }
}

static const uint8_t digit_font[16][5] = {
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},
    {5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,2,2,2},
    {7,5,7,5,7},{7,5,7,1,7},{7,5,7,5,5},{6,5,6,5,6},
    {7,4,4,4,7},{6,5,5,5,6},{7,4,7,4,7},{7,4,7,4,4},
};

static void fill_rect(uint8_t *rgba, int x0, int y0, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b)
{
    int x, y;
    for (y = y0; y < y0 + h; y++) {
        uint8_t *row = rgba + ((size_t)y * MSXVDP_PAL_W + (size_t)x0) * 4;
        for (x = 0; x < w; x++, row += 4) {
            row[0] = r; row[1] = g; row[2] = b; row[3] = 0xFF;
        }
    }
}

void msxvdp_render_palette(const msxvdp_snapshot *s, uint8_t *rgba)
{
    static const uint8_t tms_rgb[16][3] = {
        {0,0,0},{0,0,0},{0x21,0xC8,0x42},{0x5E,0xDC,0x78},
        {0x54,0x55,0xED},{0x7D,0x76,0xFC},{0xD4,0x52,0x4D},{0x42,0xEB,0xF5},
        {0xFC,0x55,0x54},{0xFF,0x79,0x78},{0xD4,0xC1,0x54},{0xE6,0xCE,0x80},
        {0x21,0xB0,0x3B},{0xC9,0x5B,0xBA},{0xCC,0xCC,0xCC},{0xFF,0xFF,0xFF},
    };
    const int inset = 3, scale = 2;
    int i;

    fill_rect(rgba, 0, 0, MSXVDP_PAL_W, MSXVDP_PAL_H, 0x1E, 0x1E, 0x22);
    for (i = 0; i < 16; i++) {
        const int cx = (i % MSXVDP_PAL_COLS) * MSXVDP_PAL_CELL + inset;
        const int cy = (i / MSXVDP_PAL_COLS) * MSXVDP_PAL_CELL + inset;
        const int side = MSXVDP_PAL_CELL - inset * 2;
        uint8_t r, g, b, lr, lg, lb;
        int x, y, row;

        if (s->chip == MSXVDP_CHIP_V9918) {
            r = tms_rgb[i][0]; g = tms_rgb[i][1]; b = tms_rgb[i][2];
        } else {
            /* "VDP palette" bytes: low = 0RRR0BBB, high = 00000GGG (RBG,
             * 3 bits/channel) -- see this file's header comment / VDP.cc's
             * PaletteDebug::read. */
            uint8_t lo = s->palette[i * 2], hi = s->palette[i * 2 + 1];
            uint8_t r3 = (lo >> 4) & 0x07, b3 = lo & 0x07, g3 = hi & 0x07;
            r = (uint8_t)((r3 << 5) | (r3 << 2) | (r3 >> 1));
            g = (uint8_t)((g3 << 5) | (g3 << 2) | (g3 >> 1));
            b = (uint8_t)((b3 << 5) | (b3 << 2) | (b3 >> 1));
        }
        fill_rect(rgba, cx - 1, cy - 1, side + 2, side + 2, 0x6E, 0x6E, 0x78);
        fill_rect(rgba, cx, cy, side, side, r, g, b);
        if (i == 0 && s->chip != MSXVDP_CHIP_V9918) {
            for (y = 0; y < side; y++)
                for (x = 0; x < side; x++)
                    if (((x / 5) + (y / 5)) & 1)
                        fill_rect(rgba, cx + x, cy + y, 1, 1, 0x4A, 0x4A, 0x52);
        }
        {
            int luma = (r * 299 + g * 587 + b * 114) / 1000;
            uint8_t v = (uint8_t)(luma > 128 ? 0x00 : 0xFF);
            lr = lg = lb = v;
        }
        for (row = 0; row < 5; row++)
            for (x = 0; x < 3; x++)
                if (digit_font[i][row] & (0x04 >> x))
                    fill_rect(rgba, cx + 3 + x * scale, cy + 3 + row * scale,
                             scale, scale, lr, lg, lb);
    }
}
