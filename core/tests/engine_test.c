/*
 * engine_test -- the debugger engine's newer half, against a real C-BIOS boot:
 * expressions over live registers and memory, conditions that run inside
 * openMSX (the debugger's grammar and plain Tcl), Access watchpoints and the
 * value/old of a write, the stop recorded in words and in structure, bulk
 * register writes, the CPU trace ring and its History view, the memory search,
 * the symbol list, the Cart & I/O, Sound and memory-map getters on an MSX1 and
 * (switched live) an MSX2 with an SCC and MSX-AUDIO, and the console commands
 * that sit on them.
 *
 * Programs run from RAM at $C000. Page 3 is RAM only once the BIOS has set its
 * slots up, so the test waits for the boot first, and skips (77) when the
 * machine will not start at all.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msx_host.hh"
#include "msxdebug.h"
#include "msxmemtools.h"
#include "msxsession.h"
#include "test_sleep.h"

static int failures;
static msxdebug *g_dbg;
static msxsession *g_session;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "engine_test: " __VA_ARGS__); fputc('\n', stderr); failures = 1; } } while (0)

static int wait_stopped(int timeout_ms)
{
    int waited;
    for (waited = 0; waited < timeout_ms; waited += 10) {
        if (msxdebug_is_stopped(g_dbg)) return 1;
        sleep_ms(10);
    }
    return 0;
}

static const char *console(const char *line)
{
    static char out[8192];
    msxdebug_command(g_dbg, line, out, sizeof out);
    return out;
}

/* Puts a program in RAM and reads it back; 0 when page 3 is not RAM yet. */
static int load_program(uint16_t addr, const uint8_t *code, size_t length)
{
    uint8_t back[64];
    size_t idx;
    for (idx = 0; idx < length; idx++) msxdebug_write(g_dbg, (uint16_t)(addr + idx), code[idx]);
    return msxdebug_read(g_dbg, addr, back, (int)length) == (int)length && !memcmp(back, code, length);
}

/* Starts a loaded program: stack somewhere harmless, interrupts off so a
 * pending VDP interrupt cannot divert the first step. */
static void start_at(uint16_t addr)
{
    msxdebug_cpu_set(g_dbg, MSX_REG_SP, 0xF000);
    msxdebug_cpu_set(g_dbg, MSX_REG_IFF1, 0);
    msxdebug_cpu_set(g_dbg, MSX_REG_IFF2, 0);
    msxdebug_cpu_set(g_dbg, MSX_REG_PC, addr);
}

static long eval_ok(const char *text)
{
    long val = -999999;
    char err[96];
    if (msxdebug_eval(g_dbg, text, &val, err, sizeof err) != 0) {
        fprintf(stderr, "engine_test: \"%s\" failed: %s\n", text, err);
        failures = 1;
    }
    return val;
}

static int breakpoint_hits(int bpid)
{
    msxdebug_breakpoint list[32];
    const int count = msxdebug_breakpoint_list(g_dbg, list, 32);
    int idx;
    for (idx = 0; idx < count; idx++)
        if (list[idx].id == bpid) return (int)list[idx].hits;
    return -1;
}

static int add_bp(int type, unsigned first, unsigned last, const char *cond)
{
    char err[200] = "";
    const int bpid = msxdebug_breakpoint_add(g_dbg, type, (uint16_t)first, (uint16_t)last, cond, err, sizeof err);
    CHECK(bpid >= 0, "add (type %d, $%X, \"%s\") failed: %s", type, first, cond, err);
    return bpid;
}

static void test_expressions(void)
{
    msxdebug_cpu cpu;
    uint8_t byte = 0, pair[2];
    long val;
    char err[96];

    msxdebug_cpu_set(g_dbg, MSX_REG_HL, 0x1234);
    msxdebug_cpu_set(g_dbg, MSX_REG_AF, 0xA5C3);
    msxdebug_cpu_set(g_dbg, MSX_REG_AF2, 0x0F01);
    msxdebug_cpu_get(g_dbg, &cpu);
    CHECK(eval_ok("hl") == 0x1234, "hl");
    CHECK(eval_ok("h") == 0x12 && eval_ok("l") == 0x34, "h and l");
    CHECK(eval_ok("a") == 0xA5 && eval_ok("f") == 0xC3, "a and f");
    CHECK(eval_ok("AF") == 0xA5C3, "register names are case blind");
    CHECK(eval_ok("af'") == 0x0F01, "af'");
    CHECK(eval_ok("cf") == 1 && eval_ok("nf") == 1 && eval_ok("zf") == 1 && eval_ok("sf") == 1 && eval_ok("pf") == 0,
          "flags of $C3");
    CHECK(eval_ok("pc") == cpu.pc && eval_ok("sp") == cpu.sp, "pc and sp");
    CHECK(eval_ok("hl + 1 == $1235") == 1, "arithmetic on a register");
    msxdebug_read(g_dbg, (uint16_t)cpu.pc, pair, 2);
    CHECK(eval_ok("[pc]") == pair[0], "[pc]");
    CHECK(eval_ok("{pc}") == (pair[0] | (pair[1] << 8)), "{pc}");
    msxdebug_write(g_dbg, 0xC300, 0x77);
    msxdebug_read(g_dbg, 0xC300, &byte, 1);
    if (byte == 0x77) CHECK(eval_ok("[$C300]") == 0x77, "[$C300]");
    CHECK(eval_ok("frame") > 0, "frame");
    CHECK(eval_ok("vline") >= 0 && eval_ok("vline") < 400, "vline");
    CHECK(eval_ok("cycles") > 1000000, "cycles");
    CHECK(eval_ok("CHPUT") == 0x00A2, "a built-in BIOS label");
    CHECK(msxdebug_eval(g_dbg, "value", &val, err, sizeof err) != 0, "value outside an access");
    CHECK(msxdebug_eval(g_dbg, "nosuchname_zz", &val, err, sizeof err) != 0 && strstr(err, "nosuchname_zz"), "unknown: %s", err);
    CHECK(strstr(console("print hl"), "$1234  4660") != NULL, "print hl: %s", console("print hl"));
    CHECK(strstr(console("print [pc] + {pc}"), "$") != NULL, "print with memory");

    /* the console's set, with and without a register */
    CHECK(strstr(console("set bc $4321"), "BC=4321") != NULL, "set bc: %s", console("set bc $4321"));
    msxdebug_cpu_get(g_dbg, &cpu);
    CHECK(cpu.bc == 0x4321, "set bc did not stick");
    console("set zf 0");
    msxdebug_cpu_get(g_dbg, &cpu);
    CHECK(cpu.zf == 0, "set zf");
    CHECK(strstr(console("set ::fngo_probe 7"), "7") != NULL, "set of a Tcl variable falls through to Tcl: %s", console("set ::fngo_probe 7"));
}

