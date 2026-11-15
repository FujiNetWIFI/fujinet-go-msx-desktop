/*
 * expr.c -- a recursive-descent evaluator for the grammar in expr.h, and a
 * token-level translator of the same grammar to an openMSX Tcl expression.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "expr.h"

typedef struct {
    const char *cur;
    const msxexpr_ctx *ctx;
    char *err;
    int errsz;
    int failed;
} parser;

static void fail(parser *prs, const char *msg)
{
    if (!prs->failed && prs->err && prs->errsz > 0)
        snprintf(prs->err, (size_t)prs->errsz, "%s", msg);
    prs->failed = 1;
}

static void skip(parser *prs)
{
    while (*prs->cur && isspace((unsigned char)*prs->cur))
        prs->cur++;
}

static int accept(parser *prs, const char *tok)
{
    size_t len = strlen(tok);
    skip(prs);
    if (strncmp(prs->cur, tok, len) == 0)
    {
        prs->cur += len;
        return 1;
    }
    return 0;
}

static long expr_or(parser *prs);

static int is_name_start(char chr)
{
    return isalpha((unsigned char)chr) || chr == '_' || chr == '.' || chr == '@';
}

static int is_name_char(char chr)
{
    return isalnum((unsigned char)chr) || chr == '_' || chr == '.' || chr == '@' || chr == '\'';
}

static long number(parser *prs)
{
    const char *str = prs->cur;
    char *end;
    long val;

    if (*str == '$')
    {
        val = strtol(str + 1, &end, 16);
        if (end == str + 1)
            fail(prs, "bad hex number");
        prs->cur = end;
        return val;
    }
    if (*str == '%')
    {
        val = strtol(str + 1, &end, 2);
        if (end == str + 1)
            fail(prs, "bad binary number");
        prs->cur = end;
        return val;
    }
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X'))
    {
        val = strtol(str + 2, &end, 16);
        if (end == str + 2)
            fail(prs, "bad hex number");
        prs->cur = end;
        return val;
    }
    /* digits: hex with an h suffix (C000h must start with a digit, 0C000h),
     * else decimal */
    {
        const char *look = str;
        while (isxdigit((unsigned char)*look))
            look++;
        if ((*look == 'h' || *look == 'H') && !is_name_char(look[1]))
        {
            val = strtol(str, NULL, 16);
            prs->cur = look + 1;
            return val;
        }
    }
    val = strtol(str, &end, 10);
    prs->cur = end;
    return val;
}

static long primary(parser *prs)
{
    long val;

    skip(prs);
    if (accept(prs, "("))
    {
        val = expr_or(prs);
        if (!accept(prs, ")"))
            fail(prs, "missing )");
        return val;
    }
    if (accept(prs, "["))
    {
        long addr = expr_or(prs);
        if (!accept(prs, "]"))
            fail(prs, "missing ]");
        return prs->ctx && prs->ctx->read_byte ? prs->ctx->read_byte(prs->ctx->user, (uint16_t)addr) : 0;
    }
    if (accept(prs, "{"))
    {
        long addr = expr_or(prs);
        long low, high;
        if (!accept(prs, "}"))
            fail(prs, "missing }");
        if (!prs->ctx || !prs->ctx->read_byte)
            return 0;
        low = prs->ctx->read_byte(prs->ctx->user, (uint16_t)addr);
        high = prs->ctx->read_byte(prs->ctx->user, (uint16_t)(addr + 1));
        return low | (high << 8);
    }
    if (isdigit((unsigned char)*prs->cur) || *prs->cur == '$' || *prs->cur == '%')
        return number(prs);
    if (is_name_start(*prs->cur))
    {
        char name[64];
        int len = 0;
        while (is_name_char(*prs->cur) && len + 1 < (int)sizeof name)
            name[len++] = *prs->cur++;
        name[len] = '\0';
        if (!prs->ctx)
            return 0;
        if (prs->ctx->name && prs->ctx->name(prs->ctx->user, name, &val))
            return val;
        {
            char msg[96];
            snprintf(msg, sizeof msg, "unknown name '%s'", name);
            fail(prs, msg);
        }
        return 0;
    }
    if (*prs->cur)
        fail(prs, "unexpected character");
    else
        fail(prs, "expression expected");
    return 0;
}

