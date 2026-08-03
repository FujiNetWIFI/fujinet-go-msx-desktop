/*
 * debugger_test -- exercises the msxdebug engine (core/debugger/debugger.c)
 * against a real, running C-BIOS boot: pause/resume, step_into actually
 * advancing PC, a persistent breakpoint firing autonomously (delivered via
 * the engine's own poll thread -- see debugger.c's header comment) while
 * free-running, disassembly of live memory, and register read/write.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msxdebug.h"
#include "msxsession.h"
#include "test_sleep.h"

static pthread_mutex_t stop_mtx = PTHREAD_MUTEX_INITIALIZER;
static int stop_count;
static msxdebug_stop_reason last_reason;
static uint16_t last_pc;

static void on_stop(void *ud, msxdebug_stop_reason reason, uint16_t pc)
{
    (void)ud;
    pthread_mutex_lock(&stop_mtx);
    stop_count++;
    last_reason = reason;
    last_pc = pc;
    pthread_mutex_unlock(&stop_mtx);
}

static int wait_for_stop_count(int want, int timeout_ms)
{
    int waited;
    for (waited = 0; waited < timeout_ms; waited += 20) {
        int n;
        pthread_mutex_lock(&stop_mtx);
        n = stop_count;
        pthread_mutex_unlock(&stop_mtx);
        if (n >= want) return 1;
        sleep_ms(20);
    }
    return 0;
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
        fprintf(stderr, "debugger_test: msxsession_new failed\n");
        return 1;
    }
    msxsession_default_opts(s, &opts);
    opts.enable_fujinet = 0;
    if (msxsession_start(s, &opts) != 0) {
        fprintf(stderr, "debugger_test: start failed: %s\n",
                msxsession_last_error(s));
        msxsession_free(s);
        return 1;
    }

    fb = malloc((size_t)MSXSESSION_FB_WIDTH * MSXSESSION_FB_HEIGHT * sizeof(uint32_t));
    for (waited_ms = 0; waited_ms < 10000; waited_ms += 50) {
        if (msxsession_copy_frame(s, fb, &serial)) break;
        sleep_ms(50);
    }
    if (serial == 0) {
        fprintf(stderr, "debugger_test: openMSX never produced a frame\n");
        rc = 1;
        goto done;
    }

    d = msxsession_debugger(s);
    if (!d) {
        fprintf(stderr, "debugger_test: msxsession_debugger returned NULL\n");
        rc = 1;
        goto done;
    }
    msxdebug_set_stop_callback(d, on_stop, NULL);

    /* 1. pause() actually halts the CPU and delivers PAUSE synchronously. */
    msxdebug_pause(d);
    if (!msxdebug_is_paused(d)) {
        fprintf(stderr, "debugger_test: not paused after msxdebug_pause\n");
        rc = 1;
    }
    if (!wait_for_stop_count(1, 500) || last_reason != MSXDBG_STOP_PAUSE) {
        fprintf(stderr, "debugger_test: pause callback not delivered as "
                        "MSXDBG_STOP_PAUSE (count=%d reason=%d)\n",
                stop_count, (int)last_reason);
        rc = 1;
    } else {
        printf("debugger_test: pause confirmed, PC=0x%04X\n", last_pc);
    }

    /* 2. step_into() actually advances PC by a real instruction. */
    {
        msxdebug_z80_regs before, after;
        uint16_t pc_before, pc_after;
        msxdebug_get_regs(d, &before);
        pc_before = (uint16_t)((before.pch << 8) | before.pcl);
        msxdebug_step_into(d);
        msxdebug_get_regs(d, &after);
        pc_after = (uint16_t)((after.pch << 8) | after.pcl);
        if (pc_after == pc_before) {
            fprintf(stderr, "debugger_test: step_into did not change PC "
                            "(stayed at 0x%04X)\n", pc_before);
            rc = 1;
        } else {
            printf("debugger_test: step_into PC 0x%04X -> 0x%04X\n",
                  pc_before, pc_after);
        }
    }

    /* 3. disassembly of live memory at PC produces a plausible instruction
     * (length 1-4, non-empty text) rather than garbage. */
    {
        msxdebug_z80_regs r;
        msxdasm_line lines[4];
        int n;
        uint16_t pc;
        msxdebug_get_regs(d, &r);
        pc = (uint16_t)((r.pch << 8) | r.pcl);
        n = msxdebug_disassemble(d, pc, 4, lines);
        if (n != 4) {
            fprintf(stderr, "debugger_test: disassemble returned %d, want 4\n", n);
            rc = 1;
        } else if (lines[0].len < 1 || lines[0].len > 4 || lines[0].text[0] == '\0') {
            fprintf(stderr, "debugger_test: implausible first line: len=%d "
                            "text=\"%s\"\n", lines[0].len, lines[0].text);
            rc = 1;
        } else {
            printf("debugger_test: disasm @0x%04X: %s (len %d)\n",
                  lines[0].addr, lines[0].text, lines[0].len);
        }
    }

    /* 4. A persistent breakpoint a few instructions ahead fires
     * autonomously while free-running, delivered via the poll thread (not
     * a call we're blocking on) -- the real point of this whole engine. */
    {
        msxdebug_z80_regs r;
        msxdasm_line lines[6];
        uint16_t pc, bp_addr;
        int n, i;
        int prev_count;

        msxdebug_get_regs(d, &r);
        pc = (uint16_t)((r.pch << 8) | r.pcl);
        n = msxdebug_disassemble(d, pc, 6, lines);
        bp_addr = pc;
        for (i = 0; i < n; i++) bp_addr = (uint16_t)(lines[i].addr + lines[i].len);

        msxdebug_bp_set(d, bp_addr);
        if (!msxdebug_bp_is_set(d, bp_addr)) {
            fprintf(stderr, "debugger_test: bp_set did not register "
                            "0x%04X as set\n", bp_addr);
            rc = 1;
        }

        pthread_mutex_lock(&stop_mtx);
        prev_count = stop_count;
        pthread_mutex_unlock(&stop_mtx);

        msxdebug_resume(d);
        if (!wait_for_stop_count(prev_count + 1, 3000)) {
            fprintf(stderr, "debugger_test: breakpoint at 0x%04X never "
                            "fired within 3s\n", bp_addr);
            rc = 1;
        } else if (last_reason != MSXDBG_STOP_BREAKPOINT) {
            fprintf(stderr, "debugger_test: autonomous stop reported reason "
                            "%d, want MSXDBG_STOP_BREAKPOINT\n",
                    (int)last_reason);
            rc = 1;
        } else if (last_pc != bp_addr) {
            fprintf(stderr, "debugger_test: breakpoint fired but PC is "
                            "0x%04X, want 0x%04X\n", last_pc, bp_addr);
            rc = 1;
        } else {
            printf("debugger_test: breakpoint at 0x%04X fired autonomously, "
                  "delivered via the poll thread\n", bp_addr);
        }
        msxdebug_bp_clear(d, bp_addr);
        if (msxdebug_bp_is_set(d, bp_addr)) {
            fprintf(stderr, "debugger_test: bp_clear did not remove 0x%04X\n",
                    bp_addr);
            rc = 1;
        }
    }

    /* 5. resume() and confirm frames flow again. */
    msxdebug_resume(d);
    {
        uint64_t after = serial;
        int resumed = 0;
        for (waited_ms = 0; waited_ms < 5000; waited_ms += 50) {
            if (msxsession_copy_frame(s, fb, &after)) { resumed = 1; break; }
            sleep_ms(50);
        }
        if (!resumed) {
            fprintf(stderr, "debugger_test: no frames after resume\n");
            rc = 1;
        } else {
            printf("debugger_test: frames resumed\n");
        }
    }

done:
    msxsession_stop(s);
    msxsession_free(s);
    free(fb);
    return rc;
}
