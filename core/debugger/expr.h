/*
 * expr -- the debugger's expression evaluator: breakpoint conditions,
 * print, set, and every address argument the prompt takes.
 *
 *   numbers     $C000  0xC000  C000h  49152  %1010
 *   names       whatever the context resolves: Z80 registers and flags,
 *               labels, frame, cycles, and value/addr/old for an access
 *   memory      [expr] the byte at that address, {expr} the 16-bit word
 *               (little-endian), both through the slot map the CPU sees now
 *               and side-effect free
 *   operators   C's, with C's precedence: unary - ! ~, * / %, + -, << >>,
 *               < <= > >=, == !=, &, ^, |, &&, ||
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MSX_EXPR_H
#define MSX_EXPR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* A name to a value; return 1 if known. */
    int (*name)(void *user, const char *name, long *out);
    /* A byte of memory, without side effects. */
    uint8_t (*read_byte)(void *user, uint16_t addr);
    void *user;
} msxexpr_ctx;

/* Evaluate; 0 and *out on success, -1 with a message otherwise. With a NULL
 * context, names evaluate to 0 and memory reads to 0 -- a syntax check. */
int msxexpr_eval(const char *text, const msxexpr_ctx *ctx, long *out, char *err, int errsz);

/* The same text as an openMSX Tcl expression, for a condition openMSX
 * evaluates itself: operators and precedence are the same in Tcl, numbers
 * become decimal, [x] and {x} become peek and peek16, and `name` writes the
 * Tcl text a name stands for ("[reg A]", "123") into out and returns 1, or
 * returns 0 for a name it does not know. Returns 0 with the text in out, or
 * -1 with a message in err (a syntax error, an unknown name, too long). */
typedef struct {
    int (*name)(void *user, const char *name, char *out, int outsz);
    void *user;
} msxexpr_tcl_ctx;

int msxexpr_to_tcl(const char *text, const msxexpr_tcl_ctx *ctx, char *out, int outsz, char *err, int errsz);

#ifdef __cplusplus
}
#endif

#endif /* MSX_EXPR_H */