static long unary(parser *prs)
{
    if (accept(prs, "-"))
        return -unary(prs);
    if (accept(prs, "!"))
        return !unary(prs);
    if (accept(prs, "~"))
        return ~unary(prs);
    if (accept(prs, "+"))
        return unary(prs);
    return primary(prs);
}

static long mul(parser *prs)
{
    long val = unary(prs);
    for (;;)
    {
        if (accept(prs, "*"))
            val *= unary(prs);
        else if (accept(prs, "/"))
        {
            long den = unary(prs);
            if (den == 0)
            {
                fail(prs, "division by zero");
                return 0;
            }
            val = den == -1 ? -val : val / den;
        }
        else if (accept(prs, "%"))
        {
            long den = unary(prs);
            if (den == 0)
            {
                fail(prs, "division by zero");
                return 0;
            }
            val = den == -1 ? 0 : val % den;
        }
        else
            return val;
    }
}

static long add(parser *prs)
{
    long val = mul(prs);
    for (;;)
    {
        if (accept(prs, "+"))
            val += mul(prs);
        else if (accept(prs, "-"))
            val -= mul(prs);
        else
            return val;
    }
}

static long shift(parser *prs)
{
    long val = add(prs);
    for (;;)
    {
        if (accept(prs, "<<"))
        {
            long count = add(prs);
            val = count < 0 || count > 62 ? 0 : (long)((unsigned long)val << count);
        }
        else if (accept(prs, ">>"))
        {
            long count = add(prs);
            val = count < 0 || count > 62 ? 0 : val >> count;
        }
        else
            return val;
    }
}

static long rel(parser *prs)
{
    long val = shift(prs);
    for (;;)
    {
        if (accept(prs, "<="))
            val = val <= shift(prs);
        else if (accept(prs, ">="))
            val = val >= shift(prs);
        else if (strncmp((skip(prs), prs->cur), "<<", 2) != 0 && accept(prs, "<"))
            val = val < shift(prs);
        else if (strncmp((skip(prs), prs->cur), ">>", 2) != 0 && accept(prs, ">"))
            val = val > shift(prs);
        else
            return val;
    }
}

static long equality(parser *prs)
{
    long val = rel(prs);
    for (;;)
    {
        if (accept(prs, "=="))
            val = val == rel(prs);
        else if (accept(prs, "!="))
            val = val != rel(prs);
        else
            return val;
    }
}

static long band(parser *prs)
{
    long val = equality(prs);
    for (;;)
    {
        skip(prs);
        if (prs->cur[0] == '&' && prs->cur[1] != '&')
        {
            prs->cur++;
            val &= equality(prs);
        }
        else
            return val;
    }
}

static long bxor(parser *prs)
{
    long val = band(prs);
    while (accept(prs, "^"))
        val ^= band(prs);
    return val;
}

static long bor(parser *prs)
{
    long val = bxor(prs);
    for (;;)
    {
        skip(prs);
        if (prs->cur[0] == '|' && prs->cur[1] != '|')
        {
            prs->cur++;
            val |= bxor(prs);
        }
        else
            return val;
    }
}

static long land(parser *prs)
{
    long val = bor(prs);
    while (accept(prs, "&&"))
    {
        long rhs = bor(prs);
        val = val && rhs;
    }
    return val;
}

static long expr_or(parser *prs)
{
    long val = land(prs);
    while (accept(prs, "||"))
    {
        long rhs = land(prs);
        val = val || rhs;
    }
    return val;
}

int msxexpr_eval(const char *text, const msxexpr_ctx *ctx, long *out, char *err, int errsz)
{
    parser prs;
    long val;

    memset(&prs, 0, sizeof prs);
    prs.cur = text ? text : "";
    prs.ctx = ctx;
    prs.err = err;
    prs.errsz = errsz;
    if (err && errsz > 0)
        err[0] = '\0';

    val = expr_or(&prs);
    skip(&prs);
    if (!prs.failed && *prs.cur)
        fail(&prs, "unexpected text after the expression");
    if (prs.failed)
        return -1;
    if (out)
        *out = val;
    return 0;
}

/* ---- the same grammar as an openMSX Tcl expression --------------------------
 * A breakpoint condition runs inside openMSX, where the debugger's own
 * evaluator is not: so the text is rewritten token by token. Operators are the
 * same in Tcl and in C with the same precedence, so they pass through; the
 * numbers become decimal, [x] and {x} become peek/peek16, and every name is
 * whatever the caller says it stands for. The grammar is checked first by
 * msxexpr_eval with a NULL context, so a text that gets here is well formed. */

