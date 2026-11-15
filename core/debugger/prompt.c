/*
 * prompt.c -- the debugger's command line (msxdebug_command) and its Tab
 * completion. Every command is a thin layer over the public API, so a
 * window's buttons and the prompt can never disagree. Anything that is not a
 * debugger command goes to openMSX's own Tcl console.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "debugger_internal.h"
#include "msxmemtools.h"
#include "msxvidtab.h"

typedef struct {
    char *dst;
    int cap, len;
} outbuf;

static void emit(outbuf *buf, const char *fmt, ...)
{
    va_list args;
    int wrote;
    if (buf->len >= buf->cap - 1) return;
    va_start(args, fmt);
    wrote = vsnprintf(buf->dst + buf->len, (size_t)(buf->cap - buf->len), fmt, args);
    va_end(args);
    if (wrote > 0)
        buf->len += wrote < buf->cap - buf->len ? wrote : buf->cap - buf->len - 1;
}

static const char *const k_commands[] = {
    "help", "step", "over", "out", "run", "stop", "frame", "runto", "break", "bpr", "bpw", "bpa",
    "bpin", "bpout", "delete", "enable", "disable", "clear", "hits", "breaks", "print", "regs",
    "set", "mem", "poke", "disasm", "trace", "watch", "unwatch", "find", "symbols", "label",
    "labels", "map", "io", "cart", "sound", "video", "save", NULL,
};

static const char k_help[] =
    "step [n]            one instruction (F7)\n"
    "over / out          step over a CALL (F8) / out to the RET (Shift+F8)\n"
    "run / stop          let it go (F5) / stop it\n"
    "frame               run to the next VDP interrupt\n"
    "runto <addr>        run until PC == addr\n"
    "break <addr> [cond] execute breakpoint\n"
    "bpr | bpw | bpa <addr>[-<end>] [cond]   read / write / read-or-write watchpoint\n"
    "bpin | bpout <port>[-<end>] [cond]      I/O port watchpoint\n"
    "delete <id|addr>, enable <id>, disable <id>, clear, breaks, hits [clear]\n"
    "print <expr>        e.g. print hl, print [$c000], print {sp}\n"
    "regs, set <reg> <expr>   the CPU registers; e.g. set pc $0038, set zf 1\n"
    "mem <addr> [len]    hex dump;  poke <addr> <val...>\n"
    "disasm [addr] [n]   disassemble\n"
    "trace [on|off|clear|n]   record / show the last n instructions\n"
    "watch [expr|lo-hi], unwatch <n|all>   the watch list\n"
    "find [hex] <bytes> | find text <string>   search memory (stopped)\n"
    "symbols [text]      the labels, filtered;  label <addr> <name>, labels [file]\n"
    "map, io, cart, sound   the memory map, slots/PPI/mapper, cartridge, sound chips\n"
    "video [section]     the Video tab as text\n"
    "save <dis|mem|vram|regs> <file>\n"
    "Expressions: $C000 0xC000 C000h 49152 %1010; a..l af bc de hl ix iy sp pc i r\n"
    "af' ...; flags cf zf sf hf pf nf; [addr] byte, {addr} word; labels; frame cycles\n"
    "vline; in a condition value, old and addr; C operators. A condition that is not\n"
    "that is an openMSX Tcl expression, e.g. [reg A] == 5. Anything else is passed\n"
    "to openMSX's Tcl console (debug list, reg A 5, slotmap, ...); Tab completes.\n";

static char *next_tok(char **cursor)
{
    char *str = *cursor, *tok;
    while (*str && isspace((unsigned char)*str)) str++;
    if (!*str) {
        *cursor = str;
        return NULL;
    }
    tok = str;
    while (*str && !isspace((unsigned char)*str)) str++;
    if (*str) *str++ = '\0';
    *cursor = str;
    return tok;
}

static char *rest_of(char *cursor)
{
    while (*cursor && isspace((unsigned char)*cursor)) cursor++;
    return cursor;
}

static int eval_text(msxdebug *dbg, outbuf *buf, const char *text, long *val)
{
    char err[96];
    if (msxdebug_eval(dbg, text, val, err, sizeof err) != 0) {
        emit(buf, "error: %s\n", err);
        return -1;
    }
    return 0;
}

/* An address argument: a bare token of 1-4 hex digits is hex unless it names a
 * label (as in the memory fields); anything else is an expression. */
