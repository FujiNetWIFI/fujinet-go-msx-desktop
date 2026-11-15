/*
 * prompt_test -- the debugger's command line (core/debugger/prompt.c) on an
 * engine with no machine behind it: the commands that only touch the engine's
 * own state (expressions, breakpoints with conditions, the watch list,
 * symbols, help, completion) and what is refused. Anything that needs the
 * machine is engine_test's.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msxdebug.h"
#include "msxmemtools.h"
#include "msxsession.h"

static int failures;
static msxdebug *g_dbg;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "prompt_test: " __VA_ARGS__); fputc('\n', stderr); failures = 1; } } while (0)

static const char *run(const char *line)
{
    static char out[8192];
    msxdebug_command(g_dbg, line, out, sizeof out);
    return out;
}

static int has(const char *line, const char *want)
{
    const char *got = run(line);
    if (!strstr(got, want)) {
        fprintf(stderr, "prompt_test: \"%s\" printed \"%s\", want \"%s\"\n", line, got, want);
        failures = 1;
        return 0;
    }
    return 1;
}

int main(void)
{
    msxsession *session = msxsession_new(NULL);
    char text[2048];
    msxdebug_breakpoint list[16];
    int count;

    if (!session) {
        fprintf(stderr, "prompt_test: msxsession_new failed\n");
        return 1;
    }
    g_dbg = msxsession_debugger(session);
    if (!g_dbg) {
        fprintf(stderr, "prompt_test: no debugger\n");
        msxsession_free(session);
        return 1;
    }

    /* print: every notation, with the binary form */
    has("print $C000 + 16", "$C010  49168  %1100000000010000");
    has("print 0x10 * 3 - 1", "$002F  47");
    has("print %1010 | 1", "$000B  11");
    has("p (1+2)*3", "$0009  9");
    has("print nosuchlabel_zz", "error: unknown name 'nosuchlabel_zz'");
    has("print 1 +", "error:");
    has("print 1/0", "division by zero");

    /* labels, user and built in */
    has("label $4010 MYSPOT", "$4010 = MYSPOT");
    has("print MYSPOT + 1", "$4011");
    has("symbols MYSPOT", "$4010  MYSPOT");
    has("symbols zzzzqqqq", "no labels match");
    CHECK(msxdebug_label_address(g_dbg, "MYSPOT") == 0x4010, "label not stored");
    has("label $4010", "usage");

    /* breakpoints: every kind, ranges, conditions in the grammar and in Tcl */
    has("break $4000", "breakpoint 1 set");
    has("bpw $C000-$C0FF value != old", "breakpoint 2 set");
    has("bpr MYSPOT", "breakpoint 3 set");
    has("bpa $C100 value == 5 && addr == $C100", "breakpoint 4 set");
    has("bpin $98", "breakpoint 5 set");
    has("bpout $98-$9B value >= $80", "breakpoint 6 set");
    has("break $4020 [reg A] == 1", "breakpoint 7 set");
    has("break $4030 a == 1 && hl > $C000", "breakpoint 8 set");
    count = msxdebug_breakpoint_list(g_dbg, list, 16);
    CHECK(count == 8, "%d breakpoints, want 8", count);
    CHECK(list[3].type == MSXDEBUG_BP_ACCESS && list[3].start == 0xC100, "the access entry");
    CHECK(list[1].type == MSXDEBUG_BP_WRITE && list[1].end == 0xC0FF, "the write range");
    CHECK(!strcmp(list[1].condition, "value != old"), "the condition is kept as typed: \"%s\"", list[1].condition);
    has("breaks", "4 access $C100 on hits 0  if value == 5 && addr == $C100");
    has("break $4040 value == 1", "error: value, addr and old belong to read and write");
    has("break $4040 nosuch == 1", "error:");
    has("break $4040 {bad", "error:");
    has("break $4040 a ==", "error:");
    has("bpin $100", "error: ports are 0-255");
    has("bpa", "usage");
    CHECK(msxdebug_breakpoint_list(g_dbg, list, 16) == 8, "a refused breakpoint was added");

    has("disable 2", "disabled 2");
    msxdebug_breakpoint_list(g_dbg, list, 16);
    CHECK(!list[1].enabled, "disable");
    has("enable #2", "enabled 2");
    has("hits clear", "hit counts cleared");
    has("delete $4000", "deleted 1");
    has("delete 3", "deleted 3");
    has("delete 99", "no such breakpoint");
    CHECK(msxdebug_breakpoint_list(g_dbg, list, 16) == 6, "delete left %d", msxdebug_breakpoint_list(g_dbg, list, 16));
    has("clear", "all breakpoints cleared");
    has("breaks", "no breakpoints");

    /* the watch list */
    msxwatch_clear();
    has("watch", "no watches");
    has("watch $FC9E", "$FC9E");
    has("watch MYSPOT", "MYSPOT");
    CHECK(msxwatch_count() == 2, "two watches expected");
    has("unwatch 0", "");
    CHECK(msxwatch_count() == 1 && !strcmp(msxwatch_text(0), "MYSPOT"), "unwatch 0 removed the wrong one");
    has("unwatch all", "");
    CHECK(msxwatch_count() == 0, "unwatch all");
    has("unwatch", "unwatch <n|all>");

    /* a machine-less engine says so rather than crashing */
    has("find 3E 00", "stop the machine first");
    has("find text MSX", "stop the machine first");
    has("find zz", "find [hex]");
    has("set a 5", "");       /* not stopped: nothing to read, nothing crashes */
    has("map", "unmapped");
    has("sound", "no sound chips");
    has("cart", "no machine running");

    /* help and completion */
    has("help", "bpa <addr>");
    has("HELP", "bpa <addr>");
    has("?", "video [section]");
    CHECK(msxdebug_completions(g_dbg, "wat", text, sizeof text) >= 1 && strstr(text, "watch") != NULL, "completion of wat");
    CHECK(msxdebug_completions(g_dbg, "bp", text, sizeof text) == 5, "bp completes five commands (bpr bpw bpa bpin bpout), got %s", text);
    CHECK(msxdebug_completions(g_dbg, "print CHPU", text, sizeof text) >= 1 && strstr(text, "CHPUT") != NULL, "label completion");
    CHECK(msxdebug_completions(g_dbg, "print h", text, sizeof text) >= 1 && strstr(text, "hl") != NULL, "register completion");
    CHECK(msxdebug_completions(g_dbg, "save d", text, sizeof text) == 1 && strstr(text, "dis") != NULL, "save kinds");
    CHECK(msxdebug_completions(g_dbg, "vid", text, sizeof text) >= 1 && strstr(text, "video") != NULL, "video completes");

    msxsession_free(session);
    if (!failures) printf("prompt_test: ok\n");
    return failures;
}