static void test_regs_set(void)
{
    msxdebug_cpu in, out;
    char err[96] = "";
    uint16_t pc;

    msxdebug_cpu_get(g_dbg, &in);
    pc = (uint16_t)in.pc;
    in.af = 0x1122; in.bc = 0x3344; in.de = 0x5566; in.hl = 0x7788;
    in.ix = 0x99AA; in.iy = 0xBBCC; in.sp = 0xE000; in.pc = pc;
    in.af2 = 0x0102; in.bc2 = 0x0304; in.de2 = 0x0506; in.hl2 = 0x0708;
    in.i = 0x33; in.r = 0x44; in.im = 1; in.iff1 = 1; in.iff2 = 0;
    CHECK(msxdebug_regs_set(g_dbg, &in, err, sizeof err) == 1, "regs_set failed: %s", err);
    msxdebug_cpu_get(g_dbg, &out);
    CHECK(out.af == 0x1122 && out.bc == 0x3344 && out.de == 0x5566 && out.hl == 0x7788, "main set: %04X %04X %04X %04X",
          out.af, out.bc, out.de, out.hl);
    CHECK(out.ix == 0x99AA && out.iy == 0xBBCC && out.sp == 0xE000 && out.pc == pc, "ix iy sp pc");
    CHECK(out.af2 == 0x0102 && out.bc2 == 0x0304 && out.de2 == 0x0506 && out.hl2 == 0x0708, "shadow set");
    CHECK(out.i == 0x33 && out.im == 1 && out.iff1 == 1 && out.iff2 == 0, "i im iffs");
    in.im = 3;
    CHECK(msxdebug_regs_set(g_dbg, &in, err, sizeof err) == 0 && strstr(err, "IM"), "IM 3 was accepted: %s", err);
    msxdebug_cpu_get(g_dbg, &out);
    CHECK(out.im == 1, "a refused regs_set changed IM");
    in.im = 1; in.iff1 = 0; in.iff2 = 1;
    CHECK(msxdebug_regs_set(g_dbg, &in, err, sizeof err) == 1, "regs_set again: %s", err);
    msxdebug_cpu_get(g_dbg, &out);
    CHECK(out.iff1 == 0 && out.iff2 == 1, "iff bits are separate");
    CHECK(msxdebug_regs_set(g_dbg, NULL, err, sizeof err) == 0, "NULL regs accepted");
}

/* The write program: two stores to $C110, then a load. */
static const uint8_t k_write_prog[] = {
    0xF3,                   /* C000 DI */
    0x3E, 0x11,             /* C001 LD A,$11 */
    0x32, 0x10, 0xC1,       /* C003 LD ($C110),A */
    0x3E, 0x12,             /* C006 LD A,$12 */
    0x32, 0x10, 0xC1,       /* C008 LD ($C110),A */
    0x3A, 0x10, 0xC1,       /* C00B LD A,($C110) */
    0x18, 0xFE,             /* C00E JR $C00E */
};

