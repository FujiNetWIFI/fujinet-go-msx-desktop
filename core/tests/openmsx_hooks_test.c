/*
 * openmsx_hooks_test -- the openMSX hook patches the debugger's Trace and
 * Video tabs stand on (tools/openmsx/patches/patch-staged-tree.py), through
 * the C API in core/openmsx/msx_host.hh, against a real C-BIOS boot:
 *
 *   - the CPU trace ring stays empty until enabled, then fills with
 *     plausible records (monotonic clock, sane flags), stops filling when
 *     disabled, clears, and records exactly the instructions a step runs;
 *   - a write watchpoint sees $wp_last_old, the byte the store replaced;
 *   - the VDP per-line log records a register write with the right line
 *     (two writes a delay loop apart land the expected number of lines
 *     apart) and a palette write.
 *
 * Programs run from RAM at $C000. Page 3 is RAM only once the BIOS has set
 * its slots up, so a RAM check that cannot be satisfied prints a note and
 * skips that section instead of failing.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msx_host.hh"
#include "msxsession.h"
#include "test_sleep.h"

static int rc;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "openmsx_hooks_test: " __VA_ARGS__); fputc('\n', stderr); rc = 1; } } while (0)

static int tcl(const char *command, char *out, int out_max)
{
    char scratch[256];
    int ok;
    if (!out) { out = scratch; out_max = (int)sizeof(scratch); }
    ok = msxhost_execute_sync(command, out, out_max);
    if (!ok) fprintf(stderr, "openmsx_hooks_test: \"%s\" failed: %s\n", command, out);
    return ok;
}

static int tcl_number(const char *command)
{
    char out[64];
    if (!tcl(command, out, (int)sizeof(out))) return -1;
    return (int)strtol(out, NULL, 0);
}

/* Puts a program in RAM at $C000 and reads it back. */
static int load_program(const uint8_t *code, size_t length)
{
    char command[64];
    uint8_t back[64];
    int got = 0;
    size_t n;
    for (n = 0; n < length; n++) {
        snprintf(command, sizeof(command), "debug write memory %u %u", (unsigned)(0xC000 + n), code[n]);
        tcl(command, NULL, 0);
    }
    if (msxhost_execute_sync_binary("debug read_block memory 49152 64", back, (int)sizeof(back), &got) != 1 ||
        got < (int)length) return 0;
    return memcmp(back, code, length) == 0;
}

/* SP somewhere harmless, PC at the program (each starts with DI). */
static void start_program(void)
{
    tcl("reg SP 0xF000", NULL, 0);
    tcl("reg PC 0xC000", NULL, 0);
}

static int wait_broken(int timeout_ms)
{
    char out[16];
    int waited;
    for (waited = 0; waited < timeout_ms; waited += 20) {
        if (tcl("debug breaked", out, (int)sizeof(out)) && (out[0] == '1' || out[0] == 't')) return 1;
        sleep_ms(20);
    }
    return 0;
}

/* Runs the program at $C000 from the start (or from pc) until it reaches its closing JR $ at end, and stops
 * there, so the VDP frame in progress still holds everything it did. */
static int run_to_end(unsigned pc, size_t length)
{
    char command[96], name[64];
    int stopped;
    snprintf(command, sizeof(command), "debug set_bp %u", (unsigned)(0xC000 + length - 2));
    if (!tcl(command, name, (int)sizeof(name))) return 0;
    start_program();
    if (pc != 0xC000) {
        snprintf(command, sizeof(command), "reg PC %u", pc);
        tcl(command, NULL, 0);
    }
    tcl("debug cont", NULL, 0);
    stopped = wait_broken(3000);
    snprintf(command, sizeof(command), "debug remove_bp %s", name);
    tcl(command, NULL, 0);
    return stopped;
}

