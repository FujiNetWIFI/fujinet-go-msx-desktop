/*
 * Symbol tables (internal to the msxdebug engine). Ported verbatim from
 * fujinet-go-adam-desktop's core/debugger/symbols.{c,h} -- generic .sym
 * text-format parsing and address lookup, nothing ADAM-specific in here.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MSXDEBUG_SYMBOLS_H
#define MSXDEBUG_SYMBOLS_H

#include <stdint.h>

#define SYM_MAX_TABLES 8

typedef struct {
    uint16_t addr;
    char name[48];
} sym_entry;

typedef struct {
    char name[16];
    sym_entry *entries; /* sorted by address */
    int count, cap;
    uint16_t lo, hi;
} sym_table;

typedef struct {
    sym_table tables[SYM_MAX_TABLES];
    int count;
} sym_tables;

int  symtabs_load_text(sym_tables *st, const char *text, const char *name);
int  symtabs_load_file(sym_tables *st, const char *path, const char *name);
const char *symtabs_at(const sym_tables *st, uint16_t addr, uint16_t *offset);
int  symtabs_find(const sym_tables *st, const char *name, uint16_t *addr);
void symtabs_free(sym_tables *st);

/* Built-in table, hand-written in symbols_builtin.c from public MSX BIOS/
 * BDOS disassembly listings (names and addresses only -- see
 * COMPLIANCE.md). */
extern const char msxdebug_builtin_bios_sym[];

#endif /* MSXDEBUG_SYMBOLS_H */
