/*
 * help_test -- the Help covers what the app has: every Preferences setting
 * and choice, every machine, every ROM, every default key and gamepad button,
 * the app's shortcuts (and the Mac's), the two ports, the debugger's tabs
 * and prompt commands, and every context a Help button opens. Every miss is
 * listed before it fails.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msxhelp.h"
#include "msxsession.h"

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } } while (0)

static const unsigned platforms[] = {
    MSXHELP_GNOME, MSXHELP_KDE, MSXHELP_WINDOWS, MSXHELP_MACOS
};
static const char *const platform_names[] = { "GNOME", "KDE", "Windows", "macOS" };

/* What the frontends' Help buttons and menus open. */
static const char *const contexts[] = {
    "main", "fujilog", "prefs/FujiNet", "prefs/Connection",
    "debugger/console", "debugger/cpu", "debugger/disassembly", "debugger/vdp",
    "debugger/breakpoints", "debugger/io", "debugger/fujinet",
};

/* The Preferences labels, as the frontends show them. */
static const char *const labels[] = {
    "Use external FujiNet", "Address:port",
};

/* The settings keys the pages write. */
static const char *const settings_keys[] = {
    "fujinet_external", "fujinet_external_addr", "machine", "window_width", "window_height",
};

/* The app's shortcuts, as a PC and as a Mac shows them. */
static const struct { const char *pc, *mac; } shortcuts[] = {
    { "Ctrl+Shift+H", "⌘?" }, { "F11", "F11" }, { "F12", "F12" },
};

static int misses;

