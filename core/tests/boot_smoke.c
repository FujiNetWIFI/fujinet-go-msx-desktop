/*
 * boot_smoke -- does the staged+patched openMSX core actually come up and
 * produce frames? This is the test that makes a macOS or Windows CI job
 * meaningful when you cannot run those platforms yourself: it exercises the
 * whole host path (msxhost_core_start, the rotateFrames() frame hook,
 * msxsession's frame store) without a window, a toolkit or FujiNet.
 *
 * Unlike the sibling targets' boot_smoke.c, there is no "run N frames"
 * loop: openMSX paces itself on its own thread and pushes frames
 * asynchronously (see msxsession.h), so this polls msxsession_copy_frame
 * for a bounded time instead.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>

#include "msxsession.h"
#include "test_sleep.h"

int main(void)
{
    msxsession *s;
    msxsession_start_opts opts;
    uint32_t *fb;
    uint64_t serial = 0;
    int frames = 0;
    unsigned long long nonblack = 0;
    int rc = 0;
    int waited_ms;

    s = msxsession_new(NULL);
    if (!s) {
        fprintf(stderr, "boot_smoke: msxsession_new failed\n");
        return 1;
    }

    msxsession_default_opts(s, &opts);
    if (msxsession_start(s, &opts) != 0) {
        fprintf(stderr, "boot_smoke: start failed: %s\n",
                msxsession_last_error(s));
        msxsession_free(s);
        return 1;
    }

    fb = malloc((size_t)MSXSESSION_FB_WIDTH * MSXSESSION_FB_HEIGHT *
               sizeof(uint32_t));
    if (!fb) {
        fprintf(stderr, "boot_smoke: out of memory\n");
        msxsession_stop(s);
        msxsession_free(s);
        return 1;
    }

    /* Poll for up to 10s of wall-clock time -- comfortably enough for
     * C-BIOS to clear the screen and print its banner (real time, since
     * openMSX is pinned to throttle=on). */
    for (waited_ms = 0; waited_ms < 10000; waited_ms += 50) {
        if (msxsession_copy_frame(s, fb, &serial)) {
            int i;
            frames++;
            for (i = 0; i < MSXSESSION_FB_WIDTH * MSXSESSION_FB_HEIGHT; i++)
                if ((fb[i] & 0x00FFFFFFu) != 0) nonblack++;
            if (frames >= 30) break; /* half a second of real frames is enough */
        }
        sleep_ms(50);
    }

    printf("frames=%d geometry=%dx%d nonblack=%llu waited_ms=%d\n",
           frames, MSXSESSION_FB_WIDTH, MSXSESSION_FB_HEIGHT, nonblack,
           waited_ms);

    if (frames == 0) {
        fprintf(stderr, "boot_smoke: no frames were produced\n");
        rc = 1;
    }
    if (nonblack == 0) {
        fprintf(stderr, "boot_smoke: every frame was blank -- the machine "
                        "did not boot\n");
        rc = 1;
    }

    msxsession_stop(s);
    msxsession_free(s);
    free(fb);
    return rc;
}
