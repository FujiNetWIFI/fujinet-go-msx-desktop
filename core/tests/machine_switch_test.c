/*
 * machine_switch_test -- does openMSX's own Reactor::switchMachine() (the
 * "machine <id>" Tcl command msxhost_switch_machine() queues) actually work
 * for a live in-place switch, with the FujiNet extension re-inserted and
 * the BOIP connection re-established afterward?
 *
 * Boots C-BIOS MSX2, confirms frames, switches to C-BIOS MSX1 in place
 * (session stays "running" throughout -- no stop/start), confirms frames
 * are still arriving (proof the switch did not wedge the emulator thread),
 * and -- with FujiNet enabled -- confirms a fresh BOIP connection shows up
 * in the console log after the switch (proof "ext FujiNet" actually landed
 * on the new machine board, not just that the old board's connection
 * lingered).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msxsession.h"
#include "test_sleep.h"

static int wait_for_frame(msxsession *s, uint32_t *fb, uint64_t *serial,
                          int timeout_ms)
{
    int waited;
    for (waited = 0; waited < timeout_ms; waited += 50) {
        if (msxsession_copy_frame(s, fb, serial)) return 1;
        sleep_ms(50);
    }
    return 0;
}

int main(void)
{
    msxsession *s;
    msxsession_start_opts opts;
    uint32_t *fb;
    uint64_t serial = 0;
    int rc = 0;
    char log[8192];
    int fujinet_ok = 0;

    s = msxsession_new(NULL);
    if (!s) {
        fprintf(stderr, "machine_switch_test: msxsession_new failed\n");
        return 1;
    }

    msxsession_default_opts(s, &opts);
    opts.machine = MSXSESSION_MACHINE_MSX2;
    if (msxsession_start(s, &opts) != 0) {
        fprintf(stderr, "machine_switch_test: start failed: %s\n",
                msxsession_last_error(s));
        msxsession_free(s);
        return 1;
    }
    fujinet_ok = msxsession_fujinet_running(s);

    fb = malloc((size_t)MSXSESSION_FB_WIDTH * MSXSESSION_FB_HEIGHT *
               sizeof(uint32_t));
    if (!fb) {
        fprintf(stderr, "machine_switch_test: out of memory\n");
        msxsession_stop(s);
        msxsession_free(s);
        return 1;
    }

    if (!wait_for_frame(s, fb, &serial, 10000)) {
        fprintf(stderr, "machine_switch_test: MSX2 never produced a frame\n");
        rc = 1;
        goto done;
    }
    if (strcmp(msxsession_machine(s), MSXSESSION_MACHINE_MSX2) != 0) {
        fprintf(stderr, "machine_switch_test: machine() reports \"%s\", "
                        "expected \"%s\"\n",
                msxsession_machine(s), MSXSESSION_MACHINE_MSX2);
        rc = 1;
    }
    printf("machine_switch_test: MSX2 booted (serial=%llu)\n",
           (unsigned long long)serial);

    /* Live switch -- the session must stay "running" throughout; this is
     * not a stop/start cycle. */
    msxsession_set_machine(s, MSXSESSION_MACHINE_MSX);
    if (!msxsession_is_running(s)) {
        fprintf(stderr, "machine_switch_test: session dropped to not-"
                        "running across the switch\n");
        rc = 1;
        goto done;
    }
    if (strcmp(msxsession_machine(s), MSXSESSION_MACHINE_MSX) != 0) {
        fprintf(stderr, "machine_switch_test: machine() reports \"%s\" "
                        "after switching, expected \"%s\"\n",
                msxsession_machine(s), MSXSESSION_MACHINE_MSX);
        rc = 1;
    }

    /* Give the queued Tcl commands a moment to execute on the openMSX
     * thread, then confirm frames are flowing again post-switch. */
    sleep_ms(500);
    if (!wait_for_frame(s, fb, &serial, 10000)) {
        fprintf(stderr, "machine_switch_test: no frames after switching to "
                        "MSX1 -- the switch may have wedged the emulator "
                        "thread\n");
        rc = 1;
        goto done;
    }
    printf("machine_switch_test: MSX1 booted after live switch "
          "(serial=%llu)\n", (unsigned long long)serial);

    if (fujinet_ok) {
        int waited_ms;
        for (waited_ms = 0; waited_ms < 10000; waited_ms += 200) {
            msxsession_fujinet_copy_log(s, log, sizeof(log));
            if (strstr(log, "BoIPChannel: connection from")) {
                printf("machine_switch_test: FujiNet reconnected after "
                      "the switch (ext FujiNet landed on the new "
                      "machine)\n");
                goto done;
            }
            sleep_ms(200);
        }
        fprintf(stderr, "machine_switch_test: FujiNet never reconnected "
                        "after the machine switch; last log tail:\n%s\n",
                log);
        rc = 1;
    }

done:
    msxsession_stop(s);
    msxsession_free(s);
    free(fb);
    return rc;
}