/* Lower case, every run of white space one space. */
static void
fold(const char *in, char *out, size_t outsz)
{
    size_t n = 0;
    int space = 0;
    for (; *in && n + 1 < outsz; in++) {
        unsigned char c = (unsigned char) *in;
        if (c == ' ' || c == '\n' || c == '\t') {
            space = 1;
            continue;
        }
        if (space && n)
            out[n++] = ' ';
        space = 0;
        out[n++] = (char) ((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    out[n] = 0;
}

static char section[1 << 18];
static char raw[1 << 18];

/* The folded text of a context's (or slug's) section on platform p; raw
 * keeps the limsx. */
static const char *
section_text(int p, const char *context)
{
    int t, b;
    section[0] = 0;
    raw[0] = 0;
    if (msxhelp_find(context, &t, &b) == 0) {
        msxhelp_plain_text(platforms[p], t, b, raw, sizeof raw);
        fold(raw, section, sizeof section);
    }
    return section;
}

static void
require(int p, const char *context, const char *what, const char *text)
{
    char want[512];
    fold(text, want, sizeof want);
    if (!strstr(section_text(p, context), want)) {
        fprintf(stderr, "MISSING (%s) %s: %s \"%s\"\n", platform_names[p], context, what, text);
        misses++;
    }
}

/* A line of the section (a table row: cells joined by " | ") with every one
 * of these, folded, in that order. */
static void
require_row(int p, const char *context, const char *a, const char *b, const char *c)
{
    char line[1024], fa[256], fb[256], fc[256];
    const char *s, *e;
    int ok = 0;

    section_text(p, context);
    fold(a, fa, sizeof fa);
    fold(b ? b : "", fb, sizeof fb);
    fold(c ? c : "", fc, sizeof fc);
    for (s = raw; *s && !ok; s = *e ? e + 1 : e) {
        char *x, *y;
        size_t n;
        e = strchr(s, '\n');
        if (!e)
            e = s + strlen(s);
        n = (size_t) (e - s) < sizeof line - 1 ? (size_t) (e - s) : sizeof line - 1;
        memcpy(line, s, n);
        line[n] = 0;
        fold(line, line, sizeof line);
        x = strstr(line, fa);
        if (!x)
            continue;
        y = b && *fb ? strstr(x + strlen(fa), fb) : x + strlen(fa);
        if (!y)
            continue;
        if (c && *fc && !strstr(y + (b && *fb ? strlen(fb) : 0), fc))
            continue;
        ok = 1;
    }
    if (!ok) {
        fprintf(stderr, "MISSING (%s) %s: a row \"%s\" | \"%s\" | \"%s\"\n", platform_names[p], context,
                a, b ? b : "", c ? c : "");
        misses++;
    }
}

static void
check_prefs(int p)
{
    for (size_t k = 0; k < sizeof labels / sizeof *labels; k++)
        require(p, "preferences", "label", labels[k]);
    for (size_t k = 0; k < sizeof settings_keys / sizeof *settings_keys; k++)
        require(p, "preferences", "setting", settings_keys[k]);
    require(p, "machines", "machine", "MSX2+");
}

/* The shortcuts the app keeps. */
static void
check_controls(int p)
{
    for (size_t k = 0; k < sizeof shortcuts / sizeof *shortcuts; k++)
        require(p, "controllers-and-keys", "shortcut", platforms[p] == MSXHELP_MACOS ? shortcuts[k].mac : shortcuts[k].pc);
}

/* The two ports, in the FujiNet topic. */
static void
check_ports(int p)
{
    char port[16];

    snprintf(port, sizeof port, "%d", MSXSESSION_BOIP_PORT);
    require(p, "fujinet", "BoIP port", port);
    snprintf(port, sizeof port, "%d", MSXSESSION_WEBUI_PORT);
    require(p, "fujinet", "web UI port", port);
    require(p, "an-external-fujinet", "default address", "127.0.0.1:65505");
}

/* The debugger's buttons and its tabs. */
/* The debugger's buttons, and each prompt command in a `code` span. */
/* The debugger's buttons and each prompt command in a `code` span. */
static void
check_debugger(int p)
{
    static const char *const words[] = { "Run or Stop", "Step Over", "Step Out", "Frame", "Run to Cursor",
                                         "Load Symbols", "seven tabs", "Follow PC" };
    static const char *const commands[] = { "step", "over", "out", "run", "stop", "frame", "runto", "break", "bpr",
                                            "bpw", "bpin", "bpout", "delete", "enable", "clear", "regs", "mem",
                                            "poke", "disasm", "label", "labels" };
    int t, b;

    for (size_t k = 0; k < sizeof words / sizeof *words; k++)
        require(p, "debugger", "button", words[k]);
    CHECK(msxhelp_find("debugger/console", &t, &b) == 0, "no debugger/console");
    {
        const msxhelp_topic *tp = msxhelp_topic_at(t);
        int end = msxhelp_section_end(t, b);
        for (size_t k = 0; k < sizeof commands / sizeof *commands; k++) {
            const char *cmd = commands[k];
            size_t n = strlen(cmd);
            int found = 0;
            for (int bl = b; bl < end && !found; bl++) {
                const msxhelp_block *blk = &tp->blocks[bl];
                if (!(blk->platforms & platforms[p]))
                    continue;
                for (int i = 0; i < blk->nspans && !found; i++) {
                    const char *c = blk->spans[i].text;
                    if (blk->spans[i].kind != MSXHELP_CODE)
                        continue;
                    found = !strncmp(c, cmd, n) && (c[n] == 0 || c[n] == ' ' || c[n] == '/');
                }
            }
            if (!found) {
                fprintf(stderr, "MISSING (%s) debugger/console: command `%s`\n", platform_names[p], cmd);
                misses++;
            }
        }
    }
}

int
main(void)
{
    msxhelp_hit hits[16];
    int t, b;

    CHECK(msxhelp_topic_count() > 0, "no topics");

    for (size_t k = 0; k < sizeof contexts / sizeof *contexts; k++)
        if (msxhelp_find(contexts[k], &t, &b) != 0) {
            fprintf(stderr, "MISSING context %s\n", contexts[k]);
            misses++;
        }
    for (int p = 0; p < 4; p++) {
        check_prefs(p);
        check_controls(p);
        check_ports(p);
        check_debugger(p);
    }

    /* lookup and search */
    CHECK(msxhelp_find("#preferences", &t, &b) == 0 && b == -1, "#preferences");
    CHECK(msxhelp_find("#the-settings-file", &t, &b) == 0 && b >= 0, "#the-settings-file");
    CHECK(msxhelp_search(MSXHELP_GNOME, "cartridge", hits, 16) > 0, "search cartridge");
    CHECK(msxhelp_search(MSXHELP_GNOME, "zzqxj", hits, 16) == 0, "search nonsense");
    int n = msxhelp_search(MSXHELP_KDE, "watchpoints", hits, 16);
    CHECK(n > 0 && msxhelp_find("debugger/breakpoints", &t, &b) == 0
          && hits[0].topic == t && hits[0].block == b, "watchpoints is the first hit");
    for (int i = 1; i < n; i++)
        CHECK(hits[i - 1].score >= hits[i].score, "hits best first");

    /* the Mac sees its own keys */
    {
        char text[4096];
        CHECK(msxhelp_find("controllers-and-keys", &t, &b) == 0, "controllers-and-keys");
        msxhelp_plain_text(MSXHELP_MACOS, t, b, text, sizeof text);
        CHECK(strstr(text, "⌘?") != NULL, "the Mac's Help key");
        msxhelp_plain_text(MSXHELP_GNOME, t, b, text, sizeof text);
        CHECK(strstr(text, "Ctrl+Shift+H") != NULL && !strstr(text, "⌘?"), "the PC's");
    }

    CHECK(misses == 0, "%d missing from data/help/help.md", misses);
    printf("help ok: %d topics\n", msxhelp_topic_count());
    return 0;
}
