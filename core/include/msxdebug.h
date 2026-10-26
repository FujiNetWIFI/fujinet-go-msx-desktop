/*
 * msxdebug -- the debugger contract for FujiNet Go MSX Desktop: a Z80
 * debugger engine behind a C API the four native debugger windows share.
 * Same shape as the SMS sibling's smsdebug.h (the family's newest Z80
 * debugger), so the windows carry over; the engine is this repository's own
 * and is built entirely on openMSX's Tcl debug interface via
 * core/openmsx/msx_host.hh's msxhost_execute_sync()/_binary(): "debug break"
 * /"step"/"cont", "debug set_bp"/"set_watchpoint", "debug read_block <
 * debuggable> ...", "reg", "slotmap". There is no exec hook of our own and
 * no per-instruction trace ring: openMSX runs autonomously on its own thread
 * at full speed, and a Tcl round-trip per instruction would be many orders
 * of magnitude too slow for a 3.5 MHz Z80. Step-over/step-out/frame are
 * therefore one-shot breakpoints, and each persistent breakpoint carries a
 * small Tcl action that counts its hits and says which one fired.
 *
 * The engine only engages while a window holds it attached:
 * msxdebug_attach() when the window opens -- which also stops the machine,
 * as on every sibling -- and msxdebug_detach() when it closes, which takes
 * every breakpoint out of openMSX and lets the machine run on at full speed.
 * Breakpoints are kept for the next attach. The FujiNet trace is the
 * exception: it records from session start, so it shows what happened before
 * anyone opened a window.
 *
 * Calls are safe from the UI thread at any time (they block until openMSX
 * has answered). Inspection is meant for the stopped state; while running
 * it answers with whatever is current. A window learns of stops (a
 * breakpoint firing on its own) by polling msxdebug_generation().
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MSXDEBUG_H
#define MSXDEBUG_H

#include <stdint.h>

#include "msxsession.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- attach / stop / go ---------------------------------------------------- */
/* Engage the engine and stop at the next instruction. Idempotent. Before the
 * machine has started it stops it as it comes up. */
void msxdebug_attach(msxdebug *d);
/* Disengage and let the machine run. Breakpoints are kept for the next
 * attach. */
void msxdebug_detach(msxdebug *d);
int  msxdebug_is_attached(msxdebug *d);

int  msxdebug_is_stopped(msxdebug *d);
void msxdebug_stop(msxdebug *d);         /* break at the next instruction */
void msxdebug_resume(msxdebug *d);       /* "run" */
/* Why the machine last stopped ("stopped", "step", "breakpoint 2 at $4010",
 * "watchpoint 1: write $F3B0", "frame", ...) and the PC, -1 if none.
 * Returns length. */
int  msxdebug_stop_reason(msxdebug *d, char *dst, int dstsz, int *address);
/* Bumped whenever the machine stopped/resumed, a command ran, or the
 * breakpoint list changed, so a window knows when to refresh. */
unsigned msxdebug_generation(msxdebug *d);

/* ---- the console ------------------------------------------------------------ */
/* Run one command line. "help" lists the short forms: step [n], over, out,
 * run, stop, frame, runto <addr>, break <addr> [cond], bpr/bpw <addr>[-<end>]
 * [cond], bpin/bpout <port>[-<end>] [cond], delete <id|addr>, enable/disable
 * <id>, clear, breaks, regs, mem <addr> [len], poke <addr> <val...>,
 * disasm [addr] [n], label <addr> <name>, labels [file]. Anything else is
 * a Tcl command for openMSX's own console ("debug list", "reg A 5",
 * "slotmap", ...). Addresses: $C000, 0xC000, C000h, decimal or a label.
 * Conditions are Tcl expressions ("[reg A] == 5"; no braces). Output text
 * into dst; returns its length. */
int  msxdebug_command(msxdebug *d, const char *command, char *dst, int dstsz);
/* Completions for the end of a line (commands, registers, labels, and what
 * openMSX itself completes), one per line. Returns the count. */
int  msxdebug_completions(msxdebug *d, const char *prefix, char *dst, int dstsz);

/* ---- stepping shortcuts (the toolbar) -------------------------------------- */
void msxdebug_step(msxdebug *d);          /* F7: one instruction */
void msxdebug_step_over(msxdebug *d);     /* F8: over a CALL/HALT/block op */
void msxdebug_step_out(msxdebug *d);      /* Shift+F8: to the RET */
/* Run to the next VDP interrupt (the vertical blank: execution reaching
 * RST 38h), the frame boundary an MSX program sees. */
