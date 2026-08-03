/*
 * rpc_test -- does msxhost_execute_sync() (the debugger's one primitive,
 * see msx_host.hh) actually work? Boots openMSX, then runs a handful of
 * real Tcl debug commands synchronously from this (non-openMSX) thread and
 * checks the results, including one issued *while the CPU is broken* --
 * the case the Reactor::run() debug-pump patch exists for for (see
 * tools/openmsx/patches/patch-staged-tree.py): without it, "debug break"
 * followed by "reg PC" would hang until msxhost_execute_sync()'s own 5s
 * timeout, because the per-frame hook alone stops firing the instant the
 * CPU halts.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msxsession.h"
#include "msx_host.hh"
#include "test_sleep.h"

int main(void)
{
    msxsession *s;
    msxsession_start_opts opts;
    uint32_t *fb;
    uint64_t serial = 0;
    char result[256];
    int rc = 0;
    int waited_ms;

    s = msxsession_new(NULL);
    if (!s) {
        fprintf(stderr, "rpc_test: msxsession_new failed\n");
        return 1;
    }
    msxsession_default_opts(s, &opts);
    opts.enable_fujinet = 0; /* not needed for this test; keep it fast */
    if (msxsession_start(s, &opts) != 0) {
        fprintf(stderr, "rpc_test: start failed: %s\n",
                msxsession_last_error(s));
        msxsession_free(s);
        return 1;
    }

    fb = malloc((size_t)MSXSESSION_FB_WIDTH * MSXSESSION_FB_HEIGHT *
               sizeof(uint32_t));
    for (waited_ms = 0; waited_ms < 10000; waited_ms += 50) {
        if (msxsession_copy_frame(s, fb, &serial)) break;
        sleep_ms(50);
    }
    if (serial == 0) {
        fprintf(stderr, "rpc_test: openMSX never produced a frame\n");
        rc = 1;
        goto done;
    }

    /* 1. A trivial command, CPU running: proves the RPC round-trips at all. */
    if (!msxhost_execute_sync("expr 2 + 2", result, sizeof(result))) {
        fprintf(stderr, "rpc_test: \"expr 2 + 2\" failed: %s\n", result);
        rc = 1;
    } else if (strcmp(result, "4") != 0) {
        fprintf(stderr, "rpc_test: \"expr 2 + 2\" returned \"%s\", "
                        "expected \"4\"\n", result);
        rc = 1;
    } else {
        printf("rpc_test: expr 2 + 2 = %s (CPU running)\n", result);
    }

    /* 2. A real register read via the reg proc (share/scripts/_cpuregs.tcl,
     * bundled in our own share tree). PC is a 16-bit register; just check
     * it parses as a plausible integer rather than asserting an exact
     * value (C-BIOS's PC at this arbitrary moment is not something to
     * pin down). */
    if (!msxhost_execute_sync("reg PC", result, sizeof(result))) {
        fprintf(stderr, "rpc_test: \"reg PC\" failed: %s\n", result);
        rc = 1;
    } else {
        char *end;
        long pc = strtol(result, &end, 10);
        if (end == result || pc < 0 || pc > 0xFFFF) {
            fprintf(stderr, "rpc_test: \"reg PC\" returned unparseable/out-"
                            "of-range \"%s\"\n", result);
            rc = 1;
        } else {
            printf("rpc_test: reg PC = %s (0x%04lX)\n", result, pc);
        }
    }

    /* 3. THE important case: break the CPU, then issue a command while it
     * is broken. If the debug-pump hook were missing, this would hang for
     * the full 5s timeout and msxhost_execute_sync() would report failure. */
    if (!msxhost_execute_sync("debug break", NULL, 0)) {
        fprintf(stderr, "rpc_test: \"debug break\" failed\n");
        rc = 1;
        goto resume;
    }
    if (!msxhost_execute_sync("debug breaked", result, sizeof(result))) {
        fprintf(stderr, "rpc_test: \"debug breaked\" failed after break: "
                        "%s\n", result);
        rc = 1;
    } else if (strcmp(result, "true") != 0 && strcmp(result, "1") != 0) {
        /* Tcl booleans print as "0"/"1" here, not "false"/"true" -- accept
         * either form rather than pin an internal Tcl formatting detail. */
        fprintf(stderr, "rpc_test: \"debug breaked\" returned \"%s\" after "
                        "\"debug break\", expected a true value\n", result);
        rc = 1;
    } else {
        printf("rpc_test: debug break confirmed (breaked=%s) -- the "
              "Reactor::run() debug-pump hook is keeping the RPC queue "
              "draining while the CPU is halted\n", result);
    }
    /* A second command while still broken, to be sure it wasn't a fluke
     * timing window right at the break. */
    if (!msxhost_execute_sync("reg PC", result, sizeof(result))) {
        fprintf(stderr, "rpc_test: \"reg PC\" failed while broken: %s\n",
                result);
        rc = 1;
    } else {
        printf("rpc_test: reg PC = %s while broken (second command, still "
              "responsive)\n", result);
    }

    /* 4. The binary-safe path (msxhost_execute_sync_binary), still while
     * broken -- and specifically a byte value > 0x7F, the case that would
     * come back corrupted through msxhost_execute_sync's %s/getString()
     * path (Tcl's bytearray-to-string "shimmering": high bytes expand to
     * multi-byte UTF-8-ish sequences). Poke 0xAB into VRAM address 0, read
     * it back through "debug read_block VRAM 0 1", and check the exact
     * byte survives. */
    if (!msxhost_execute_sync("debug write VRAM 0 0xAB", NULL, 0)) {
        fprintf(stderr, "rpc_test: \"debug write VRAM 0 0xAB\" failed\n");
        rc = 1;
    } else {
        uint8_t byte = 0;
        int len = -1;
        if (!msxhost_execute_sync_binary("debug read_block VRAM 0 1", &byte,
                                         1, &len)) {
            fprintf(stderr, "rpc_test: \"debug read_block VRAM 0 1\" "
                            "failed\n");
            rc = 1;
        } else if (len != 1) {
            fprintf(stderr, "rpc_test: \"debug read_block VRAM 0 1\" "
                            "returned length %d, expected 1\n", len);
            rc = 1;
        } else if (byte != 0xAB) {
            fprintf(stderr, "rpc_test: \"debug read_block VRAM 0 1\" "
                            "returned byte 0x%02X, expected 0xAB -- high-"
                            "byte value corrupted?\n", byte);
            rc = 1;
        } else {
            printf("rpc_test: binary read_block VRAM[0] = 0x%02X (high-byte "
                  "value survived intact)\n", byte);
        }
    }

resume:
    if (!msxhost_execute_sync("debug cont", NULL, 0)) {
        fprintf(stderr, "rpc_test: \"debug cont\" failed\n");
        rc = 1;
    }
    /* Confirm frames resume after continuing -- proof the break/continue
     * round trip left the emulator in a genuinely runnable state. */
    {
        uint64_t after = serial;
        int resumed = 0;
        for (waited_ms = 0; waited_ms < 5000; waited_ms += 50) {
            if (msxsession_copy_frame(s, fb, &after)) { resumed = 1; break; }
            sleep_ms(50);
        }
        if (!resumed) {
            fprintf(stderr, "rpc_test: no frames after \"debug cont\" -- "
                            "the emulator did not really resume\n");
            rc = 1;
        } else {
            printf("rpc_test: frames resumed after debug cont\n");
        }
    }

done:
    msxsession_stop(s);
    msxsession_free(s);
    free(fb);
    return rc;
}
