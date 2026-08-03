/*
 * msxvdp -- VDP (V9918/V9938/V9958) snapshot + decoders for the msxdebug
 * engine's VDP view. Widens fujinet-go-adam-desktop's TMS9918A-only
 * vdpview.c/adamvdp_* (the MSX1 VDP *is* a TMS9918A, so that code's
 * approach carries over exactly for the MSX1-compatible modes) with the
 * V9938/V9958 register set, palette RAM, and mode table.
 *
 * SCOPE, stated plainly rather than silently narrowed: this pass ports the
 * register/status/palette/memory side of the VDP debugger in full --
 * snapshot capture from openMSX's real debuggables, the complete V9938/
 * V9958 mode table (via openMSX's own src/video/DisplayMode.hh encoding,
 * not reconstructed from memory), register/status text description, a
 * physical-VRAM hex dump, and palette swatch rendering. It does NOT yet
 * widen the nametable/pattern/sprite RASTER rendering
 * (msxvdp_render_nametable et al) to the V9938 bitmap modes (Graphic 4-7),
 * sprite mode 2, or YJK/YAE color decode, or add a command-engine pane --
 * those render TEXT1/GRAPHIC1/GRAPHIC2/MULTICOLOR (the MSX1-compatible
 * modes, byte-identical to a TMS9918A) via the same logic ADAM's vdpview.c
 * already validated, and report "not yet decoded in this mode" for the
 * V9938-only bitmap/YJK modes rather than rendering garbage -- the same
 * honest-failure precedent ADAMVDP_MODE_INVALID already set. See TODO for
 * the follow-up scope.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MSXVDP_H
#define MSXVDP_H

#include <stdint.h>

#include "msxdebug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Largest "physical VRAM" any real MSX2/2+ configuration exposes (128K
 * base + 64K expansion, per the debuggable's own doc -- see
 * msxdebug_vdp_snapshot). */
#define MSXVDP_VRAM_MAX 0x30000

typedef enum {
    MSXVDP_CHIP_V9918 = 0, /* MSX1: a real TMS9918A */
    MSXVDP_CHIP_V9938,     /* MSX2 */
    MSXVDP_CHIP_V9958,     /* MSX2+ / turboR: adds YJK/YAE */
} msxvdp_chip;

/* Display modes, matching openMSX's own DisplayMode encoding (src/video/
 * DisplayMode.hh) exactly -- copied from there, not reconstructed, so this
 * table is exactly as authoritative as openMSX's own renderer. */
typedef enum {
    MSXVDP_MODE_GRAPHIC1   = 0x00,
    MSXVDP_MODE_TEXT1      = 0x01,
    MSXVDP_MODE_MULTICOLOR = 0x02,
    MSXVDP_MODE_GRAPHIC2   = 0x04,
    MSXVDP_MODE_TEXT1Q     = 0x05, /* undocumented / bogus combination */
    MSXVDP_MODE_MULTIQ     = 0x06, /* undocumented / bogus combination */
    MSXVDP_MODE_GRAPHIC3   = 0x08,
    MSXVDP_MODE_TEXT2      = 0x09,
    MSXVDP_MODE_GRAPHIC4   = 0x0C,
    MSXVDP_MODE_GRAPHIC5   = 0x10,
    MSXVDP_MODE_GRAPHIC6   = 0x14,
    MSXVDP_MODE_GRAPHIC7   = 0x1C,
} msxvdp_mode;
#define MSXVDP_MODE_YJK 0x20 /* flag bit, ORed onto the base mode above */
#define MSXVDP_MODE_YAE 0x40 /* flag bit; only meaningful together with YJK */

typedef struct {
    uint8_t  vram[MSXVDP_VRAM_MAX]; /* "physical VRAM": mode-independent */
    uint32_t vram_size;             /* bytes actually populated above */
    uint8_t  regs[64];              /* "VDP regs": R0-R46 (incl. R32-R46 the command engine) */
    uint8_t  status[16];            /* "VDP status regs": S#0-S#9 */
    uint8_t  palette[32];           /* "VDP palette": 16 entries, RBG, low/high byte */
    uint32_t addr;                  /* "VDP VRAM pointer": 17-bit */
    msxvdp_chip chip;
} msxvdp_snapshot;

/* Fills out from the live VDP via msxhost_execute_sync_binary("debug
 * read_block ...") against the "physical VRAM", "VDP regs", "VDP status
 * regs", "VDP palette" and "VRAM pointer" debuggables (names confirmed by
 * reading src/video/VDP.cc/VDPVRAM.cc -- note the last one is bare "VRAM
 * pointer", not "VDP VRAM pointer"). chip is determined once at session
 * start (from the booted machine's config) and passed in rather than
 * guessed here. */