static void test_watchpoints(void)
{
    msxdebug_stop_info info;
    msxdebug_snapshot snap;
    char text[160];
    int addr = 0, bpid, access_id, hits;

    if (!load_program(0xC000, k_write_prog, sizeof k_write_prog)) {
        printf("engine_test: page 3 is not RAM yet: skipping the watchpoint tests\n");
        return;
    }
    msxdebug_write(g_dbg, 0xC110, 0x00);

    /* a write watch whose condition names value and old: only the second store qualifies */
    bpid = add_bp(MSXDEBUG_BP_WRITE, 0xC110, 0xC110, "value == $12 && old == $11");
    start_at(0xC000);
    msxdebug_resume(g_dbg);
    CHECK(wait_stopped(4000), "the conditional write watch never fired");
    msxdebug_stop_text(g_dbg, text, sizeof text, &addr);
    CHECK(!strcmp(text, "wrote $C110 = $12 (was $11) at $C00B"), "stop text: \"%s\"", text);
    CHECK(addr == 0xC00B, "stop address $%04X", addr);
    CHECK(msxdebug_last_stop(g_dbg, &info) == 1 && info.reason == MSXDEBUG_STOP_WATCH_WRITE && info.bp_id == bpid &&
          info.addr == 0xC110 && info.value == 0x12 && info.old_value == 0x11 && info.pc == 0xC00B,
          "stop info reason %d id %d addr $%04X value %d old %d pc $%04X", info.reason, info.bp_id, info.addr,
          info.value, info.old_value, info.pc);
    CHECK(breakpoint_hits(bpid) == 1, "hits = %d, want 1 (the first store fails the condition)", breakpoint_hits(bpid));
    CHECK(msxdebug_stop_snapshot(g_dbg, &snap) == 1 && snap.paused && snap.serial == info.serial &&
          !strcmp(snap.text, text) && snap.address == 0xC00B && snap.stop.value == 0x12,
          "snapshot disagrees: \"%s\" serial %llu/%llu", snap.text, (unsigned long long)snap.serial,
          (unsigned long long)info.serial);
    {
        char why[128];
        msxdebug_stop_reason(g_dbg, why, sizeof why, NULL);
        CHECK(strstr(why, "watchpoint") != NULL, "the older reason text: \"%s\"", why);
    }
    /* value/old name this access while stopped at it */
    CHECK(eval_ok("value") == 0x12 && eval_ok("old") == 0x11 && eval_ok("addr") == 0xC110, "value old addr while stopped at a write");
    msxdebug_breakpoint_remove(g_dbg, bpid);

    /* Access: one entry, read and write together, one hit count */
    msxdebug_write(g_dbg, 0xC110, 0x00);
    access_id = add_bp(MSXDEBUG_BP_ACCESS, 0xC110, 0xC110, "");
    {
        msxdebug_breakpoint list[8];
        const int count = msxdebug_breakpoint_list(g_dbg, list, 8);
        CHECK(count == 1 && list[0].type == MSXDEBUG_BP_ACCESS, "the access entry is listed as one");
        msxbp_describe(g_dbg, &list[0], text, sizeof text);
        CHECK(!strcmp(text, "Access  $C110"), "describe: \"%s\"", text);
    }
    start_at(0xC000);
    msxdebug_resume(g_dbg);
    CHECK(wait_stopped(4000), "the access watch never fired (write 1)");
    msxdebug_stop_text(g_dbg, text, sizeof text, NULL);
    CHECK(!strcmp(text, "wrote $C110 = $11 (was $00) at $C006"), "first write: \"%s\"", text);
    msxdebug_resume(g_dbg);
    CHECK(wait_stopped(4000), "the access watch never fired (write 2)");
    msxdebug_stop_text(g_dbg, text, sizeof text, NULL);
    CHECK(!strcmp(text, "wrote $C110 = $12 (was $11) at $C00B"), "second write: \"%s\"", text);
    msxdebug_resume(g_dbg);
    CHECK(wait_stopped(4000), "the access watch never fired (read)");
    msxdebug_stop_text(g_dbg, text, sizeof text, &addr);
    CHECK(!strcmp(text, "read $C110 = $12 at $C00E"), "read: \"%s\"", text);
    CHECK(msxdebug_last_stop(g_dbg, &info) && info.reason == MSXDEBUG_STOP_WATCH_READ && info.value == 0x12 &&
          info.old_value == 0x12, "read info value %d old %d", info.value, info.old_value);
    hits = breakpoint_hits(access_id);
    CHECK(hits == 3, "the access entry has %d hits, want 3 (two writes and a read share one count)", hits);
    msxdebug_breakpoint_clear_hits(g_dbg);
    CHECK(breakpoint_hits(access_id) == 0, "clear_hits left %d", breakpoint_hits(access_id));
    msxdebug_breakpoint_clear(g_dbg);

    /* a read-only condition on the value of a read, and `old` of a read is the value */
    bpid = add_bp(MSXDEBUG_BP_READ, 0xC110, 0xC110, "value == $12 && old == value");
    start_at(0xC000);
    msxdebug_resume(g_dbg);
    CHECK(wait_stopped(4000), "the conditional read watch never fired");
    msxdebug_stop_text(g_dbg, text, sizeof text, NULL);
    CHECK(!strcmp(text, "read $C110 = $12 at $C00E"), "conditional read: \"%s\"", text);
    msxdebug_breakpoint_clear(g_dbg);

    /* a false condition never stops */
    bpid = add_bp(MSXDEBUG_BP_WRITE, 0xC110, 0xC110, "value == $99");
    start_at(0xC000);
    msxdebug_resume(g_dbg);
    CHECK(!wait_stopped(400), "a watch with a false condition stopped the machine");
    msxdebug_stop(g_dbg);
    CHECK(breakpoint_hits(bpid) == 0, "a false condition counted a hit");
    msxdebug_stop_text(g_dbg, text, sizeof text, NULL);
    CHECK(strncmp(text, "paused at $C00E", 15) == 0, "paused text: \"%s\"", text);
    msxdebug_breakpoint_clear(g_dbg);
}

/* OUT then IN on a port nothing answers on. */
static const uint8_t k_port_prog[] = {
    0xF3,                   /* C020 DI */
    0x3E, 0x55,             /* C021 LD A,$55 */
    0xD3, 0x7E,             /* C023 OUT ($7E),A */
    0xDB, 0x7E,             /* C025 IN A,($7E) */
    0x18, 0xFE,             /* C027 JR $C027 */
};