static int eval_addr(msxdebug *dbg, outbuf *buf, const char *text, long *val)
{
    size_t len = strlen(text), idx;
    for (idx = 0; idx < len && isxdigit((unsigned char)text[idx]); idx++) {}
    if (len >= 1 && len <= 4 && idx == len && msxdebug_label_address(dbg, text) < 0) {
        *val = strtol(text, NULL, 16);
        return 0;
    }
    return eval_text(dbg, buf, text, val);
}

/* "<addr>" or "<addr>-<end>" */
static int range_of(msxdebug *dbg, outbuf *buf, char *tok, long *first, long *last)
{
    char *dash = strchr(tok + 1, '-');
    if (dash) {
        *dash = '\0';
        if (eval_addr(dbg, buf, tok, first) || eval_addr(dbg, buf, dash + 1, last)) return -1;
        return 0;
    }
    if (eval_addr(dbg, buf, tok, first)) return -1;
    *last = *first;
    return 0;
}

static void show_regs(msxdebug *dbg, outbuf *buf)
{
    msxdebug_cpu cpu;
    msxdebug_cpu_get(dbg, &cpu);
    emit(buf,
         "AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X SP=%04X PC=%04X\n"
         "AF'=%04X BC'=%04X DE'=%04X HL'=%04X  I=%02X R=%02X IM=%d IFF1=%d IFF2=%d%s\n"
         "flags: %c%c%c%c%c%c\n",
         cpu.af, cpu.bc, cpu.de, cpu.hl, cpu.ix, cpu.iy, cpu.sp, cpu.pc, cpu.af2, cpu.bc2, cpu.de2,
         cpu.hl2, cpu.i, cpu.r, cpu.im, cpu.iff1, cpu.iff2, cpu.halted ? " HALT" : "",
         cpu.sf ? 'S' : '-', cpu.zf ? 'Z' : '-', cpu.hf ? 'H' : '-', cpu.pf ? 'P' : '-',
         cpu.nf ? 'N' : '-', cpu.cf ? 'C' : '-');
}

static void dump(outbuf *buf, uint16_t addr, const uint8_t *bytes, int count)
{
    int row, col;
    for (row = 0; row < count; row += 16) {
        emit(buf, "%04X: ", (unsigned)((addr + row) & 0xFFFF));
        for (col = 0; col < 16; col++) {
            if (row + col < count) emit(buf, "%02X ", bytes[row + col]);
            else emit(buf, "   ");
        }
        emit(buf, " ");
        for (col = 0; col < 16 && row + col < count; col++)
            emit(buf, "%c", bytes[row + col] >= 32 && bytes[row + col] < 127 ? bytes[row + col] : '.');
        emit(buf, "\n");
    }
}

static int reg_by_name(const char *name)
{
    static const struct { const char *name; int reg; } k_regs[] = {
        { "pc", MSX_REG_PC }, { "sp", MSX_REG_SP }, { "af", MSX_REG_AF }, { "bc", MSX_REG_BC },
        { "de", MSX_REG_DE }, { "hl", MSX_REG_HL }, { "ix", MSX_REG_IX }, { "iy", MSX_REG_IY },
        { "af'", MSX_REG_AF2 }, { "bc'", MSX_REG_BC2 }, { "de'", MSX_REG_DE2 }, { "hl'", MSX_REG_HL2 },
        { "i", MSX_REG_I }, { "r", MSX_REG_R }, { "im", MSX_REG_IM }, { "iff1", MSX_REG_IFF1 },
        { "iff2", MSX_REG_IFF2 }, { "a", MSX_REG_A }, { "f", MSX_REG_F }, { "b", MSX_REG_B },
        { "c", MSX_REG_C }, { "d", MSX_REG_D }, { "e", MSX_REG_E }, { "h", MSX_REG_H }, { "l", MSX_REG_L },
        { "sf", MSX_FLAG_S }, { "zf", MSX_FLAG_Z }, { "hf", MSX_FLAG_H }, { "pf", MSX_FLAG_PV },
        { "nf", MSX_FLAG_N }, { "cf", MSX_FLAG_C },
    };
    size_t idx;
    for (idx = 0; idx < sizeof(k_regs) / sizeof(k_regs[0]); idx++)
        if (!strcasecmp(k_regs[idx].name, name)) return k_regs[idx].reg;
    return -1;
}