static void test_trace(void)
{
    static MsxCpuTraceRec rec[2048];
    unsigned got, n, wrong_clock = 0, wrong_flags = 0;
    uint64_t total;

    CHECK(msxhost_trace_total() == 0 && msxhost_trace_count() == 0, "the trace ring is not empty before it is enabled");
    sleep_ms(200);
    CHECK(msxhost_trace_total() == 0, "records arrived with tracing off");

    CHECK(msxhost_trace_enable(1) == 1, "trace_enable(1) failed");
    CHECK(msxhost_trace_is_enabled(), "is_enabled says no");
    sleep_ms(500);
    total = msxhost_trace_total();
    CHECK(total > 50, "only %llu records in 500 ms", (unsigned long long)total);
    got = msxhost_trace_read(0, rec, 2048);
    CHECK(got == 2048 || got == msxhost_trace_count(), "read returned %u", got);
    for (n = 1; n < got; n++) {
        if (rec[n].cycle < rec[n - 1].cycle) wrong_clock++;
        if (rec[n].iff > 3 || rec[n].im > 2 || rec[n].halted > 1) wrong_flags++;
    }
    CHECK(wrong_clock == 0, "%u records with a clock that goes backwards", wrong_clock);
    CHECK(wrong_flags == 0, "%u records with impossible IFF/IM/HALT", wrong_flags);
    CHECK(rec[got - 1].cycle > 0, "the clock never ran");
    printf("openmsx_hooks_test: %llu trace records in 500 ms, newest pc $%04X op %02X %02X\n",
           (unsigned long long)total, rec[got - 1].pc, rec[got - 1].op[0], rec[got - 1].op[1]);

    /* off: the count stops */
    CHECK(msxhost_trace_enable(0) == 1, "trace_enable(0) failed");
    total = msxhost_trace_total();
    sleep_ms(200);
    CHECK(msxhost_trace_total() == total, "records kept arriving after disable");
    msxhost_trace_clear();
    CHECK(msxhost_trace_total() == 0 && msxhost_trace_count() == 0, "clear left records");

    /* a step records the instruction it ran: its address and four bytes */
    {
        int steps_seen = 0, tried, before_pc, waited;
        MsxCpuTraceRec one;
        CHECK(tcl("debug break", NULL, 0), "break failed");
        CHECK(msxhost_trace_enable(1) == 1, "trace_enable(1) failed while broken");
        msxhost_trace_clear();
        for (tried = 0; tried < 6; tried++) {
            uint64_t before = msxhost_trace_total();
            before_pc = tcl_number("reg PC");
            tcl("debug step", NULL, 0);
            /* the step runs on openMSX's thread after the command returns */
            for (waited = 0; waited < 300 && msxhost_trace_total() == before; waited += 10) sleep_ms(10);
            sleep_ms(30);
            if (msxhost_trace_total() == before + 1 && msxhost_trace_read(msxhost_trace_count() - 1, &one, 1) == 1) {
                CHECK(one.pc == before_pc, "step from $%04X recorded pc $%04X", before_pc, one.pc);
                steps_seen++;
            } else {
                CHECK(msxhost_trace_total() == before, "one step recorded %llu records",
                      (unsigned long long)(msxhost_trace_total() - before));
            }
        }
        /* a step that takes an interrupt runs no instruction: tolerate a miss or two */
        CHECK(steps_seen >= 3, "only %d of 6 steps were recorded", steps_seen);
        printf("openmsx_hooks_test: %d of 6 steps recorded at the right pc\n", steps_seen);
        msxhost_trace_enable(0);
        tcl("debug cont", NULL, 0);
    }
}

static void test_write_old(void)
{
    /* DI ; LD A,$22 ; LD ($C100),A ; JR $ */
    static const uint8_t code[] = { 0xF3, 0x3E, 0x22, 0x32, 0x00, 0xC1, 0x18, 0xFE };
    char out[64], watch_name[64];
    int attempt, old_value, new_value;

    tcl("debug break", NULL, 0);
    if (!load_program(code, sizeof(code))) {
        printf("openmsx_hooks_test: page 3 is not RAM yet: skipping the write watchpoint check\n");
        tcl("debug cont", NULL, 0);
        return;
    }
    tcl("debug write memory 49408 17", NULL, 0);        /* $C100 = $11 */
    CHECK(tcl("debug set_watchpoint write_mem 0xC100 {} {set ::fngo_old $wp_last_old; "
              "set ::fngo_new $wp_last_value; debug break}", out, sizeof(out)),
          "set_watchpoint failed");
    snprintf(watch_name, sizeof(watch_name), "%s", out);
    for (attempt = 0; attempt < 2; attempt++) {
        int expected_old = attempt == 0 ? 0x11 : 0x22;
        start_program();
        tcl("debug cont", NULL, 0);
        CHECK(wait_broken(4000), "the write watchpoint never fired (attempt %d)", attempt);
        old_value = tcl_number("set ::fngo_old");
        new_value = tcl_number("set ::fngo_new");
        CHECK(old_value == expected_old, "attempt %d: wp_last_old = $%02X, want $%02X", attempt, old_value, expected_old);
        CHECK(new_value == 0x22, "attempt %d: wp_last_value = $%02X, want $22", attempt, new_value);
        printf("openmsx_hooks_test: write watchpoint saw old $%02X new $%02X\n", old_value, new_value);
    }
    /* the variable does not leak outside a watchpoint */
    tcl("info exists wp_last_old", out, sizeof(out));
    CHECK(out[0] == '0', "wp_last_old is still set outside a watchpoint");
    {
        char remove[96];
        snprintf(remove, sizeof(remove), "debug remove_watchpoint %s", watch_name);
        tcl(remove, NULL, 0);
    }
    tcl("debug cont", NULL, 0);
}

