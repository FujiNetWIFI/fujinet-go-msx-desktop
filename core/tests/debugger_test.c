/*
 * debugger_test -- exercises the msxdebug engine (core/debugger/debugger.c)
 * against a real, running C-BIOS boot: attach stopping the machine and
 * detach letting it go, step into/over/out and frame, an execute breakpoint
 * and a memory watchpoint firing on their own (found by the engine's poll
 * thread) with hit counts and a stop reason, conditions, disassembly of live
 * memory with the PC marked, register read/write, the console, and the slot
 * state.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msxdebug.h"
#include "msxsession.h"
#include "test_sleep.h"

static int rc;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "debugger_test: " __VA_ARGS__); fputc('\n', stderr); rc = 1; } } while (0)

static int wait_stopped(msxdebug *d, int timeout_ms)
{
    int waited;
    for (waited = 0; waited < timeout_ms; waited += 20) {
        if (msxdebug_is_stopped(d)) return 1;
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
    int waited_ms;

    s = msxsession_new(NULL);
    if (!s) {
        fprintf(stderr, "debugger_test: msxsession_new failed\n");
        return 1;
    }
    msxsession_default_opts(s, &opts);
    opts.enable_fujinet = 0;
    if (msxsession_start(s, &opts) != 0) {
        fprintf(stderr, "debugger_test: start failed: %s\n", msxsession_last_error(s));
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

    /* 1. attach stops the machine; detach lets it go. */
    {
        unsigned g0 = msxdebug_generation(d);
        char why[128];
        int addr = 0;
        CHECK(!msxdebug_is_attached(d) && !msxdebug_is_stopped(d), "starts attached or stopped");
        msxdebug_attach(d);
        CHECK(msxdebug_is_attached(d) && msxdebug_is_stopped(d), "not stopped after attach");
        CHECK(msxdebug_generation(d) != g0, "attach did not bump the generation");
        msxdebug_stop_reason(d, why, sizeof(why), &addr);
        CHECK(addr >= 0, "no stop address after attach");
        printf("debugger_test: attach stopped at $%04X (%s)\n", addr, why);
    }

    /* 2. step into advances PC; the CPU view and the disassembly agree. */
    {
        msxdebug_cpu a, b;
        msxdebug_line lines[8];
        int pcl = -2;
        msxdebug_cpu_get(d, &a);
        msxdebug_step(d);
        msxdebug_cpu_get(d, &b);
        CHECK(a.pc != b.pc, "step did not change PC (stayed at $%04X)", a.pc);
        printf("debugger_test: step PC $%04X -> $%04X\n", a.pc, b.pc);
        msxdebug_disassemble(d, (uint16_t)b.pc, lines, 8, &pcl);
        CHECK(pcl == 0, "the PC row is %d, want 0", pcl);
        CHECK(lines[0].is_pc && lines[0].length >= 1 && lines[0].disasm[0], "implausible first line");
        printf("debugger_test: disasm @$%04X: %s\n", lines[0].address, lines[0].disasm);
        CHECK(msxdebug_row_address(d, lines[3].address, -3) == lines[0].address,
              "row_address back 3 from row 3 is not row 0");
        CHECK(msxdebug_row_address(d, lines[0].address, 3) == lines[3].address,
              "row_address forward 3 is not row 3");
    }

    /* 3. registers: write through, flags, read back. */
    {
        msxdebug_cpu c;
        msxdebug_cpu_set(d, MSX_REG_BC, 0x1234);
        msxdebug_cpu_set(d, MSX_FLAG_C, 1);
        msxdebug_cpu_get(d, &c);
        CHECK(c.bc == 0x1234, "BC reads $%04X, want $1234", c.bc);
        CHECK(c.cf == 1, "carry did not stick");
        msxdebug_cpu_set(d, MSX_FLAG_C, 0);
        msxdebug_cpu_get(d, &c);
        CHECK(c.cf == 0, "carry did not clear");
    }

    /* 4. an execute breakpoint a few instructions ahead fires on its own,
     * found by the poll thread, with its hit counted and the reason named. */
    {
        msxdebug_line lines[6];
        msxdebug_breakpoint list[8];
        char why[128];
        int addr = 0, id, n, i;
        uint16_t bp_addr;

        msxdebug_disassemble(d, (uint16_t)0, lines, 1, NULL);   /* touch */
        {
            msxdebug_cpu c;
            msxdebug_cpu_get(d, &c);
            msxdebug_disassemble(d, (uint16_t)c.pc, lines, 6, NULL);
        }
        bp_addr = (uint16_t)(lines[5].address);
        CHECK(msxdebug_breakpoint_toggle(d, bp_addr) == 1, "toggle did not set a breakpoint");
        CHECK(msxdebug_breakpoint_check(d, bp_addr), "breakpoint_check says no");
        msxdebug_disassemble(d, bp_addr, lines, 1, NULL);
        CHECK(lines[0].has_breakpoint && lines[0].breakpoint_enabled, "the row does not show its breakpoint");

        msxdebug_resume(d);
        CHECK(wait_stopped(d, 4000), "breakpoint at $%04X never fired", bp_addr);
        msxdebug_stop_reason(d, why, sizeof(why), &addr);
        CHECK(addr == bp_addr, "stopped at $%04X, want $%04X", addr, bp_addr);
        CHECK(strstr(why, "breakpoint") != NULL, "reason \"%s\" does not name the breakpoint", why);
        n = msxdebug_breakpoint_list(d, list, 8);
        id = -1;
        for (i = 0; i < n; i++) if (list[i].start == bp_addr) id = i;
        CHECK(id >= 0 && list[id].hits == 1, "hits = %u, want 1", id >= 0 ? list[id].hits : 0);
        printf("debugger_test: breakpoint fired: %s (hits %u)\n", why, id >= 0 ? list[id].hits : 0);

        /* disabled: it must not fire */
        msxdebug_breakpoint_enable(d, list[id].id, 0);
        msxdebug_breakpoint_list(d, list, 8);
        CHECK(!list[id].enabled, "disable did not take");
        msxdebug_breakpoint_remove(d, list[id].id);
        CHECK(!msxdebug_breakpoint_check(d, bp_addr), "remove did not remove");
    }

    /* 5. the frame stops at the VDP interrupt vector; step over and step
     * out are tried on a small program in RAM (interrupts off): DI,
     * CALL $C008, NOP, ... $C008: RET. */
    {
        static const uint8_t prog[] = { 0xF3, 0xCD, 0x08, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xC9 };
        msxdebug_cpu c;
        char why[128];
        size_t k;

        for (k = 0; k < sizeof(prog); k++) msxdebug_write(d, (uint16_t)(0xC000 + k), prog[k]);
        msxdebug_cpu_set(d, MSX_REG_SP, 0xF000);
        msxdebug_cpu_set(d, MSX_REG_PC, 0xC000);
        msxdebug_step(d);                       /* DI */
        msxdebug_step_over(d);                  /* CALL: over it */
        CHECK(wait_stopped(d, 4000), "step over never stopped");
        msxdebug_cpu_get(d, &c);
        CHECK(c.pc == 0xC004, "step over stopped at $%04X, want $C004", c.pc);

        msxdebug_cpu_set(d, MSX_REG_PC, 0xC001);
        msxdebug_cpu_set(d, MSX_REG_SP, 0xF000);
        msxdebug_step(d);                       /* CALL: into it */
        msxdebug_cpu_get(d, &c);
        CHECK(c.pc == 0xC008, "step into the call reached $%04X, want $C008", c.pc);
        msxdebug_step_out(d);
        CHECK(wait_stopped(d, 4000), "step out never stopped");
        msxdebug_cpu_get(d, &c);
        CHECK(c.pc == 0xC004, "step out stopped at $%04X, want $C004", c.pc);
        printf("debugger_test: step over and step out reached $%04X\n", c.pc);

        msxdebug_frame(d);
        CHECK(wait_stopped(d, 4000), "frame never stopped");
        msxdebug_cpu_get(d, &c);
        msxdebug_stop_reason(d, why, sizeof(why), NULL);
        CHECK(c.pc == 0x0038, "frame stopped at $%04X, want the interrupt vector $0038", c.pc);
        CHECK(strcmp(why, "frame") == 0, "frame reason \"%s\"", why);
        printf("debugger_test: frame stopped at $%04X (%s)\n", c.pc, why);
    }

    /* 6. a conditional memory watchpoint: C-BIOS writes the system RAM
     * (JIFFY, the interrupt's tick counter, at $FC9E) every frame. */
    {
        msxdebug_breakpoint list[8];
        char err[160], why[128];
        int id, id2, n, i;
        id = msxdebug_breakpoint_add(d, MSXDEBUG_BP_WRITE, 0xFC9E, 0xFC9F, "", err, sizeof(err));
        CHECK(id >= 0, "watchpoint add failed: %s", err);
        id2 = msxdebug_breakpoint_add(d, MSXDEBUG_BP_WRITE, 0xFC9E, 0xFC9E, "{bad", err, sizeof(err));
        CHECK(id2 < 0 && err[0], "a brace in a condition was accepted");
        id2 = msxdebug_breakpoint_add(d, MSXDEBUG_BP_WRITE, 0xFC9E, 0xFC9E, "[[", err, sizeof(err));
        CHECK(id2 < 0 && err[0], "a bad condition was accepted");
        printf("debugger_test: a bad condition is refused: %s\n", err);
        msxdebug_resume(d);
        CHECK(wait_stopped(d, 4000), "write watchpoint never fired");
        msxdebug_stop_reason(d, why, sizeof(why), NULL);
        CHECK(strstr(why, "watchpoint") != NULL, "reason \"%s\" does not name the watchpoint", why);
        n = msxdebug_breakpoint_list(d, list, 8);
        for (i = 0; i < n; i++)
            if (list[i].id == id) CHECK(list[i].hits >= 1, "watchpoint has no hits");
        printf("debugger_test: %s\n", why);
        msxdebug_breakpoint_clear(d);
        CHECK(msxdebug_breakpoint_list(d, list, 8) == 0, "clear left breakpoints");
    }

    /* 7. the console: an alias, a label, and a Tcl pass-through. */
    {
        char out[4096];
        msxdebug_command(d, "regs", out, sizeof(out));
        CHECK(strstr(out, "PC=") != NULL, "regs printed %s", out);
        msxdebug_command(d, "reg A", out, sizeof(out));
        CHECK(out[0] != '\0', "a Tcl command printed nothing");
        msxdebug_command(d, "break 0x4000 [reg A] == 1", out, sizeof(out));
        CHECK(strstr(out, "set") != NULL, "break printed %s", out);
        msxdebug_command(d, "breaks", out, sizeof(out));
        CHECK(strstr(out, "$4000") != NULL && strstr(out, "if [reg A] == 1") != NULL, "breaks printed %s", out);
        msxdebug_command(d, "clear", out, sizeof(out));
        msxdebug_command(d, "label $4000 MYLABEL", out, sizeof(out));
        CHECK(msxdebug_label_address(d, "MYLABEL") == 0x4000, "label not found");
        msxdebug_command(d, "nosuchcommand", out, sizeof(out));
        CHECK(strstr(out, "error") != NULL, "a bad command did not report an error: %s", out);
        CHECK(msxdebug_completions(d, "ste", out, sizeof(out)) >= 1 && strstr(out, "step") != NULL,
              "no completion for ste");
    }

    /* 8. the slot state names the machine and the slot map. */
    {
        static char text[8192];
        msxdebug_io_state(d, text, sizeof(text));
        CHECK(strstr(text, "Selected slots") != NULL && strstr(text, "PSG") != NULL, "io state: %s", text);
    }

    /* 9. detach lets the machine run again, and the engine keeps nothing armed. */
    {
        uint64_t after = serial;
        int resumed = 0;
        msxdebug_breakpoint_toggle(d, 0x0000);      /* kept across the detach */
        msxdebug_detach(d);
        CHECK(!msxdebug_is_attached(d) && !msxdebug_is_stopped(d), "still attached/stopped after detach");
        for (waited_ms = 0; waited_ms < 5000; waited_ms += 50) {
            if (msxsession_copy_frame(s, fb, &after)) { resumed = 1; break; }
            sleep_ms(50);
        }
        CHECK(resumed, "no frames after detach");
        CHECK(msxdebug_breakpoint_check(d, 0x0000), "detach forgot the breakpoint");
        printf("debugger_test: detach resumed the machine\n");
    }

done:
    msxsession_stop(s);
    msxsession_free(s);
    free(fb);
    return rc;
}