static const char *type_name(int type)
{
    if ((type & MSXDEBUG_BP_ACCESS) == MSXDEBUG_BP_ACCESS) return "access";
    switch (type) {
    case MSXDEBUG_BP_EXEC:  return "exec";
    case MSXDEBUG_BP_READ:  return "read";
    case MSXDEBUG_BP_WRITE: return "write";
    case MSXDEBUG_BP_IN:    return "in";
    default:                return "out";
    }
}

static void list_breaks(msxdebug *dbg, outbuf *buf)
{
    msxdebug_breakpoint list[128];
    const int count = msxdebug_breakpoint_list(dbg, list, 128);
    int idx;
    if (!count) emit(buf, "no breakpoints\n");
    for (idx = 0; idx < count; idx++) {
        emit(buf, "%d %s $%04X", list[idx].id, type_name(list[idx].type), list[idx].start);
        if (list[idx].end > list[idx].start) emit(buf, "-$%04X", list[idx].end);
        emit(buf, " %s hits %u%s%s\n", list[idx].enabled ? "on" : "off", list[idx].hits,
             list[idx].condition[0] ? "  if " : "", list[idx].condition);
    }
}

static void add_bp(msxdebug *dbg, outbuf *buf, int type, char *args)
{
    char *tok = next_tok(&args);
    char err[160];
    long first, last;
    int bpid;
    const int is_port = type == MSXDEBUG_BP_IN || type == MSXDEBUG_BP_OUT;

    if (!tok) {
        emit(buf, "usage: <addr>[-<end>] [condition]\n");
        return;
    }
    if (range_of(dbg, buf, tok, &first, &last)) return;
    if (is_port && (first < 0 || last > 255 || first > 255)) {
        emit(buf, "error: ports are 0-255\n");
        return;
    }
    bpid = msxdebug_breakpoint_add(dbg, type, (uint16_t)first, (uint16_t)last, rest_of(args), err, sizeof err);
    if (bpid < 0) emit(buf, "error: %s\n", err);
    else        emit(buf, "breakpoint %d set\n", bpid);
}

static void where_text(msxdebug *dbg, outbuf *buf)
{
    msxdebug_line line;
    msxdebug_cpu cpu;
    msxdebug_cpu_get(dbg, &cpu);
    msxdebug_disassemble(dbg, (uint16_t)cpu.pc, &line, 1, NULL);
    emit(buf, "$%04X  %-12s %s%s%s\n", (unsigned)cpu.pc, line.bytes, line.disasm,
         line.label[0] ? "   ; " : "", line.label);
}

/* The next word of the rest of the line, without consuming it ("" if none). */
static const char *peek_word(const char *cursor, char *word, size_t cap)
{
    size_t len = 0;
    while (*cursor && isspace((unsigned char)*cursor)) cursor++;
    while (*cursor && !isspace((unsigned char)*cursor) && len + 1 < cap) word[len++] = *cursor++;
    word[len] = '\0';
    return word;
}

/* Does the word after `trace` make it a debugger command, or is it Tcl's own
 * trace? */
static int is_trace_arg(const char *tok)
{
    return !tok[0] || !strcmp(tok, "on") || !strcmp(tok, "off") || !strcmp(tok, "clear") ||
           isdigit((unsigned char)tok[0]) || tok[0] == '$';
}

