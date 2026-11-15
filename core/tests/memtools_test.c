/*
 * memtools_test -- the watch list, range parsing, search patterns and
 * breakpoint descriptions (core/debugger/memtools.c) on an engine with no
 * machine behind it: nothing here reads memory, so it runs everywhere.
 * (A watch's live value is checked by engine_test, which has a machine.)
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

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "memtools_test: " __VA_ARGS__); fputc('\n', stderr); failures = 1; } } while (0)

static void check_range(msxdebug *dbg, const char *text, int want_ok, unsigned want_lo, unsigned want_hi)
{
    uint16_t lo = 0, hi = 0;
    const int parsed = msxsym_parse_range(dbg, text, &lo, &hi);
    CHECK(parsed == want_ok, "range \"%s\" parsed=%d, want %d", text, parsed, want_ok);
    if (parsed && want_ok)
        CHECK(lo == want_lo && hi == want_hi, "range \"%s\" is $%04X-$%04X, want $%04X-$%04X", text, lo, hi,
              want_lo, want_hi);
}

int main(void)
{
    msxsession *session = msxsession_new(NULL);
    msxdebug *dbg;

    if (!session) {
        fprintf(stderr, "memtools_test: msxsession_new failed\n");
        return 1;
    }
    dbg = msxsession_debugger(session);
    if (!dbg) {
        fprintf(stderr, "memtools_test: no debugger\n");
        msxsession_free(session);
        return 1;
    }

    /* ranges: bare hex, $-hex, lo-hi, lo..hi, expressions and the built-in BIOS labels */
    check_range(dbg, "C000", 1, 0xC000, 0xC000);
    check_range(dbg, "$C000", 1, 0xC000, 0xC000);
    check_range(dbg, "$C000-$C00F", 1, 0xC000, 0xC00F);
    check_range(dbg, "C00F-C000", 1, 0xC000, 0xC00F);
    check_range(dbg, "4000..400F", 1, 0x4000, 0x400F);
    check_range(dbg, "$C000+$10", 1, 0xC010, 0xC010);
    check_range(dbg, "  F3B0  ", 1, 0xF3B0, 0xF3B0);
    check_range(dbg, "", 0, 0, 0);
    check_range(dbg, "nosuchlabel_zz", 0, 0, 0);
    check_range(dbg, "$C000..", 0, 0, 0);
    {
        uint16_t lo = 0, hi = 0;
        int found = 0, idx;
        int picks[8];
        const int count = msxdebug_symbols_filter(dbg, "CH", picks, 8);
        CHECK(count > 0, "the built-in BIOS table has no names with CH in them");
        for (idx = 0; idx < count; idx++) {
            uint16_t addr;
            char name[64];
            if (msxdebug_symbols_get(dbg, picks[idx], &addr, name, sizeof name) && addr < 0x4000) {
                char expr[80];
                snprintf(expr, sizeof expr, "%s+1", name);
                CHECK(msxsym_parse_range(dbg, name, &lo, &hi) && lo == addr, "label %s did not resolve", name);
                CHECK(msxsym_parse_range(dbg, expr, &lo, &hi) && lo == addr + 1, "label+1 did not resolve for %s", name);
                found = 1;
                break;
            }
        }
        CHECK(found, "no BIOS label to try");
    }

    /* watches */
    {
        char row[200];
        msxwatch_clear();
        CHECK(msxwatch_count() == 0, "clear left watches");
        CHECK(msxwatch_add("   ") == -1, "a blank watch was added");
        CHECK(msxwatch_add("C000  ") == 0 && msxwatch_add("$FC9E-$FC9F") == 1, "add failed");
        CHECK(!strcmp(msxwatch_text(0), "C000") && !strcmp(msxwatch_text(1), "$FC9E-$FC9F"), "text kept as typed (trimmed)");
        CHECK(msxwatch_add("nosuchlabel_zz") == 2, "an unresolved watch is kept");
        msxwatch_format(dbg, 2, row, sizeof row);
        CHECK(strstr(row, "?") != NULL, "an unresolved watch should show ?: %s", row);
        msxwatch_format(dbg, 0, row, sizeof row);
        CHECK(strstr(row, "$C000") != NULL && strstr(row, "  -") != NULL, "running watch row: %s", row);
        {
            uint16_t lo = 0, hi = 0;
            CHECK(msxwatch_range(dbg, 1, &lo, &hi) && lo == 0xFC9E && hi == 0xFC9F, "watch range");
            CHECK(!msxwatch_range(dbg, 2, &lo, &hi), "an unresolved watch has no range");
        }
        {
            const unsigned gen = msxwatch_generation();
            msxwatch_remove(1);
            CHECK(msxwatch_count() == 2 && msxwatch_generation() != gen, "remove");
            CHECK(!strcmp(msxwatch_text(1), "nosuchlabel_zz"), "remove closed the gap");
            msxwatch_remove(9);
            CHECK(msxwatch_count() == 2, "remove out of range changed the list");
        }
        {
            int idx, added = 0;
            for (idx = 0; idx < MSXWATCH_MAX + 4; idx++) if (msxwatch_add("C000") >= 0) added++;
            CHECK(msxwatch_count() == MSXWATCH_MAX, "the list is not capped at %d (%d)", MSXWATCH_MAX, msxwatch_count());
        }
        msxwatch_clear();
        CHECK(msxwatch_count() == 0 && msxwatch_text(0) == NULL, "clear");
    }

    /* search patterns */
    {
        uint8_t pat[MSXSEARCH_MAX], mask = 0;
        int count = 0, flags = -1;
        CHECK(msxsearch_parse(MSXSEARCH_HEX, "$C3 0x00,40 f", pat, &count, &mask, &flags) && count == 4 &&
              pat[0] == 0xC3 && pat[1] == 0x00 && pat[2] == 0x40 && pat[3] == 0x0F && mask == 0xFF && flags == 0,
              "hex pattern: %d bytes", count);
        CHECK(msxsearch_parse(MSXSEARCH_TEXT, "MSX", pat, &count, &mask, &flags) && count == 3 &&
              !memcmp(pat, "MSX", 3) && (flags & MSXDEBUG_SEARCH_NOCASE), "text pattern");
        CHECK(!msxsearch_parse(MSXSEARCH_HEX, "zz", pat, &count, &mask, &flags), "non-hex accepted");
        CHECK(!msxsearch_parse(MSXSEARCH_HEX, "  ", pat, &count, &mask, &flags), "empty hex accepted");
        CHECK(!msxsearch_parse(MSXSEARCH_TEXT, "", pat, &count, &mask, &flags), "empty text accepted");
    }

    /* breakpoint descriptions */
    {
        msxdebug_breakpoint bpt;
        char text[200];
        memset(&bpt, 0, sizeof bpt);
        bpt.type = MSXDEBUG_BP_WRITE;
        bpt.start = 0xC010;
        bpt.end = 0xC010;
        snprintf(bpt.condition, sizeof bpt.condition, "value != old");
        msxbp_describe(dbg, &bpt, text, sizeof text);
        CHECK(!strcmp(text, "Write  $C010  if value != old"), "describe: \"%s\"", text);
        bpt.type = MSXDEBUG_BP_ACCESS;
        bpt.end = 0xC01F;
        bpt.condition[0] = '\0';
        msxbp_describe(dbg, &bpt, text, sizeof text);
        CHECK(!strcmp(text, "Access  $C010-$C01F"), "describe access: \"%s\"", text);
        bpt.type = MSXDEBUG_BP_OUT;
        bpt.start = 0x98;
        bpt.end = 0x99;
        msxbp_describe(dbg, &bpt, text, sizeof text);
        CHECK(!strcmp(text, "Out  $98-$99"), "describe out: \"%s\"", text);
        bpt.type = MSXDEBUG_BP_EXEC;
        bpt.start = bpt.end = 0x0000;
        msxbp_describe(dbg, &bpt, text, sizeof text);
        CHECK(strncmp(text, "Exec  $0000", 11) == 0, "describe exec: \"%s\"", text);
    }

    /* region names */
    {
        const msxdebug_region map[] = {
            { 0x0000, 0x7FFF, 1, 0, "Main ROM (slot 0)" },
            { 0x8000, 0xFFFE, 1, 1, "Main RAM (slot 3-2)" },
            { 0xFFFF, 0xFFFF, 1, 1, "sub-slot register (slot 3)" },
        };
        char text[64];
        CHECK(msxmem_region_name(map, 3, 0x4000, text, sizeof text) > 0 && !strcmp(text, "Main ROM (slot 0)"), "region of $4000");
        CHECK(msxmem_region_name(map, 3, 0xFFFF, text, sizeof text) > 0 && strstr(text, "sub-slot"), "region of $FFFF");
        CHECK(msxmem_region_name(map, 2, 0xFFFF, text, sizeof text) == 0 && !text[0], "a gap has no name");
    }

    msxsession_free(session);
    if (!failures) printf("memtools_test: ok\n");
    return failures;
}
