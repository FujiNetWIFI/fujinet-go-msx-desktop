/*
 * fujinet_smoke -- does the whole FujiNet chain actually come up? Boots
 * openMSX with FujiNet enabled and confirms the FujiNet console log shows
 * openMSX's FujiNet cartridge device (src/serial/FujiNet.cc) actually
 * connecting in over BOIP -- not just that the listener came up, which
 * msxsession_start() already blocks on internally (fujinet_wait_for_boip).
 *
 * Confirmed on hardware (well, in this test): the connection happens at
 * FujiNet-extension boot time, before C-BIOS's boot menu is even up --
 * openMSX's FujiNet device connects out as soon as the extension is
 * inserted, independent of whether MSX software has talked to it yet. The
 * 'c' keypress (opening the FujiNet CONFIG screen from C-BIOS, the human
 * path) is injected anyway as a secondary, closer-to-real-usage check, but
 * the BOIP connection itself is not gated on it.
 *
 * "BoIPChannel: connection from:" is logged by
 * lib/hardware/BoIPChannel.cpp's accept_connection() -- the same component
 * the CoCo port's DriveWire bus and this target's RS232 bus both share --
 * exactly when a client is accepted, so grepping the FujiNet console log
 * for it is a portable, non-proc-parsing way to prove the round trip
 * without a display or a human watching the CONFIG host list populate.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msxsession.h"
#include "test_sleep.h"

/* 'c' is ASCII 0x63 -- an X11 keysym and an SDL keycode in one, per
 * core/src/input_map.c's Latin-1 passthrough. */
#define KEYSYM_C 0x63u

int main(void)
{
    msxsession *s;
    msxsession_start_opts opts;
    uint32_t *fb;
    uint64_t serial = 0;
    int rc = 0;
    int waited_ms;
    int booted = 0;
    char log[8192];

    s = msxsession_new(NULL);
    if (!s) {
        fprintf(stderr, "fujinet_smoke: msxsession_new failed\n");
        return 1;
    }

    msxsession_default_opts(s, &opts);
    if (!opts.enable_fujinet) {
        fprintf(stderr, "fujinet_smoke: FujiNet not enabled by default; "
                        "nothing to test\n");
        msxsession_free(s);
        return 1;
    }

    if (msxsession_start(s, &opts) != 0) {
        fprintf(stderr, "fujinet_smoke: start failed: %s\n",
                msxsession_last_error(s));
        msxsession_free(s);
        return 1;
    }

    if (!msxsession_fujinet_running(s)) {
        /* Genuinely unavailable (a WITH_FUJINET=OFF build, or libfujinet.so
         * missing from a dev tree that hasn't run
         * tools/fujinet/build-fujinet-desktop.sh yet) is an environment
         * fact, not a bug -- skip (ctest 77), same shape as boot_smoke
         * skipping on a build with no system ROMs compiled in. */
        printf("fujinet_smoke: FujiNet runtime did not start (not built?); "
              "skipping\n");
        msxsession_stop(s);
        msxsession_free(s);
        return 77;
    }

    fb = malloc((size_t)MSXSESSION_FB_WIDTH * MSXSESSION_FB_HEIGHT *
               sizeof(uint32_t));
    if (!fb) {
        fprintf(stderr, "fujinet_smoke: out of memory\n");
        msxsession_stop(s);
        msxsession_free(s);
        return 1;
    }

    /* Wait for C-BIOS to actually be up before typing at it -- same bound
     * as boot_smoke. */
    for (waited_ms = 0; waited_ms < 10000 && !booted; waited_ms += 50) {
        if (msxsession_copy_frame(s, fb, &serial)) {
            booted = 1;
            break;
        }
        sleep_ms(50);
    }
    if (!booted) {
        fprintf(stderr, "fujinet_smoke: openMSX never produced a frame\n");
        rc = 1;
        goto done;
    }
    /* A little extra settle time: the first frames are still the boot
     * splash, and injecting a key before C-BIOS's keyboard scan is live
     * would just be dropped. */
    sleep_ms(1000);

    msxsession_key(s, 1, KEYSYM_C, KEYSYM_C, 0, 0);
    sleep_ms(50);
    msxsession_key(s, 0, KEYSYM_C, KEYSYM_C, 0, 0);

    /* Poll the FujiNet console log for proof openMSX's FujiNet device
     * actually connected in over BOIP. */
    for (waited_ms = 0; waited_ms < 15000; waited_ms += 200) {
        msxsession_fujinet_copy_log(s, log, sizeof(log));
        if (strstr(log, "BoIPChannel: connection from") ||
            strstr(log, "BoIPChannel connected")) {
            printf("fujinet_smoke: BOIP connection confirmed after %d ms\n",
                   waited_ms);
            goto done;
        }
        sleep_ms(200);
    }

    fprintf(stderr, "fujinet_smoke: no BOIP connection seen within %d ms; "
                    "last log tail:\n%s\n", waited_ms, log);
    rc = 1;

done:
    msxsession_stop(s);
    msxsession_free(s);
    free(fb);
    return rc;
}