static void show_sound(msxdebug *dbg, outbuf *buf)
{
    msxdebug_sound sound;
    int idx;

    if (!msxdebug_sound_get(dbg, &sound)) {
        emit(buf, "no sound chips answer (is the machine running?)\n");
        return;
    }
    if (sound.psg.present) {
        const msxdebug_psg *psg = &sound.psg;
        emit(buf, "PSG:");
        for (idx = 0; idx < 16; idx++) emit(buf, " R%X=%02X", idx, psg->reg[idx]);
        emit(buf, "\n");
        for (idx = 0; idx < 3; idx++)
            emit(buf, "  %c: period %4d (%.1f Hz)%s%s level %d%s\n", 'A' + idx, psg->period[idx], psg->freq[idx],
                 psg->tone_on[idx] ? " tone" : "", psg->noise_on[idx] ? " noise" : "", psg->level[idx],
                 psg->envelope[idx] ? " envelope" : "");
        emit(buf, "  noise %d  envelope %d shape %X\n", psg->noise_period, psg->env_period, psg->env_shape);
    }
    if (sound.scc.present) {
        emit(buf, "SCC (%s)\n", sound.scc.name);
        for (idx = 0; idx < 5; idx++)
            emit(buf, "  %d: period %4d (%.1f Hz) volume %2d %s\n", idx + 1, sound.scc.period[idx],
                 sound.scc.freq[idx], sound.scc.volume[idx], sound.scc.enabled[idx] ? "on" : "off");
    }
    if (sound.opll.present) {
        emit(buf, "MSX-MUSIC (%s)%s\n", sound.opll.name, sound.opll.rhythm ? " rhythm mode" : "");
        for (idx = 0; idx < 9; idx++)
            emit(buf, "  %d: fnum %3d block %d inst %2d volume %2d %s\n", idx + 1, sound.opll.fnum[idx],
                 sound.opll.block[idx], sound.opll.instrument[idx], sound.opll.volume[idx],
                 sound.opll.key_on[idx] ? "key on" : "off");
    }
    if (sound.y8950.present) {
        emit(buf, "MSX-AUDIO (%s)\n", sound.y8950.name);
        dump(buf, 0, sound.y8950.reg, 0x100);
    }
}