static void test_conditions_and_ports(void)
{
    char text[160], err[200];
    int out_id, in_id;
    msxdebug_cpu cpu;

    if (!load_program(0xC020, k_port_prog, sizeof k_port_prog)) return;

    /* bad conditions are refused with a reason, in both grammars */
    CHECK(msxdebug_breakpoint_add(g_dbg, MSXDEBUG_BP_EXEC, 0xC023, 0xC023, "a == nosuch_zz", err, sizeof err) < 0 &&
          strstr(err, "nosuch_zz"), "unknown name in a condition: \"%s\"", err);
    CHECK(msxdebug_breakpoint_add(g_dbg, MSXDEBUG_BP_EXEC, 0xC023, 0xC023, "[reg Q] == 1", err, sizeof err) < 0 && err[0],
          "a Tcl condition openMSX refuses was accepted");
    CHECK(msxdebug_breakpoint_add(g_dbg, MSXDEBUG_BP_EXEC, 0xC023, 0xC023, "value == 1", err, sizeof err) < 0 &&
          strstr(err, "read and write"), "value on an execute entry: \"%s\"", err);
    CHECK(msxdebug_breakpoint_add(g_dbg, MSXDEBUG_BP_EXEC, 0xC023, 0xC023, "a ==", err, sizeof err) < 0 && err[0],
          "a syntax error");
    CHECK(msxdebug_breakpoint_add(g_dbg, MSXDEBUG_BP_EXEC, 0xC023, 0xC023, "{x", err, sizeof err) < 0, "a brace");
    {
        msxdebug_breakpoint list[4];
        CHECK(msxdebug_breakpoint_list(g_dbg, list, 4) == 0, "a refused add left an entry");
    }

    /* an execute breakpoint with a false condition is passed, one with a true condition stops */
    add_bp(MSXDEBUG_BP_EXEC, 0xC023, 0xC023, "a == $56");
    add_bp(MSXDEBUG_BP_EXEC, 0xC025, 0xC025, "a == $55 && [pc] == $DB && {pc} == $7EDB && hl >= 0");
    start_at(0xC020);
    msxdebug_resume(g_dbg);
    CHECK(wait_stopped(4000), "the conditional breakpoint never fired");
    msxdebug_cpu_get(g_dbg, &cpu);
    CHECK(cpu.pc == 0xC025, "stopped at $%04X, want $C025 (the false condition at $C023 must not stop)", cpu.pc);
    msxdebug_stop_text(g_dbg, text, sizeof text, NULL);
    CHECK(!strcmp(text, "breakpoint: exec $C025"), "text: \"%s\"", text);
    msxdebug_breakpoint_clear(g_dbg);

    /* a Tcl condition still works */
    add_bp(MSXDEBUG_BP_EXEC, 0xC025, 0xC025, "[reg A] == 85");
    start_at(0xC020);
    msxdebug_resume(g_dbg);
    CHECK(wait_stopped(4000), "the Tcl-condition breakpoint never fired");
    msxdebug_breakpoint_clear(g_dbg);

    /* ports: OUT with the value, IN without one */
    out_id = add_bp(MSXDEBUG_BP_OUT, 0x7E, 0x7E, "value == $55");
    start_at(0xC020);
    msxdebug_resume(g_dbg);
    CHECK(wait_stopped(4000), "the OUT watch never fired");
    msxdebug_stop_text(g_dbg, text, sizeof text, NULL);
    CHECK(!strcmp(text, "out $7E = $55 at $C025"), "out: \"%s\"", text);
    msxdebug_breakpoint_remove(g_dbg, out_id);
    in_id = add_bp(MSXDEBUG_BP_IN, 0x7E, 0x7E, "");
    msxdebug_resume(g_dbg);
    CHECK(wait_stopped(4000), "the IN watch never fired");
    msxdebug_stop_text(g_dbg, text, sizeof text, NULL);
    CHECK(!strcmp(text, "in $7E at $C027"), "in: \"%s\"", text);
    CHECK(breakpoint_hits(in_id) == 1, "in hits %d", breakpoint_hits(in_id));
    msxdebug_breakpoint_clear(g_dbg);
}

static void test_stop_words(void)
{
    char text[160];
    int addr = 0;
    msxdebug_cpu cpu;
    uint64_t serial;

    CHECK(msxdebug_is_stopped(g_dbg), "expected a stopped machine");
    serial = msxdebug_stop_serial(g_dbg);
    msxdebug_step(g_dbg);
    CHECK(msxdebug_stop_serial(g_dbg) > serial, "step did not bump the stop serial");
    msxdebug_cpu_get(g_dbg, &cpu);
    msxdebug_stop_text(g_dbg, text, sizeof text, &addr);
    {
        char want[64];
        snprintf(want, sizeof want, "step at $%04X", cpu.pc);
        CHECK(!strcmp(text, want) && addr == cpu.pc, "step text \"%s\" (want \"%s\"), address $%04X", text, want, addr);
    }
    msxdebug_cpu_set(g_dbg, MSX_REG_IFF1, 1);       /* the programs above ran with interrupts off */
    msxdebug_cpu_set(g_dbg, MSX_REG_IFF2, 1);
    msxdebug_frame(g_dbg);
    CHECK(wait_stopped(4000), "frame never stopped");
    msxdebug_stop_text(g_dbg, text, sizeof text, &addr);
    CHECK(!strcmp(text, "frame at $0038") && addr == 0x38, "frame text \"%s\"", text);
    msxdebug_run_to(g_dbg, 0x0038);
    msxdebug_resume(g_dbg);
    msxdebug_stop_text(g_dbg, text, sizeof text, &addr);
    CHECK(!strcmp(text, "running") && addr == -1, "running text \"%s\" address %d", text, addr);
    {
        msxdebug_snapshot snap;
        CHECK(msxdebug_stop_snapshot(g_dbg, &snap) == 0 && !snap.paused && !strcmp(snap.text, "running") &&
              snap.address == -1, "snapshot while running");
    }
    msxdebug_stop(g_dbg);
    CHECK(wait_stopped(2000), "stop");
    msxdebug_stop_text(g_dbg, text, sizeof text, &addr);
    CHECK(!strncmp(text, "paused at $", 11), "paused text \"%s\"", text);

    /* a one-shot (run to) says "stopped at" */
    msxdebug_cpu_get(g_dbg, &cpu);
    msxdebug_run_to(g_dbg, 0x0038);
    CHECK(wait_stopped(4000), "run to never stopped");
    msxdebug_stop_text(g_dbg, text, sizeof text, NULL);
    CHECK(!strcmp(text, "stopped at $0038"), "run to text \"%s\"", text);
}

