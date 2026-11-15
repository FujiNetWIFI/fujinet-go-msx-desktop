/*
 * msxvidcap -- the VDP's state as a frame left it, for the Video tab
 * (msxvidtab.h). Taken once per published frame on the openMSX thread
 * (msxhost_notify_frame in core/openmsx/msx_host.cc) while a tab exists, so a
 * reader sees one consistent frame while the machine runs; a machine parked
 * by the debugger is read live (msxhost_capture_fill).
 *
 * What a capture holds:
 *   - pixels: the published frame, XRGB8888, always 640 x 480 (openMSX scales
 *     every source to it), the very pixels msxsession_copy_frame hands out;
 *   - the VDP at the end of the frame: control and command registers, status
 *     registers, palette RAM (V9938 and later) and all of VRAM, in openMSX's
 *     "physical VRAM" order (Graphic 6 and 7 keep the two interleaved banks);
 *   - the per-line log of the frame (msx_host.hh's msxhost_vdp_capture_*):
 *     every register and palette write with the line it happened at, on top
 *     of the registers and palette at the frame's start. This is what makes
 *     split screens, raster palette changes and scrolling visible: a VDP,
 *     unlike the STIC, draws each line from the registers as they stand then.
 *
 * The log is copied here as its own plain types so that a frontend including
 * msxvidtab.h needs no openMSX header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSXVIDCAP_H
#define MSXVIDCAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MSXVID_FB_W 640
#define MSXVID_FB_H 480
#define MSXVID_VRAM_MAX 0x30000     /* 128K + 64K expansion */
#define MSXVID_MAX_EVENTS 4096

enum {
    MSXVID_EV_REG = 0,          /* a control/command register write that changed it */
    MSXVID_EV_PALETTE = 1,      /* a palette entry changed; value = 0x0GRB (see below) */
    MSXVID_EV_REG_SAME = 2      /* a write of the value already there */
};

typedef struct msxvid_event {
    uint16_t line;      /* absolute line in the frame (0 = frame start) */
    uint16_t column;    /* VDP clock (21 MHz) within the line, 0..1367 */
    uint8_t kind;       /* MSXVID_EV_* */
    uint8_t index;      /* register number, or palette entry */
    uint16_t value;     /* register value; a palette entry as openMSX's 0x0GRB
                         * word (bits 8-10 green, 4-6 red, 0-2 blue) */
} msxvid_event;

typedef struct msxvid_log {
    int valid;                       /* 0: no frame recorded */
    uint32_t frame;                  /* the VDP's frame counter */
    int lines_per_frame;             /* 262 NTSC, 313 PAL */
    int pal;
    uint8_t regs[32];                /* control registers at the frame's start */
    uint16_t palette[16];            /* 0x0GRB words at the frame's start */
    int progress_sx, progress_sy;    /* command in progress, -1 when none */
    int progress_dx, progress_dy;
    int last_command;                /* the last command the engine ran, -1 when none */
    uint32_t event_count;            /* valid entries in events */
    uint32_t dropped;                /* events lost past MSXVID_MAX_EVENTS */
    msxvid_event events[MSXVID_MAX_EVENTS];
} msxvid_log;

typedef struct msxvid_capture {
    uint32_t pixels[MSXVID_FB_W * MSXVID_FB_H];  /* the published frame, 0x00RRGGBB */
    int fb_valid;
    uint8_t regs[64];           /* R#0-46 as the VDP holds them */
    uint8_t status[16];         /* S#0-9 */
    uint8_t palette[32];        /* 16 entries: low byte 0RRR0BBB, high byte 00000GGG */
    int has_palette;            /* a V9938 or later: palette RAM exists */
    int chip;                   /* msxvdp_chip: 0 V9918, 1 V9938, 2 V9958 */
    uint32_t vram_size;         /* bytes of vram that are populated */
    uint8_t vram[MSXVID_VRAM_MAX];
    msxvid_log log;             /* the last completed frame */
    msxvid_log cur;             /* parked only: the frame in progress, up to the beam */
    int beam_line;              /* parked only: absolute line the beam is on, -1 unknown */
    int beam_cycle;             /* parked only: VDP clock within it, -1 unknown */
    uint64_t frame;             /* frames captured since the tab asked */
    uint64_t serial;            /* 0: nothing captured yet */
    int live;                   /* the VDP part was read from a parked machine */
} msxvid_capture;

/* Frames are captured only while at least one tab wants them (a count:
 * enable(1) from each tab, enable(0) when it is freed). The first enable also
 * starts the per-line log, the last stops it. Thread-safe. */
void msxhost_capture_enable(int enable);

/* Copies the latest captured frame into dst iff its serial differs from
 * *serial_inout, updates it and returns 1; 0 and dst untouched otherwise.
 * *serial_inout = 0 forces a copy once something has been captured. */
int msxhost_capture_get(msxvid_capture *dst, uint64_t *serial_inout);

/* Overwrites the VDP part of dst (registers, status, palette, VRAM, the
 * in-progress log and the beam) with the machine as it stands, and sets live.
 * The frame (pixels) and the completed log are left as they were. Only for a
 * machine parked by the debugger: it asks openMSX through
 * msxhost_execute_sync, so it must not be called from openMSX's thread. */
void msxhost_capture_fill(msxvid_capture *dst);

#ifdef __cplusplus
}
#endif

#endif