int msxdebug_command(msxdebug *dbg, const char *command, char *dst, int dstsz)
{
    char line[512];
    char *cursor = line, *cmd;
    outbuf buf = { dst, dstsz, 0 };
    char word[32];
    long val, aux;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!dbg || !command) return 0;
    snprintf(line, sizeof line, "%s", command);
    cmd = next_tok(&cursor);
    if (!cmd) return 0;
    {
        /* the command word is case-insensitive; the rest of the line is not */
        char *ltr;
        for (ltr = cmd; *ltr; ltr++) *ltr = (char)tolower((unsigned char)*ltr);
    }

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        emit(&buf, "%s", k_help);
    } else if (!strcmp(cmd, "step") || !strcmp(cmd, "s")) {
        char *tok = next_tok(&cursor);
        long count = 1, idx;
        if (tok && eval_text(dbg, &buf, tok, &count)) return buf.len;
        if (count < 1) count = 1;
        if (count > 1000) count = 1000;
        if (!msxdebug_is_stopped(dbg)) {
            emit(&buf, "not stopped\n");
        } else {
            for (idx = 0; idx < count; idx++) msxdebug_step(dbg);
            where_text(dbg, &buf);
        }
    } else if (!strcmp(cmd, "over") || !strcmp(cmd, "n")) {
        msxdebug_step_over(dbg);
        emit(&buf, "stepping over...\n");
    } else if (!strcmp(cmd, "out")) {
        msxdebug_step_out(dbg);
        emit(&buf, "stepping out...\n");
    } else if (!strcmp(cmd, "run") || !strcmp(cmd, "go") || !strcmp(cmd, "c")) {
        msxdebug_resume(dbg);
        emit(&buf, "running\n");
    } else if (!strcmp(cmd, "stop")) {
        msxdebug_stop(dbg);
        where_text(dbg, &buf);
    } else if (!strcmp(cmd, "frame")) {
        msxdebug_frame(dbg);
        emit(&buf, "running to the next VDP interrupt...\n");
    } else if (!strcmp(cmd, "runto")) {
        char *tok = next_tok(&cursor);
        if (!tok || eval_addr(dbg, &buf, tok, &val)) {
            if (!tok) emit(&buf, "usage: runto <addr>\n");
            return buf.len;
        }
        msxdebug_run_to(dbg, (uint16_t)val);
        emit(&buf, "running to $%04X...\n", (unsigned)(val & 0xFFFF));
    } else if (!strcmp(cmd, "break") || !strcmp(cmd, "bp")) {
        add_bp(dbg, &buf, MSXDEBUG_BP_EXEC, cursor);
    } else if (!strcmp(cmd, "bpr")) {
        add_bp(dbg, &buf, MSXDEBUG_BP_READ, cursor);
    } else if (!strcmp(cmd, "bpw")) {
        add_bp(dbg, &buf, MSXDEBUG_BP_WRITE, cursor);
    } else if (!strcmp(cmd, "bpa")) {
        add_bp(dbg, &buf, MSXDEBUG_BP_ACCESS, cursor);
    } else if (!strcmp(cmd, "bpin")) {
        add_bp(dbg, &buf, MSXDEBUG_BP_IN, cursor);
    } else if (!strcmp(cmd, "bpout")) {
        add_bp(dbg, &buf, MSXDEBUG_BP_OUT, cursor);
    } else if (!strcmp(cmd, "delete")) {
        char *tok = next_tok(&cursor);
        msxdebug_breakpoint list[128];
        int count, idx, target = 0;
        if (!tok || eval_text(dbg, &buf, tok[0] == '#' ? tok + 1 : tok, &val)) {
            if (!tok) emit(&buf, "usage: delete <id|addr>\n");
            return buf.len;
        }
        count = msxdebug_breakpoint_list(dbg, list, 128);
        for (idx = 0; idx < count && !target; idx++)
            if (list[idx].id == val) target = list[idx].id;
        for (idx = 0; idx < count && !target; idx++)
            if (list[idx].type == MSXDEBUG_BP_EXEC && list[idx].start == (val & 0xFFFF)) target = list[idx].id;
        if (target) {
            msxdebug_breakpoint_remove(dbg, target);
            emit(&buf, "deleted %d\n", target);
        } else {
            emit(&buf, "no such breakpoint\n");
        }
    } else if (!strcmp(cmd, "enable") || !strcmp(cmd, "disable")) {
        char *tok = next_tok(&cursor);
        if (!tok || eval_text(dbg, &buf, tok[0] == '#' ? tok + 1 : tok, &val)) {
            if (!tok) emit(&buf, "usage: %s <id>\n", cmd);
            return buf.len;
        }
        msxdebug_breakpoint_enable(dbg, (int)val, !strcmp(cmd, "enable"));
        emit(&buf, "%sd %ld\n", cmd, val);
    } else if (!strcmp(cmd, "clear")) {
        msxdebug_breakpoint_clear(dbg);
        emit(&buf, "all breakpoints cleared\n");
    } else if (!strcmp(cmd, "hits")) {
        char *tok = next_tok(&cursor);
        if (tok && !strcmp(tok, "clear")) {
            msxdebug_breakpoint_clear_hits(dbg);
            emit(&buf, "hit counts cleared\n");
        } else {
            list_breaks(dbg, &buf);
        }
    } else if (!strcmp(cmd, "breaks") || !strcmp(cmd, "bl")) {
        list_breaks(dbg, &buf);
    } else if (!strcmp(cmd, "print") || !strcmp(cmd, "p")) {
        if (eval_text(dbg, &buf, rest_of(cursor), &val) == 0) {
            char bin[17];
            int bit;
            for (bit = 0; bit < 16; bit++) bin[bit] = (char)('0' + ((val >> (15 - bit)) & 1));
            bin[16] = '\0';
            emit(&buf, "$%04lX  %ld  %%%s\n", (unsigned long)val & 0xFFFF, val, bin);
        }
    } else if (!strcmp(cmd, "regs") || !strcmp(cmd, "r")) {
        show_regs(dbg, &buf);
    } else if (!strcmp(cmd, "set") && reg_by_name(peek_word(cursor, word, sizeof word)) >= 0) {
        char *name = next_tok(&cursor);
        const int reg = name ? reg_by_name(name) : -1;
        if (eval_text(dbg, &buf, rest_of(cursor), &val) == 0) {
            if (!msxdebug_is_stopped(dbg)) {
                emit(&buf, "the machine is running (stop first)\n");
            } else {
                msxdebug_cpu_set(dbg, reg, (int)val);
                show_regs(dbg, &buf);
            }
        }
    } else if (!strcmp(cmd, "mem") || !strcmp(cmd, "m")) {
        char *addr_tok = next_tok(&cursor), *len_tok = next_tok(&cursor);
        uint8_t bytes[512];
        aux = 64;
        if (!addr_tok) {
            emit(&buf, "usage: mem <addr> [len]\n");
            return buf.len;
        }
        if (eval_addr(dbg, &buf, addr_tok, &val) || (len_tok && eval_text(dbg, &buf, len_tok, &aux)))
            return buf.len;
        if (aux > (long)sizeof bytes) aux = (long)sizeof bytes;
        if (aux < 1) aux = 1;
        aux = msxdebug_read(dbg, (uint16_t)val, bytes, (int)aux);
        dump(&buf, (uint16_t)val, bytes, (int)aux);
    } else if (!strcmp(cmd, "poke")) {
        char *tok = next_tok(&cursor), *byte_tok;
        int count = 0;
        if (!tok) {
            emit(&buf, "usage: poke <addr> <val...>\n");
            return buf.len;
        }
        if (eval_addr(dbg, &buf, tok, &val)) return buf.len;
        while ((byte_tok = next_tok(&cursor)) != NULL) {
            if (eval_text(dbg, &buf, byte_tok, &aux)) return buf.len;
            msxdebug_write(dbg, (uint16_t)(val + count), (uint8_t)aux);
            count++;
        }
        emit(&buf, "%d byte%s at $%04lX\n", count, count == 1 ? "" : "s", (unsigned long)val & 0xFFFF);
    } else if (!strcmp(cmd, "disasm") || !strcmp(cmd, "dis") || !strcmp(cmd, "d")) {
        char *addr_tok = next_tok(&cursor), *count_tok = next_tok(&cursor);
        msxdebug_line lines[64];
        msxdebug_cpu cpu;
        int count, idx;
        msxdebug_cpu_get(dbg, &cpu);
        val = cpu.pc;
        aux = 8;
        if ((addr_tok && eval_addr(dbg, &buf, addr_tok, &val)) || (count_tok && eval_text(dbg, &buf, count_tok, &aux)))
            return buf.len;
        if (aux > 64) aux = 64;
        if (aux < 1) aux = 1;
        count = msxdebug_disassemble(dbg, (uint16_t)val, lines, (int)aux, NULL);
        for (idx = 0; idx < count; idx++)
            emit(&buf, "%c%04X  %-12s %s%s%s\n", lines[idx].is_pc ? '>' : ' ', lines[idx].address,
                 lines[idx].bytes, lines[idx].disasm, lines[idx].label[0] ? "   ; " : "", lines[idx].label);
    } else if (!strcmp(cmd, "label")) {
        char *addr_tok = next_tok(&cursor), *name = next_tok(&cursor);
        if (!addr_tok || !name || eval_addr(dbg, &buf, addr_tok, &val)) {
            if (!addr_tok || !name) emit(&buf, "usage: label <addr> <name>\n");
            return buf.len;
        }
        if (msxdebug_set_label(dbg, (uint16_t)val, name) == 0)
            emit(&buf, "$%04X = %s\n", (unsigned)(val & 0xFFFF), name);
        else
            emit(&buf, "bad label\n");
    } else if (!strcmp(cmd, "labels")) {
        char msg[256];
        char *file = rest_of(cursor);
        if (*file) {
            msxdebug_load_symbols(dbg, file, msg, sizeof msg);
            emit(&buf, "%s\n", msg);
        } else {
            emit(&buf, "usage: labels <file>\n");
        }
    } else if (!strcmp(cmd, "symbols")) {
        int picks[40], count, idx;
        char *text = rest_of(cursor);
        count = msxdebug_symbols_filter(dbg, text, picks, 40);
        if (!count) emit(&buf, "no labels match\n");
        for (idx = 0; idx < count; idx++) {
            uint16_t addr;
            char name[64];
            if (msxdebug_symbols_get(dbg, picks[idx], &addr, name, sizeof name))
                emit(&buf, "$%04X  %s\n", addr, name);
        }
    } else if (!strcmp(cmd, "trace") && is_trace_arg(peek_word(cursor, word, sizeof word))) {
        char *tok = next_tok(&cursor);
        if (tok && !strcmp(tok, "on")) {
            msxdebug_trace_enable(dbg, 1);
            emit(&buf, "trace on\n");
        } else if (tok && !strcmp(tok, "off")) {
            msxdebug_trace_enable(dbg, 0);
            emit(&buf, "trace off\n");
        } else if (tok && !strcmp(tok, "clear")) {
            msxdebug_trace_clear(dbg);
        } else {
            msxdebug_trace rows[64];
            int count, idx;
            aux = 16;
            if (tok && eval_text(dbg, &buf, tok, &aux)) return buf.len;
            if (aux > 64) aux = 64;
            if (aux < 1) aux = 1;
            count = msxdebug_trace_read(dbg, rows, (int)aux);
            if (!count) emit(&buf, "trace is %s and empty\n", msxdebug_trace_enabled(dbg) ? "on" : "off");
            for (idx = count - 1; idx >= 0; idx--) {
                char text[160];
                msxdebug_trace_format(&rows[idx], text, sizeof text);
                emit(&buf, "%10llu  %s\n", (unsigned long long)rows[idx].cycles, text);
            }
        }
    } else if (!strcmp(cmd, "watch")) {
        char *text = rest_of(cursor);
        int idx;
        if (*text && msxwatch_add(text) < 0) emit(&buf, "no room for another watch\n");
        if (!msxwatch_count()) emit(&buf, "no watches (watch <addr|label|lo-hi>)\n");
        for (idx = 0; idx < msxwatch_count(); idx++) {
            char row[160];
            msxwatch_format(dbg, idx, row, sizeof row);
            emit(&buf, "%2d  %s\n", idx, row);
        }
    } else if (!strcmp(cmd, "unwatch")) {
        char *tok = next_tok(&cursor);
        if (!tok) {
            emit(&buf, "unwatch <n|all>\n");
        } else if (!strcmp(tok, "all")) {
            msxwatch_clear();
        } else if (eval_text(dbg, &buf, tok, &val) == 0) {
            msxwatch_remove((int)val);
        }
    } else if (!strcmp(cmd, "find")) {
        char *tok = next_tok(&cursor);
        char joined[200];
        int kind = MSXSEARCH_HEX, count = 0, flags = 0, found, idx;
        uint8_t pat[MSXSEARCH_MAX], mask;
        uint16_t hits[32];
        if (tok && !strcmp(tok, "text")) {
            kind = MSXSEARCH_TEXT;
            tok = rest_of(cursor);
        } else if (tok && !strcmp(tok, "hex")) {
            tok = rest_of(cursor);
        } else if (tok) {
            snprintf(joined, sizeof joined, "%s %s", tok, rest_of(cursor));
            tok = joined;
        }
        if (!tok || !*tok || !msxsearch_parse(kind, tok, pat, &count, &mask, &flags)) {
            emit(&buf, "find [hex] <bytes> | find text <string>\n");
            return buf.len;
        }
        found = msxdebug_search(dbg, pat, count, mask, flags, 0, 0xFFFF, hits, 32);
        if (found < 0) emit(&buf, "stop the machine first\n");
        else if (!found) emit(&buf, "not found\n");
        for (idx = 0; idx < found; idx++) emit(&buf, "$%04X\n", hits[idx]);
    } else if (!strcmp(cmd, "map")) {
        msxdebug_region map[16];
        const int count = msxdebug_memory_map(dbg, map, 16);
        int idx;
        for (idx = 0; idx < count; idx++)
            emit(&buf, "$%04X-$%04X  %s%s%s\n", map[idx].lo, map[idx].hi, map[idx].name,
                 map[idx].readable ? "" : " (no read)", map[idx].writable ? "" : " (read only)");
    } else if (!strcmp(cmd, "io")) {
        char text[6000];
        msxdebug_io_state(dbg, text, sizeof text);
        emit(&buf, "%s", text[0] ? text : "the machine is not running\n");
    } else if (!strcmp(cmd, "cart")) {
        char text[300];
        msxdebug_cart_summary(dbg, text, sizeof text);
        emit(&buf, "%s\n", text);
    } else if (!strcmp(cmd, "sound")) {
        show_sound(dbg, &buf);
    } else if (!strcmp(cmd, "save")) {
        char *kind = next_tok(&cursor);
        char *file = rest_of(cursor);
        char msg[300];
        if (!kind || !*file) {
            emit(&buf, "usage: save <dis|mem|vram|regs> <file>\n");
        } else {
            msxdebug_save(dbg, kind, file, msg, sizeof msg);
            emit(&buf, "%s\n", msg);
        }
    } else if (!strcmp(cmd, "video")) {
        char text[4096];
        char *section = next_tok(&cursor);
        if (msxvid_tab_text_for(dbg, section, text, sizeof text) > 0)
            emit(&buf, "%s", text);
        else
            emit(&buf, "no video state yet\n");
    } else {
        /* openMSX's own console */
        char res[8192];
        const int answered = msxdebug_tcl(dbg, command, res, sizeof res);
        if (!answered) emit(&buf, "error: %s\n", res);
        else if (res[0]) emit(&buf, "%s\n", res);
    }
    return buf.len;
}

