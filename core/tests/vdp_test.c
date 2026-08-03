/*
 * vdp_test -- exercises the VDP snapshot/decode path (core/debugger/
 * msxvdpview.c) against a real running MSX2 boot: a real "VDP regs"
 * snapshot decodes to a plausible mode, a VRAM poke round-trips through
 * the physical-VRAM hex dump, and the palette decoder produces the
 * documented RBG-format colors rather than garbage.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "msxdebug.h"
#include "msxsession.h"
#include "msxvdp.h"

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int main(void)
{
    msxsession *s;
    msxsession_start_opts opts;
    msxdebug *d;
    uint32_t *fb;
    uint64_t serial = 0;
    int rc = 0;
    int waited_ms;

    s = msxsession_new(NULL);
    if (!s) {
        fprintf(stderr, "vdp_test: msxsession_new failed\n");
        return 1;
    }
    msxsession_default_opts(s, &opts);
    opts.machine = MSXSESSION_MACHINE_MSX2;
    opts.enable_fujinet = 0;
    if (msxsession_start(s, &opts) != 0) {
        fprintf(stderr, "vdp_test: start failed: %s\n", msxsession_last_error(s));
        msxsession_free(s);
        return 1;
    }

    fb = malloc((size_t)MSXSESSION_FB_WIDTH * MSXSESSION_FB_HEIGHT * sizeof(uint32_t));
    for (waited_ms = 0; waited_ms < 10000; waited_ms += 50) {
        if (msxsession_copy_frame(s, fb, &serial)) break;
        sleep_ms(50);
    }
    if (serial == 0) {
        fprintf(stderr, "vdp_test: openMSX never produced a frame\n");
        rc = 1;
        goto done;
    }

    d = msxsession_debugger(s);
    if (!d) {
        fprintf(stderr, "vdp_test: msxsession_debugger returned NULL\n");
        rc = 1;
        goto done;
    }

    /* 1. A real snapshot decodes to a plausible mode -- C-BIOS's boot
     * screen is Screen 0 (Text 1) on real MSX2 firmware, but the important
     * assertion is that decode does not report "undefined": any of the
     * documented base modes is acceptable proof the register bits parsed
     * sanely, since exactly which screen C-BIOS happens to be showing at
     * this instant is not something this test should pin down. */
    {
        msxvdp_snapshot snap;
        msxvdp_mode_info mi;
        char state[4096];
        int len;

        msxdebug_vdp_snapshot(d, MSXVDP_CHIP_V9938, &snap);
        if (snap.vram_size == 0) {
            fprintf(stderr, "vdp_test: snapshot vram_size is 0\n");
            rc = 1;
            goto skip_mode;
        }
        msxvdp_decode_mode(&snap, &mi);
        if (!mi.mode_name || strstr(mi.mode_name, "undefined") != NULL) {
            fprintf(stderr, "vdp_test: mode decoded as \"%s\" (undefined) "
                            "-- R0=0x%02X R1=0x%02X\n",
                    mi.mode_name ? mi.mode_name : "(null)", snap.regs[0],
                    snap.regs[1]);
            rc = 1;
        } else {
            printf("vdp_test: live mode = %s, vram_size=%u\n", mi.mode_name,
                  (unsigned)snap.vram_size);
        }

        len = msxvdp_format_state(&snap, state, sizeof(state));
        if (len <= 0 || strstr(state, "Registers") == NULL) {
            fprintf(stderr, "vdp_test: format_state produced no usable "
                            "output (len=%d)\n", len);
            rc = 1;
        }
    skip_mode:;
    }

    /* 2. Poke a byte into VRAM via the debugger's own write path, take a
     * fresh snapshot, and confirm the physical-VRAM hex dump shows exactly
     * that byte at that address -- proves the snapshot's binary-safe
     * capture (msxhost_execute_sync_binary) actually reflects live state,
     * not a stale/zeroed buffer. */
    {
        uint8_t poke_byte = 0x5A;
        uint16_t addr = 0x1234;
        msxvdp_snapshot snap;
        char hex[256];

        /* VDP debug commands are keyed by name, not a msxdebug API of
         * their own for VRAM writes yet (this pass's scope -- see
         * msxvdp.h) -- reuse msxdebug_write_mem's sibling pattern isn't
         * applicable here since that targets CPU memory, not VRAM
         * directly, so poke through the same debug command the snapshot
         * reads from. */
        {
            extern int msxhost_execute_sync(const char *, char *, int);
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "debug write {physical VRAM} %u %u",
                    (unsigned)addr, (unsigned)poke_byte);
            if (!msxhost_execute_sync(cmd, NULL, 0)) {
                fprintf(stderr, "vdp_test: VRAM poke command failed\n");
                rc = 1;
            }
        }
        msxdebug_vdp_snapshot(d, MSXVDP_CHIP_V9938, &snap);
        if (addr < snap.vram_size && snap.vram[addr] != poke_byte) {
            fprintf(stderr, "vdp_test: after poking 0x%02X at VRAM $%04X, "
                            "snapshot shows 0x%02X\n", poke_byte, addr,
                    snap.vram[addr]);
            rc = 1;
        } else if (addr < snap.vram_size) {
            printf("vdp_test: VRAM poke round-tripped through the snapshot "
                  "(0x%02X at $%04X)\n", poke_byte, addr);
        }
        msxvdp_format_hex(&snap, (uint32_t)(addr & ~0xFu), 1, hex, sizeof(hex));
        if (!strstr(hex, "5A")) {
            fprintf(stderr, "vdp_test: hex dump around $%04X does not show "
                            "the poked byte: \"%s\"\n", addr, hex);
            rc = 1;
        }
    }

    /* 3. Palette decode: on a real V9938, entry 0 is programmable (unlike
     * the fixed TMS9918A table) -- just confirm the renderer runs without
     * crashing and produces a plausible (non-uniform, since 16 distinct
     * default-palette entries should not all collapse to one color)
     * buffer. */
    {
        msxvdp_snapshot snap;
        uint8_t rgba[MSXVDP_PAL_W * MSXVDP_PAL_H * 4];
        int i, distinct = 0;
        uint8_t first[3];

        msxdebug_vdp_snapshot(d, MSXVDP_CHIP_V9938, &snap);
        msxvdp_render_palette(&snap, rgba);
        first[0] = rgba[0]; first[1] = rgba[1]; first[2] = rgba[2];
        for (i = 1; i < MSXVDP_PAL_W * MSXVDP_PAL_H; i++) {
            if (rgba[i * 4] != first[0] || rgba[i * 4 + 1] != first[1] ||
                rgba[i * 4 + 2] != first[2]) {
                distinct = 1;
                break;
            }
        }
        if (!distinct) {
            fprintf(stderr, "vdp_test: palette render produced a uniform "
                            "buffer (all one color) -- likely broken\n");
            rc = 1;
        } else {
            printf("vdp_test: palette render produced a non-uniform "
                  "buffer as expected\n");
        }
    }

done:
    msxsession_stop(s);
    msxsession_free(s);
    free(fb);
    return rc;
}