void msxdebug_frame(msxdebug *d);
/* Run until PC == addr. */
void msxdebug_run_to(msxdebug *d, uint16_t addr);

/* ---- CPU --------------------------------------------------------------------- */
typedef struct {
    int pc, sp, af, bc, de, hl, ix, iy;
    int af2, bc2, de2, hl2;
    int i, r, im, iff1, iff2;
    int sf, zf, yf, hf, xf, pf, nf, cf;    /* the flags in F */
} msxdebug_cpu;
void msxdebug_cpu_get(msxdebug *d, msxdebug_cpu *out);
typedef enum {
    MSX_REG_PC = 0, MSX_REG_SP, MSX_REG_AF, MSX_REG_BC, MSX_REG_DE, MSX_REG_HL,
    MSX_REG_IX, MSX_REG_IY, MSX_REG_AF2, MSX_REG_BC2, MSX_REG_DE2, MSX_REG_HL2,
    MSX_REG_I, MSX_REG_R, MSX_REG_IM, MSX_REG_IFF1, MSX_REG_IFF2,
    MSX_REG_A, MSX_REG_F, MSX_REG_B, MSX_REG_C, MSX_REG_D, MSX_REG_E,
    MSX_REG_H, MSX_REG_L,
    MSX_FLAG_S, MSX_FLAG_Z, MSX_FLAG_H, MSX_FLAG_PV, MSX_FLAG_N, MSX_FLAG_C,
    MSX_REG_COUNT
} msxdebug_reg;
void msxdebug_cpu_set(msxdebug *d, int reg, int value);

/* ---- memory ------------------------------------------------------------------- */
/* CPU-bus reads through the live slot map, without side effects. */
int  msxdebug_read(msxdebug *d, uint16_t addr, uint8_t *dst, int n);
/* A debugger edit through the same map (RAM takes it; ROM ignores it). */
void msxdebug_write(msxdebug *d, uint16_t addr, uint8_t value);

/* ---- slots and I/O ------------------------------------------------------------ */
/* The machine's slot selection and map, the PSG registers, the memory
 * mapper and the PPI slot register, as text lines. Returns length. */
int  msxdebug_io_state(msxdebug *d, char *dst, int dstsz);

/* ---- disassembly -------------------------------------------------------------- */
typedef struct {
    uint16_t address;
    int is_pc;
    int has_breakpoint;      /* an execute breakpoint here, enabled or not */
    int breakpoint_enabled;
    int length;
    char bytes[16];
    char label[48];
    char disasm[64];
    char comment[64];        /* a jump/call target's label */
} msxdebug_line;
/* Up to `max` lines starting at `addr`. Returns the count; *pc_line is the
 * index of the PC's line in `out`, -1 when not among them. */
int  msxdebug_disassemble(msxdebug *d, uint16_t addr, msxdebug_line *out,
                          int max, int *pc_line);
/* The address `rows` disassembly rows before (negative) or after `addr` --
 * for scrolling, and for "Follow PC" (PC a third of the way down).
 * Backwards is a best guess, as on every Z80 debugger. */
int  msxdebug_row_address(msxdebug *d, uint16_t addr, int rows);

/* ---- symbols ---------------------------------------------------------------- */
/* .sym format: "HHHH NAME [; comment]" per line, '#' comments. A small
 * built-in MSX BIOS table (core/debugger/symbols_builtin.c) loads when the
 * engine is created. Message into msg; returns labels loaded or -1. */
int  msxdebug_load_symbols(msxdebug *d, const char *path, char *msg, int msgsz);
/* Address of a label, or -1. Label of an address into dst (may be empty). */
int  msxdebug_label_address(msxdebug *d, const char *label);
int  msxdebug_address_label(msxdebug *d, uint16_t addr, char *dst, int dstsz);
int  msxdebug_set_label(msxdebug *d, uint16_t addr, const char *label);
const char *msxdebug_symbol_at(msxdebug *d, uint16_t addr, uint16_t *offset);
int  msxdebug_symbol_find(msxdebug *d, const char *name, uint16_t *addr);