static const MsxVdpEvent *find_event(const MsxVdpFrameLog *log, int kind, int index, int value, int from)
{
    uint32_t n;
    for (n = (uint32_t)from; n < log->event_count; n++)
        if (log->events[n].kind == kind && log->events[n].index == index && log->events[n].value == value)
            return &log->events[n];
    return NULL;
}

static void test_vdp(void)
{
    static MsxVdpFrameLog done, current;
    /* DI ; LD A,v1 ; OUT ($99),A ; LD A,$87 ; OUT ($99),A ; LD B,0 ; DJNZ $ ;
     * LD A,v2 ; OUT ($99),A ; LD A,$87 ; OUT ($99),A ; JR $   (R#7 twice) */
    uint8_t code[] = { 0xF3, 0x3E, 0, 0xD3, 0x99, 0x3E, 0x87, 0xD3, 0x99, 0x06, 0x00, 0x10, 0xFE,
                       0x3E, 0, 0xD3, 0x99, 0x3E, 0x87, 0xD3, 0x99, 0x18, 0xFE };
    int waited, attempt, checked_lines = 0, saw_register = 0;
    char out[64];

    CHECK(!msxhost_vdp_capture_is_enabled(), "capture starts enabled");
    CHECK(msxhost_vdp_capture_frame(0, &done) == 0 && !done.valid, "a frame before any capture");
    msxhost_vdp_capture_enable(1);
    CHECK(msxhost_vdp_capture_is_enabled(), "capture did not enable");
    for (waited = 0; waited < 3000 && !msxhost_vdp_capture_frame(0, &done); waited += 20) sleep_ms(20);
    CHECK(done.valid, "no completed frame after 3 s");
    CHECK(done.lines_per_frame == 262 || done.lines_per_frame == 313, "lines per frame %d", done.lines_per_frame);
    CHECK(done.pal == (done.lines_per_frame == 313), "pal flag %d vs %d lines", done.pal, done.lines_per_frame);
    CHECK(done.progress_sx == -1 && done.progress_sy == -1, "a command is in progress on an idle screen");
    printf("openmsx_hooks_test: baseline frame %u: R#0=%02X R#1=%02X R#7=%02X, %d lines, %u events\n",
           done.frame, done.regs[0], done.regs[1], done.regs[7], done.lines_per_frame, done.event_count);

    tcl("debug break", NULL, 0);
    code[2] = (uint8_t)(done.regs[7] ^ 0x05);
    code[14] = (uint8_t)(done.regs[7] ^ 0x0A);
    if (!load_program(code, sizeof(code))) {
        printf("openmsx_hooks_test: page 3 is not RAM yet: skipping the VDP register check\n");
        tcl("debug cont", NULL, 0);
        goto palette;
    }
    for (attempt = 0; attempt < 8 && !checked_lines; attempt++) {
        const MsxVdpFrameLog *logs[2] = { &done, &current };
        int which;
        tcl("debug break", NULL, 0);
        CHECK(run_to_end(0xC000, sizeof(code)), "the VDP program never reached its end");
        msxhost_vdp_capture_frame(0, &done);
        msxhost_vdp_capture_frame(1, &current);
        for (which = 0; which < 2 && !checked_lines; which++) {
            const MsxVdpEvent *first = find_event(logs[which], MSXHOST_VDP_EVENT_REG, 7, code[2], 0);
            const MsxVdpEvent *second = find_event(logs[which], MSXHOST_VDP_EVENT_REG, 7, code[14], 0);
            if (first) saw_register = 1;
            if (first && second) {
                int apart = (int)second->line - (int)first->line;
                /* DJNZ x 256 is 3323 Z80 clocks, 228 per line: about 14.6 lines (wait states add a few) */
                CHECK(first->line < logs[which]->lines_per_frame && first->column < 1368, "event position %u/%u", first->line, first->column);
                CHECK(apart >= 13 && apart <= 18, "two writes a delay loop apart are %d lines apart", apart);
                printf("openmsx_hooks_test: R#7 written on line %u col %u, then line %u (%d apart)\n",
                       first->line, first->column, second->line, apart);
                checked_lines = 1;
            }
        }
    }
    CHECK(saw_register, "the R#7 write never appeared in the VDP log");
    CHECK(checked_lines, "no frame held both R#7 writes");
    /* the same write twice is flagged as such (the value is already there) */
    tcl("debug break", NULL, 0);
    CHECK(run_to_end(0xC00D, sizeof(code)), "the second write never ran");   /* only the second write */
    msxhost_vdp_capture_frame(0, &done);
    msxhost_vdp_capture_frame(1, &current);
    CHECK(find_event(&current, MSXHOST_VDP_EVENT_REG_SAME, 7, code[14], 0) != NULL ||
          find_event(&done, MSXHOST_VDP_EVENT_REG_SAME, 7, code[14], 0) != NULL,
          "a repeated register write is not flagged REG_SAME");

palette:
    /* a palette write: R#16 = 14, then the two data bytes at port $9A */
    {
        uint8_t pal[] = { 0xF3, 0x3E, 14, 0xD3, 0x99, 0x3E, 0x90, 0xD3, 0x99,
                          0x3E, 0, 0xD3, 0x9A, 0x3E, 0, 0xD3, 0x9A, 0x18, 0xFE };
        unsigned want;
        if (!msxhost_vdp_capture_frame(0, &done)) { CHECK(0, "no baseline frame for the palette"); return; }
        want = (unsigned)((done.palette[14] ^ 0x0235) & 0x0777);
        pal[10] = (uint8_t)(want & 0x77);
        pal[14] = (uint8_t)((want >> 8) & 0x07);
        tcl("debug break", NULL, 0);
        if (!load_program(pal, sizeof(pal))) {
            printf("openmsx_hooks_test: page 3 is not RAM yet: skipping the palette check\n");
            tcl("debug cont", NULL, 0);
            return;
        }
        {
            int seen = 0, tries;
            for (tries = 0; tries < 4 && !seen; tries++) {
                if (tries) {
                    /* the palette changed last time but fell across a frame start: change it again */
                    char poke[64];
                    want ^= 0x0111u;
                    tcl("debug break", NULL, 0);
                    snprintf(poke, sizeof(poke), "debug write memory 49162 %u", want & 0x77u);
                    tcl(poke, NULL, 0);
                    snprintf(poke, sizeof(poke), "debug write memory 49166 %u", (want >> 8) & 0x07u);
                    tcl(poke, NULL, 0);
                }
                CHECK(run_to_end(0xC000, sizeof(pal)), "the palette program never reached its end");
                msxhost_vdp_capture_frame(1, &current);
                seen = find_event(&current, MSXHOST_VDP_EVENT_PALETTE, 14, (int)want, 0) != NULL;
                if (!seen && msxhost_vdp_capture_frame(0, &done))
                    seen = find_event(&done, MSXHOST_VDP_EVENT_PALETTE, 14, (int)want, 0) != NULL;
            }
            CHECK(seen, "the palette write (entry 14 = $%04X) never appeared", want);
            if (seen) printf("openmsx_hooks_test: palette entry 14 = $%04X logged\n", want);
        }
    }

    /* off: nothing new, and the API reports no frame after a re-enable until one starts */
    msxhost_vdp_capture_enable(0);
    CHECK(!msxhost_vdp_capture_is_enabled(), "capture did not disable");
    (void)out;
}