void msxdebug_vdp_snapshot(msxdebug *d, msxvdp_chip chip, msxvdp_snapshot *out);

/* ---- mode decode ----------------------------------------------------- */

typedef struct {
    msxvdp_mode base;
    int yjk, yae;      /* V9958 only; yae implies yjk */
    const char *mode_name;
    int is_text, is_bitmap, is_planar, is_v9938_only;
    int sprite_mode;   /* 0 = none, 1 = mode 1 (MSX1-style), 2 = mode 2 */
    int line_width;    /* 256 or 512 */
} msxvdp_mode_info;

/* Decodes R0/R1/R25 into the full mode -- same bit arithmetic as openMSX's
 * own DisplayMode(reg0, reg1, reg25) constructor. */
void msxvdp_decode_mode(const msxvdp_snapshot *s, msxvdp_mode_info *out);

/* "Graphic 2", "Text 1", "Graphic 7 (YJK)", ... */
const char *msxvdp_mode_name(msxvdp_mode base, int yjk, int yae);

/* ---- register / status text ------------------------------------------- */

/* R0-R27's name ("Mode/EXTVID", "Sprite attribute table", ...); registers
 * above 27 (the command engine) are named individually since they don't
 * fit the same one-line-per-register table shape -- see
 * msxvdp_describe_command_engine. */
const char *msxvdp_register_name(int reg);

/* One decoded text line for register reg (0-27). Table registers resolve
 * to their VRAM address; R0/R1/R7/R8/R9 etc. report their documented bit
 * fields. Returns the length written. */
int msxvdp_describe_register(const msxvdp_snapshot *s, int reg, char *out,
                             int max);
int msxvdp_describe_status(const msxvdp_snapshot *s, char *out, int max);

/* SX/SY/DX/DY/NX/NY/CLR/ARG and the command register (R46: upper nibble
 * command code, lower nibble logical operator) plus S#2's CE/TR bits.
 * Numeric only (raw values and the command/logical-op nibbles split out)
 * rather than a full command-name table -- see this file's header comment
 * for what is deferred. */
int msxvdp_describe_command_engine(const msxvdp_snapshot *s, char *out,
                                   int max);

/* The whole decode as monospace text -- mode, VRAM pointer, then one
 * decoded line per register plus status plus the command engine block.
 * Returns the length written. */
int msxvdp_format_state(const msxvdp_snapshot *s, char *out, int max);

/* Classic hex dump of physical VRAM: rows of 16 bytes, "ADDR  xx..xx
 * ascii", starting at base (wrapping at s->vram_size). Returns the length
 * written. */
int msxvdp_format_hex(const msxvdp_snapshot *s, uint32_t base, int rows,
                      char *out, int max);

/* Same poke-line grammar as adam-desktop's adamvdp_parse_poke (hex
 * address, then optional ':'/'=' and any number of hex-byte tokens);
 * widened only in that addr is 32-bit (17-bit VRAM addressing). */
int msxvdp_parse_poke(const char *text, uint32_t *addr, uint8_t *bytes,
                      int max);

/* ---- rendering ---------------------------------------------------------
 * RGBA8888. Nametable/pattern/sprite views render the MSX1-compatible
 * modes (Graphic 1/2/3, Multicolor, Text 1) exactly as ADAM's did; for any
 * other mode they fill the buffer with a mid-gray and the caller should
 * prefer msxvdp_format_state's text decode instead (see this file's header
 * comment). Sizes fixed: nametable 256x192, patterns 256x64, sprites
 * 128x64, palette MSXVDP_PAL_W x MSXVDP_PAL_H. */
void msxvdp_render_nametable(const msxvdp_snapshot *s, uint8_t *rgba);
void msxvdp_render_patterns(const msxvdp_snapshot *s, int bank, uint8_t *rgba);

typedef struct {
    int y, x;
    int pattern;
    int color;
    int early_clock;
} msxvdp_sprite;
void msxvdp_render_sprites(const msxvdp_snapshot *s, uint8_t *rgba,
                           msxvdp_sprite info[32]);

#define MSXVDP_PAL_CELL 40
#define MSXVDP_PAL_COLS 8
#define MSXVDP_PAL_ROWS 2
#define MSXVDP_PAL_W (MSXVDP_PAL_CELL * MSXVDP_PAL_COLS)
#define MSXVDP_PAL_H (MSXVDP_PAL_CELL * MSXVDP_PAL_ROWS)
/* V9938/58: reads the live palette RAM. V9918 (MSX1): falls back to the
 * TMS9918A's fixed 16-color table, since that chip has no palette RAM. */
void msxvdp_render_palette(const msxvdp_snapshot *s, uint8_t *rgba);

const char *msxvdp_color_name_v9918(int index); /* fixed TMS9918A names */

#ifdef __cplusplus
}
#endif

#endif /* MSXVDP_H */