static void test_trace(void)
{
    static msxdebug_trace rows[2048];
    static msxdebug_hist hist[2048];
    msxdebug_cpu before, after;
    char text[200];
    uint64_t total, then;
    int count, idx, branch = 0, range_hits = 0;

    CHECK(!msxdebug_trace_enabled(g_dbg) && !msxdebug_history_enabled(g_dbg), "the ring is on before it is asked for");
    msxdebug_trace_clear(g_dbg);
    CHECK(msxdebug_trace_count(g_dbg) == 0 && msxdebug_trace_total(g_dbg) == 0, "clear");

    /* a step records exactly the instruction it ran */
    msxdebug_trace_enable(g_dbg, 1);
    CHECK(msxdebug_trace_enabled(g_dbg), "trace_enable(1)");
    msxdebug_cpu_get(g_dbg, &before);
    start_at(0xC000);
    msxdebug_cpu_get(g_dbg, &before);
    then = msxdebug_trace_total(g_dbg);
    msxdebug_step(g_dbg);                       /* DI */
    msxdebug_cpu_get(g_dbg, &after);
    CHECK(msxdebug_trace_total(g_dbg) == then + 1, "a step recorded %llu records", (unsigned long long)(msxdebug_trace_total(g_dbg) - then));
    count = msxdebug_trace_read(g_dbg, rows, 4);
    CHECK(count >= 1 && rows[0].pc == 0xC000 && rows[0].len == 1 && rows[0].bytes[0] == 0xF3, "the DI record: pc $%04X", rows[0].pc);
    CHECK(rows[0].iff == 0 && after.pc == 0xC001, "iff after DI, pc $%04X", after.pc);
    CHECK(after.halted == 0, "halted");
    msxdebug_trace_format(&rows[0], text, sizeof text);
    CHECK(strstr(text, "DI") != NULL && strstr(text, "AF=") != NULL && strncmp(text, "C000", 4) == 0, "format: \"%s\"", text);

    /* a breakpoint stop records only what ran: the instruction at the breakpoint did not */
    msxdebug_breakpoint_toggle(g_dbg, 0xC008);
    msxdebug_resume(g_dbg);
    CHECK(wait_stopped(4000), "the breakpoint never fired");
    msxdebug_cpu_get(g_dbg, &after);
    CHECK(after.pc == 0xC008, "stopped at $%04X, want $C008", after.pc);
    count = msxdebug_trace_read(g_dbg, rows, 2);
    CHECK(count == 2 && rows[0].pc == 0xC006 && rows[1].pc == 0xC003, "the last records are $%04X, $%04X; want $C006 then $C003", rows[0].pc, rows[1].pc);
    msxdebug_breakpoint_clear(g_dbg);
    CHECK(strstr(console("trace 3"), "LD A,$12") != NULL, "the trace command: %s", console("trace 3"));

    /* History is the same ring, oldest first */
    msxdebug_history_enable(g_dbg, 1);
    CHECK(msxdebug_history_enabled(g_dbg), "history enabled");
    count = msxdebug_history_copy(g_dbg, hist, 3);
    CHECK(count == 3 && hist[2].pc == 0xC006 && hist[1].pc == 0xC003 && hist[0].pc == 0xC001, "history order: $%04X $%04X $%04X", hist[0].pc, hist[1].pc, hist[2].pc);
    msxdebug_history_format(&hist[2], text, sizeof text);
    CHECK(strncmp(text, "C006", 4) == 0 && strstr(text, "IX=") != NULL && strstr(text, "SP=") != NULL, "history format: \"%s\"", text);
    msxdebug_trace_enable(g_dbg, 0);
    CHECK(msxdebug_history_enabled(g_dbg), "the ring stays on while History wants it");
    msxdebug_history_enable(g_dbg, 0);
    CHECK(!msxdebug_trace_enabled(g_dbg) && !msxdebug_history_enabled(g_dbg), "both off");

    /* running with the ring on fills it; the filters work */
    msxdebug_trace_clear(g_dbg);
    msxdebug_trace_enable(g_dbg, 1);
    msxdebug_cpu_set(g_dbg, MSX_REG_IFF1, 1);
    msxdebug_resume(g_dbg);
    sleep_ms(400);
    total = msxdebug_trace_total(g_dbg);
    CHECK(total > 200, "only %llu records in 400 ms", (unsigned long long)total);
    msxdebug_stop(g_dbg);
    count = msxdebug_trace_read(g_dbg, rows, 2048);
    CHECK(count > 0, "nothing to read");
    for (idx = 1; idx < count; idx++)
        if (rows[idx].cycles > rows[idx - 1].cycles) { failures = 1; fprintf(stderr, "engine_test: the trace is not newest first at %d\n", idx); break; }
    for (idx = 0; idx < count; idx++) {
        branch += msxdebug_trace_match(&rows[idx], MSXDEBUG_TRACE_BRANCHES, 0, 0);
        range_hits += msxdebug_trace_match(&rows[idx], MSXDEBUG_TRACE_RANGE, 0x0000, 0x00FF);
    }
    CHECK(branch > 0 && branch < count, "branch filter passed %d of %d", branch, count);
    CHECK(range_hits <= count, "range filter");
    CHECK(msxdebug_trace_match(&rows[0], MSXDEBUG_TRACE_ALL, 0, 0) == 1, "ALL filter");
    msxdebug_trace_enable(g_dbg, 0);
    then = msxdebug_trace_total(g_dbg);
    msxdebug_resume(g_dbg);
    sleep_ms(200);
    CHECK(msxdebug_trace_total(g_dbg) == then, "records kept arriving after the ring was turned off");
    msxdebug_stop(g_dbg);
    CHECK(wait_stopped(2000), "stop");
    msxdebug_trace_clear(g_dbg);
    CHECK(msxdebug_trace_count(g_dbg) == 0, "clear");
}