int msxdebug_completions(msxdebug *dbg, const char *prefix, char *dst, int dstsz)
{
    outbuf buf = { dst, dstsz, 0 };
    const char *word;
    size_t len;
    int count = 0, idx;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!dbg || !prefix) return 0;
    word = strrchr(prefix, ' ');
    word = word ? word + 1 : prefix;
    len = strlen(word);

    if (word == prefix) {       /* the command word */
        for (idx = 0; k_commands[idx]; idx++)
            if (!strncmp(k_commands[idx], word, len)) {
                emit(&buf, "%s\n", k_commands[idx]);
                count++;
            }
    }
    if (!strncmp(prefix, "save ", 5) && !strchr(prefix + 5, ' ')) {
        static const char *const k_kinds[] = { "dis", "mem", "vram", "regs", NULL };
        for (idx = 0; k_kinds[idx]; idx++)
            if (!strncmp(k_kinds[idx], word, len)) {
                emit(&buf, "%s\n", k_kinds[idx]);
                count++;
            }
        return count;
    }
    if (len) {                  /* registers, then labels */
        const sym_tables *tables = msxdebug_symtabs(dbg);
        int table_idx;
        for (idx = 0; msxdebug_reg_names[idx]; idx++)
            if (word != prefix && !strncmp(msxdebug_reg_names[idx], word, len)) {
                emit(&buf, "%s\n", msxdebug_reg_names[idx]);
                count++;
            }
        for (table_idx = 0; tables && table_idx < tables->count; table_idx++) {
            const sym_table *table = &tables->tables[table_idx];
            for (idx = 0; idx < table->count && count < 40; idx++)
                if (!strncasecmp(table->entries[idx].name, word, len)) {
                    emit(&buf, "%s\n", table->entries[idx].name);
                    count++;
                }
        }
    }
    /* what openMSX itself completes (a Tcl command line) */
    if (strchr(prefix, ' ') || !count) {
        char cmd[640], res[512];
        snprintf(cmd, sizeof cmd, "tabcompletion {%s}", prefix);
        if (!strchr(prefix, '{') && !strchr(prefix, '}') && msxdebug_rpc(cmd, res, sizeof res) &&
            strlen(res) > strlen(prefix) && !strncmp(res, prefix, strlen(prefix))) {
            emit(&buf, "%s\n", res + (word - prefix));
            count++;
        }
    }
    return count;
}