int main(void)
{
    msxsession *s;
    msxsession_start_opts opts;
    uint32_t *fb;
    uint64_t serial = 0;
    int waited_ms;

    s = msxsession_new(NULL);
    if (!s) {
        fprintf(stderr, "openmsx_hooks_test: msxsession_new failed\n");
        return 1;
    }
    msxsession_default_opts(s, &opts);
    opts.enable_fujinet = 0;
    opts.machine = MSXSESSION_MACHINE_MSX2;   /* the palette needs a V9938, whatever the user's settings say */
    if (msxsession_start(s, &opts) != 0) {
        fprintf(stderr, "openmsx_hooks_test: start failed: %s\n", msxsession_last_error(s));
        msxsession_free(s);
        return 1;
    }
    fb = malloc((size_t)MSXSESSION_FB_WIDTH * MSXSESSION_FB_HEIGHT * sizeof(uint32_t));
    for (waited_ms = 0; waited_ms < 10000; waited_ms += 50) {
        if (msxsession_copy_frame(s, fb, &serial)) break;
        sleep_ms(50);
    }
    if (serial == 0) {
        fprintf(stderr, "openmsx_hooks_test: openMSX never produced a frame\n");
        rc = 1;
    } else {
        /* let the BIOS get past its slot set-up (RAM in page 3) */
        for (waited_ms = 0; waited_ms < 3000; waited_ms += 50) sleep_ms(50);
        test_trace();
        test_write_old();
        test_vdp();
    }
    msxsession_stop(s);
    msxsession_free(s);
    free(fb);
    return rc;
}