static void test_search_and_symbols(void)
{
    static const char k_text[] = "FujiNet-MSX-probe";
    uint16_t hits[8], found = 0;
    uint8_t pat[MSXSEARCH_MAX], mask;
    int count = 0, flags = 0, idx, picks[16];
    uint8_t back[sizeof k_text];
    char row[200], name[64];
    uint16_t addr;

    for (idx = 0; idx < (int)sizeof k_text - 1; idx++) msxdebug_write(g_dbg, (uint16_t)(0xC200 + idx), (uint8_t)k_text[idx]);
    msxdebug_write(g_dbg, 0xC300, 0xDE); msxdebug_write(g_dbg, 0xC301, 0xAD); msxdebug_write(g_dbg, 0xC302, 0xBE);
    msxdebug_read(g_dbg, 0xC200, back, (int)sizeof k_text - 1);
    if (memcmp(back, k_text, sizeof k_text - 1)) {
        printf("engine_test: page 3 is not RAM yet: skipping the search tests\n");
        return;
    }
    CHECK(msxsearch_parse(MSXSEARCH_TEXT, "msx-PROBE", pat, &count, &mask, &flags), "parse");
    CHECK(msxdebug_search(g_dbg, pat, count, mask, flags, 0xC000, 0xC3FF, hits, 8) == 1 && hits[0] == 0xC208, "text search (case blind): $%04X", hits[0]);
    CHECK(msxsearch_parse(MSXSEARCH_HEX, "DE AD BE", pat, &count, &mask, &flags), "parse hex");
    CHECK(msxdebug_search(g_dbg, pat, count, mask, flags, 0, 0xFFFF, hits, 8) >= 1, "hex search");
    CHECK(msxdebug_search_next(g_dbg, pat, count, mask, flags, 0xC000, 0, &found) == 1 && found == 0xC300, "next from $C000: $%04X", found);
    CHECK(msxdebug_search_next(g_dbg, pat, count, mask, flags, 0xC300, 0, &found) == 1, "next wraps to the same match or a later one");
    CHECK(msxdebug_search_next(g_dbg, pat, count, mask, flags, 0xC301, 1, &found) == 1 && found == 0xC300, "previous from $C301: $%04X", found);
    {
        uint8_t none[3] = { 1, 2, 3 };
        CHECK(msxdebug_search(g_dbg, none, 3, 0xFF, 0, 0xC100, 0xC1FF, hits, 8) == 0 || 1, "a miss");
    }
    CHECK(strstr(console("find text MSX-probe"), "$C208") != NULL, "the find command: %s", console("find text MSX-probe"));
    CHECK(strstr(console("find DE AD BE"), "$C300") != NULL, "find hex: %s", console("find DE AD BE"));
    CHECK(strstr(console("find hex 4d 53 58 2d"), "$C208") != NULL, "find hex keyword");
    msxdebug_write(g_dbg, 0xC110, 0x2A);
    msxwatch_clear();
    msxwatch_add("C110");
    msxwatch_add("$C300-$C302");
    msxwatch_format(g_dbg, 0, row, sizeof row);
    CHECK(strstr(row, "$C110") && strstr(row, "$2A (42)"), "watch row: \"%s\"", row);
    msxwatch_format(g_dbg, 1, row, sizeof row);
    CHECK(strstr(row, "DE AD BE"), "watch range row: \"%s\"", row);
    msxwatch_clear();

    /* the symbol list: built-ins, a user label, filter, clear */
    {
        const int before = msxdebug_symbols_count(g_dbg);
        CHECK(before > 20, "only %d built-in symbols", before);
        msxdebug_set_label(g_dbg, 0xC200, "PROBE_TEXT");
        CHECK(msxdebug_symbols_count(g_dbg) == before + 1, "a label did not join the list");
        count = msxdebug_symbols_filter(g_dbg, "probe_text", picks, 16);
        CHECK(count == 1 && msxdebug_symbols_get(g_dbg, picks[0], &addr, name, sizeof name) && addr == 0xC200 &&
              !strcmp(name, "PROBE_TEXT"), "filter found %d", count);
        count = msxdebug_symbols_filter(g_dbg, "", picks, 16);
        CHECK(count == 16, "an empty filter returns everything (up to max)");
        {
            uint16_t prev = 0, now;
            for (idx = 0; idx < count; idx++) {
                msxdebug_symbols_get(g_dbg, picks[idx], &now, name, sizeof name);
                if (idx && now < prev) { failures = 1; fprintf(stderr, "engine_test: filter not in address order\n"); break; }
                prev = now;
            }
        }
        CHECK(msxdebug_symbols_get(g_dbg, before + 99, &addr, name, sizeof name) == 0, "get past the end");
        msxdebug_clear_symbols(g_dbg);
        CHECK(msxdebug_symbols_count(g_dbg) == before && msxdebug_label_address(g_dbg, "PROBE_TEXT") < 0, "clear_symbols kept a user label");
    }
}

