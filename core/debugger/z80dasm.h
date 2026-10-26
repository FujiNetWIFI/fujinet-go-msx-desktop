/*
 * Z80 disassembler, ported verbatim from fujinet-go-adam-desktop (same CPU,
 * already tested) -- see z80dasm.c.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef Z80DASM_H
#define Z80DASM_H

#include <stdint.h>

/* Instruction flags (z80d_insn.flags). */
#define MSXDASM_JUMP     0x01
#define MSXDASM_CALL     0x02
#define MSXDASM_RET      0x04
#define MSXDASM_COND     0x08
#define MSXDASM_RELATIVE 0x10
#define MSXDASM_BLOCK    0x20 /* LDIR/CPIR/INIR/OTIR family */
#define MSXDASM_HALT     0x40

/* One decoded instruction. */
typedef struct {
    uint16_t addr;
    uint8_t len; /* 1..4 */
    uint8_t bytes[4];
    char text[32];
    uint16_t target;
    uint8_t flags; /* MSXDASM_* */
} z80d_insn;

/* Decodes the instruction at addr from code[0..3]; returns its length. */
int z80_disassemble(z80d_insn *out, uint16_t addr, const uint8_t code[4]);

#endif /* Z80DASM_H */