typedef struct {
    const char *cur;
    const msxexpr_tcl_ctx *ctx;
    char *out;
    int cap, len;
    char *err;
    int errsz;
    int failed;
} tclgen;

static void tcl_fail(tclgen *gen, const char *msg)
{
    if (!gen->failed && gen->err && gen->errsz > 0)
        snprintf(gen->err, (size_t)gen->errsz, "%s", msg);
    gen->failed = 1;
}

static void tcl_put(tclgen *gen, const char *text)
{
    const size_t len = strlen(text);
    if (gen->failed)
        return;
    if (gen->len + (int)len + 1 > gen->cap)
    {
        tcl_fail(gen, "the condition is too long");
        return;
    }
    memcpy(gen->out + gen->len, text, len + 1);
    gen->len += (int)len;
}

static void tcl_group(tclgen *gen, char closer);

static void tcl_memory(tclgen *gen, const char *head, char closer)
{
    tcl_put(gen, head);
    tcl_group(gen, closer);
    tcl_put(gen, ") & 0xFFFF}]]");
}

/* Emits tokens until `closer` (consumed) or the end of the text. */
static void tcl_group(tclgen *gen, char closer)
{
    int operand = 1;     /* the next token is an operand, not an operator */

    while (!gen->failed)
    {
        const char chr = *gen->cur;

        if (!chr)
        {
            if (closer)
                tcl_fail(gen, "missing closing bracket");
            return;
        }
        if (isspace((unsigned char)chr))
        {
            gen->cur++;
            tcl_put(gen, " ");
            continue;
        }
        if (closer && chr == closer)
        {
            gen->cur++;
            return;
        }
        if (chr == '[' || chr == '{')
        {
            gen->cur++;
            tcl_memory(gen, chr == '[' ? "[peek [expr {(" : "[peek16 [expr {(", chr == '[' ? ']' : '}');
            operand = 0;
            continue;
        }
        if (chr == '(')
        {
            gen->cur++;
            tcl_put(gen, "(");
            tcl_group(gen, ')');
            tcl_put(gen, ")");
            operand = 0;
            continue;
        }
        if (operand && (isdigit((unsigned char)chr) || chr == '$' || chr == '%'))
        {
            parser prs;
            char digits[32];
            memset(&prs, 0, sizeof prs);
            prs.cur = gen->cur;
            snprintf(digits, sizeof digits, "%ld", number(&prs));
            if (prs.failed)
            {
                tcl_fail(gen, "bad number");
                return;
            }
            gen->cur = prs.cur;
            tcl_put(gen, digits);
            operand = 0;
            continue;
        }
        if (operand && is_name_start(chr))
        {
            char name[64];
            char text[256];
            int len = 0;
            while (is_name_char(*gen->cur) && len + 1 < (int)sizeof name)
                name[len++] = *gen->cur++;
            name[len] = '\0';
            if (!gen->ctx || !gen->ctx->name || !gen->ctx->name(gen->ctx->user, name, text, (int)sizeof text))
            {
                char msg[96];
                snprintf(msg, sizeof msg, "unknown name '%s'", name);
                tcl_fail(gen, msg);
                return;
            }
            tcl_put(gen, "(");
            tcl_put(gen, text);
            tcl_put(gen, ")");
            operand = 0;
            continue;
        }
        if (strchr("+-*/%&|^<>=!~", chr))
        {
            char one[2] = { chr, '\0' };
            gen->cur++;
            tcl_put(gen, one);
            /* a binary operator is followed by an operand; so is a unary one */
            operand = 1;
            continue;
        }
        tcl_fail(gen, "unexpected character");
        return;
    }
}

int msxexpr_to_tcl(const char *text, const msxexpr_tcl_ctx *ctx, char *out, int outsz, char *err, int errsz)
{
    tclgen gen;

    if (err && errsz > 0)
        err[0] = '\0';
    if (!out || outsz <= 0)
        return -1;
    out[0] = '\0';
    if (msxexpr_eval(text, NULL, NULL, err, errsz) != 0)
        return -1;
    memset(&gen, 0, sizeof gen);
    gen.cur = text;
    gen.ctx = ctx;
    gen.out = out;
    gen.cap = outsz;
    gen.err = err;
    gen.errsz = errsz;
    tcl_group(&gen, '\0');
    return gen.failed ? -1 : 0;
}