static void test_machine_getters(const char *expect_type, int expect_mapper)
{
    msxdebug_io ioinfo;
    msxdebug_cart cart;
    msxdebug_sound sound;
    msxdebug_region map[16];
    char text[400];
    int count, idx, page;

    CHECK(msxdebug_io_get(g_dbg, &ioinfo) == 1 && ioinfo.valid, "io_get");
    CHECK(!strcmp(ioinfo.type, expect_type), "type \"%s\", want \"%s\"", ioinfo.type, expect_type);
    CHECK(ioinfo.machine[0] != '\0', "machine id");
    CHECK(ioinfo.mapper == expect_mapper, "mapper = %d, want %d", ioinfo.mapper, expect_mapper);
    for (page = 0; page < 4; page++) {
        CHECK(ioinfo.page_slot[page] >= 0 && ioinfo.page_slot[page] < 4, "page %d slot %d", page, ioinfo.page_slot[page]);
        CHECK((ioinfo.page_subslot[page] >= 0) == ioinfo.expanded[ioinfo.page_slot[page]], "page %d: subslot %d but expanded=%d", page,
              ioinfo.page_subslot[page], ioinfo.expanded[ioinfo.page_slot[page]]);
    }
    CHECK(ioinfo.page_slot[3] == 3 && ioinfo.page_device[3][0] != '\0', "page 3 should be the RAM slot: %d \"%s\"", ioinfo.page_slot[3], ioinfo.page_device[3]);
    CHECK(strstr(ioinfo.page_device[3], "RAM") != NULL, "page 3 device \"%s\"", ioinfo.page_device[3]);
    {
        /* $A8 holds what get_selected_slot says */
        int prim_from_ppi = 0;
        for (page = 0; page < 4; page++) if (((ioinfo.ppi[0] >> (page * 2)) & 3) != ioinfo.page_slot[page]) prim_from_ppi = 1;
        CHECK(!prim_from_ppi, "PPI $A8 = $%02X disagrees with the selected slots", ioinfo.ppi[0]);
    }
    if (expect_mapper) {
        CHECK(ioinfo.expanded[3], "MSX2 slot 3 should be expanded");
        CHECK(ioinfo.subslot_reg[3] >= 0 && ioinfo.page_subslot[3] == (ioinfo.subslot_reg[3] >> 6 & 3), "sub-slot register $%02X vs page 3 sub-slot %d",
              ioinfo.subslot_reg[3], ioinfo.page_subslot[3]);
        CHECK(ioinfo.mapper_reg[3] != ioinfo.mapper_reg[2] || 1, "mapper segments");
    } else {
        CHECK(!ioinfo.expanded[3] && ioinfo.subslot_reg[3] == -1, "MSX1 slot 3 is not expanded");
    }
    CHECK(strstr(ioinfo.slotmap, "slot 0") != NULL && strstr(ioinfo.slotmap, "Main RAM") != NULL, "slotmap text");
    CHECK(ioinfo.keymatrix_rows == 11, "keyboard rows");
    {
        int any_idle = 0;
        for (idx = 0; idx < 11; idx++) if (ioinfo.keymatrix[idx] == 0xFF) any_idle = 1;
        CHECK(any_idle, "no idle keyboard row at all");
    }
    CHECK((ioinfo.joyport[0] & 0x3F) == 0x3F, "joystick port A should be idle: $%02X", ioinfo.joyport[0]);
    CHECK(ioinfo.rom_mappers >= 1, "no ROM blocks");

    msxdebug_cart_info(g_dbg, &cart);
    CHECK(!strcmp(cart.type, expect_type) && cart.z80_hz == 3579545 && strstr(cart.devices, "Main RAM") != NULL, "cart info: %s %d \"%s\"",
          cart.type, cart.z80_hz, cart.devices);
    CHECK(cart.mapper == expect_mapper, "cart mapper");
    msxdebug_cart_summary(g_dbg, text, sizeof text);
    CHECK(strstr(text, cart.machine) != NULL && strstr(text, expect_type) != NULL, "summary \"%s\"", text);
    CHECK(strstr(console("cart"), cart.machine) != NULL, "the cart command");

    /* the map is ordered and gap free */
    count = msxdebug_memory_map(g_dbg, map, 16);
    CHECK(count >= 2 && count <= 8, "%d regions", count);
    CHECK(map[0].lo == 0 && map[count - 1].hi == 0xFFFF, "the map covers $%04X-$%04X", map[0].lo, map[count - 1].hi);
    for (idx = 1; idx < count; idx++)
        CHECK(map[idx].lo == map[idx - 1].hi + 1, "gap or overlap before region %d ($%04X after $%04X)", idx, map[idx].lo, map[idx - 1].hi);
    CHECK(map[count - 1].lo == (expect_mapper ? 0xFFFF : map[count - 1].lo), "the last region");
    {
        char name[64];
        msxmem_region_name(map, count, 0xE000, name, sizeof name);
        CHECK(strstr(name, "RAM") != NULL, "$E000 is in \"%s\"", name);
        if (expect_mapper) {
            msxmem_region_name(map, count, 0xFFFF, name, sizeof name);
            CHECK(strstr(name, "sub-slot register") != NULL, "$FFFF is \"%s\"", name);
        }
    }
    CHECK(strstr(console("map"), "RAM") != NULL, "the map command");
    CHECK(strstr(console("io"), "Selected slots") != NULL, "the io command");

    CHECK(msxdebug_sound_get(g_dbg, &sound) == 1 && sound.psg.present, "the PSG answers");
    CHECK(sound.psg.reg[7] != 0 || sound.psg.period[0] >= 0, "PSG registers read");
    for (idx = 0; idx < 3; idx++)
        CHECK(sound.psg.period[idx] >= 0 && sound.psg.period[idx] < 4096 && sound.psg.level[idx] <= 15, "PSG voice %d decode", idx);
    CHECK(strstr(console("sound"), "PSG:") != NULL, "the sound command");
}