/* ---- breakpoints -------------------------------------------------------------- */
typedef enum {
    MSXDEBUG_BP_EXEC = 1, MSXDEBUG_BP_READ = 2, MSXDEBUG_BP_WRITE = 4,
    MSXDEBUG_BP_IN = 8, MSXDEBUG_BP_OUT = 16
} msxdebug_bp_type;
typedef struct {
    int id;
    int type;                /* msxdebug_bp_type bit */
    uint16_t start, end;     /* a port range for IN/OUT (0-255) */
    int enabled;
    unsigned hits;
    char condition[128];     /* a Tcl expression, "" for none */
} msxdebug_breakpoint;
/* Execute breakpoint at addr: added if absent, removed if present. Returns
 * 1 when one is now set. */
int  msxdebug_breakpoint_toggle(msxdebug *d, uint16_t addr);
/* 1 when an execute breakpoint is at addr (enabled or not). */
int  msxdebug_breakpoint_check(msxdebug *d, uint16_t addr);
/* Returns the new id, or -1 with the reason in err (a condition openMSX
 * refuses, a bad range). */
int  msxdebug_breakpoint_add(msxdebug *d, int type, uint16_t start, uint16_t end,
                             const char *condition, char *err, int errsz);
void msxdebug_breakpoint_remove(msxdebug *d, int id);
void msxdebug_breakpoint_enable(msxdebug *d, int id, int enabled);
int  msxdebug_breakpoint_list(msxdebug *d, msxdebug_breakpoint *out, int max);
void msxdebug_breakpoint_clear(msxdebug *d);

/* ---- FujiNet: the cartridge's socket, as FujiBus packets ------------------------ */
/* Always on from msxsession_start, debugger open or not. The patched
 * openMSX FujiNet cartridge (src/serial/FujiNet.cc, see
 * tools/openmsx/patches/patch-staged-tree.py) hands every byte it sends or
 * receives to core/debugger/fujitrace.c, which SLIP-decodes them. The last
 * 512 packets are kept; identical exchanges in a row fold into one entry.
 * DBC packets (device FF) are the cartridge's own: FujiNet pushing a ROM
 * into it, and its acks. The MSX never sees them. */
typedef struct {
    uint64_t serial;
    int      to_host;       /* 1: FujiNet -> MSX (or the cartridge), 0: -> FujiNet */
    int      device, command, length;   /* from the FujiBus header, -1 if short */
    int      nbytes;
    uint8_t  data[64];      /* the decoded packet's first bytes */
    char     text[96];      /* "MSX -> FujiNet 70 E8 GET ADAPTERCONFIG (7 bytes)" */
    uint32_t repeat;        /* N when N identical exchanges in a row folded into this one */
} msx_fuji_packet;
int  msxdebug_fujinet_trace(msxdebug *d, msx_fuji_packet *out, int max);  /* newest max, oldest first */
void msxdebug_fujinet_trace_clear(msxdebug *d);
/* The cartridge's link and byte/packet counts, as text lines. */
int  msxdebug_fujinet_state(msxdebug *d, char *dst, int dstsz);

/* ---- files -------------------------------------------------------------------- */
/* Save with the path a native file picker supplied. kind: "dis" (the
 * disassembly of $0000-$FFFF as mapped now), "mem" (the 64K the CPU sees),
 * "vram" (the VDP's VRAM), "regs" (CPU registers as text). Message into
 * msg; returns 0 or -1. */
int  msxdebug_save(msxdebug *d, const char *kind, const char *path,
                   char *msg, int msgsz);

/* ---- hooks the session calls --------------------------------------------------- */
/* The machine (re)started, was switched or was reset: breakpoints went with
 * the old machine, so an attached engine re-arms and stops the new one. */
void msxdebug_machine_changed(msxdebug *d, int power_cycled);

/* ---- tests ---------------------------------------------------------------------- */
typedef enum {
    MSXDBG_STOP_PAUSE = 0, MSXDBG_STOP_BREAKPOINT, MSXDBG_STOP_STEP,
    MSXDBG_STOP_RUNTO
} msxdebug_stop_reason_kind;
/* Optional push for tests: fires from the engine's poll thread when it
 * notices a stop it did not just ask for. */
void msxdebug_set_stop_callback(msxdebug *d,
                                void (*cb)(void *ud, int reason, uint16_t pc),
                                void *ud);

#ifdef __cplusplus
}
#endif

#endif /* MSXDEBUG_H */