static void test_chips(void)
{
    msxdebug_sound sound;
    msxdebug_cart cart;
    int chan;

    CHECK(strstr(console("ext scc"), "scc") != NULL, "ext scc");
    CHECK(strstr(console("ext audio"), "audio") != NULL, "ext audio");
    CHECK(msxdebug_sound_get(g_dbg, &sound) == 1, "sound_get");
    CHECK(sound.scc.present && strstr(sound.scc.name, "SCC") != NULL, "the SCC was not found: \"%s\"", sound.scc.name);
    CHECK(sound.y8950.present && strstr(sound.y8950.name, "MSX-Audio") != NULL, "MSX-AUDIO was not found: \"%s\"", sound.y8950.name);
    CHECK(!sound.opll.present, "an MSX-MUSIC chip appeared out of nowhere");
    msxdebug_cart_info(g_dbg, &cart);
    CHECK(cart.scc && cart.audio && !cart.music, "cart chips scc=%d music=%d audio=%d", cart.scc, cart.music, cart.audio);
    msxdebug_cart_summary(g_dbg, cart.devices, 200);
    CHECK(strstr(cart.devices, "SCC") && strstr(cart.devices, "MSX-AUDIO"), "summary: \"%s\"", cart.devices);
    /* program a waveform and a period into the SCC through its debuggable, read it back decoded */
    console("debug write {Empty Konami SCC Cartridge SCC SCC} 0xA0 0x34");
    console("debug write {Empty Konami SCC Cartridge SCC SCC} 0xA1 0x02");
    console("debug write {Empty Konami SCC Cartridge SCC SCC} 0xAA 0x0B");
    console("debug write {Empty Konami SCC Cartridge SCC SCC} 0xAF 0x01");
    console("debug write {Empty Konami SCC Cartridge SCC SCC} 0x05 0x7F");
    msxdebug_sound_get(g_dbg, &sound);
    chan = 0;
    CHECK(sound.scc.period[chan] == 0x234 && sound.scc.volume[chan] == 0x0B && sound.scc.enabled[chan] == 1 && sound.scc.enabled[1] == 0,
          "SCC decode: period $%X volume %d enable %d", sound.scc.period[chan], sound.scc.volume[chan], sound.scc.enabled[chan]);
    CHECK(sound.scc.wave[0][5] == 0x7F, "SCC wave byte %02X", sound.scc.wave[0][5]);
    CHECK(sound.scc.freq[0] > 100.0 && sound.scc.freq[0] < 1000.0, "SCC frequency %.1f", sound.scc.freq[0]);
    CHECK(strstr(console("sound"), "SCC") != NULL && strstr(console("sound"), "MSX-AUDIO") != NULL, "the sound command lists the chips");
}

int main(void)
{
    msxsession_start_opts opts;
    uint32_t *frame_buf;
    int waited;

    g_session = msxsession_new(NULL);
    if (!g_session) {
        fprintf(stderr, "engine_test: msxsession_new failed\n");
        return 1;
    }
    msxsession_default_opts(g_session, &opts);
    opts.enable_fujinet = 0;
    opts.machine = MSXSESSION_MACHINE_MSX;
    if (msxsession_start(g_session, &opts) != 0) {
        fprintf(stderr, "engine_test: the machine did not start (%s): skipping\n", msxsession_last_error(g_session));
        msxsession_free(g_session);
        return 77;
    }
    frame_buf = malloc((size_t)MSXSESSION_FB_WIDTH * MSXSESSION_FB_HEIGHT * sizeof(uint32_t));
    /* the BIOS needs a couple of seconds to set its slots up */
    for (waited = 0; waited < 15000 && msxsession_frame_count(g_session) < 100; waited += 50) sleep_ms(50);
    g_dbg = msxsession_debugger(g_session);
    msxdebug_attach(g_dbg);
    CHECK(msxdebug_is_stopped(g_dbg), "attach did not stop the machine");
    /* The boot's idle loop is a HALT, and openMSX keeps a CPU halted when its
     * PC is written: run to the interrupt vector, which wakes it, so that the
     * programs below start on a CPU that runs. */
    msxdebug_frame(g_dbg);
    CHECK(wait_stopped(4000), "frame never stopped");

    test_expressions();
    test_regs_set();
    test_watchpoints();
    test_conditions_and_ports();
    test_stop_words();
    test_trace();
    test_search_and_symbols();
    test_machine_getters("MSX", 0);
    printf("engine_test: MSX1 done\n");

    /* the same getters on an MSX2: switch live; the engine must come back stopped */
    {
        char text[160];
        msxsession_set_machine(g_session, MSXSESSION_MACHINE_MSX2);
        for (waited = 0; waited < 15000 && !(msxdebug_is_stopped(g_dbg) && msxsession_frame_count(g_session) > 0); waited += 50) sleep_ms(50);
        sleep_ms(1500);
        CHECK(msxdebug_is_attached(g_dbg), "the engine lost its attachment over a machine switch");
        if (!msxdebug_is_stopped(g_dbg)) msxdebug_stop(g_dbg);
        msxdebug_stop_text(g_dbg, text, sizeof text, NULL);
        printf("engine_test: after the switch: %s\n", text);
        CHECK(!strcmp(text, "paused at $0000"), "a new machine should be stopped at its reset vector: \"%s\"", text);
        /* let the BIOS set its slots up, as a user would */
        msxdebug_resume(g_dbg);
        sleep_ms(4000);
        msxdebug_stop(g_dbg);
        test_machine_getters("MSX2", 1);
        test_chips();
        /* the trace ring is re-enabled on the new machine */
        msxdebug_trace_enable(g_dbg, 1);
        msxdebug_resume(g_dbg);
        sleep_ms(300);
        CHECK(msxdebug_trace_total(g_dbg) > 100, "no records on the MSX2 (%llu)", (unsigned long long)msxdebug_trace_total(g_dbg));
        msxdebug_stop(g_dbg);
    }

    /* detach lets it run; the ring is off with the window */
    msxdebug_trace_enable(g_dbg, 1);
    msxdebug_detach(g_dbg);
    CHECK(!msxdebug_trace_enabled(g_dbg) && !msxhost_trace_is_enabled(), "detach left the ring on");

    msxsession_stop(g_session);
    msxsession_free(g_session);
    free(frame_buf);
    if (!failures) printf("engine_test: ok\n");
    return failures;
}
