/*
 * msxdebug -- the toolkit-agnostic debugger engine for FujiNet Go MSX
 * Desktop. Unlike fujinet-go-adam-desktop's adamdebug (built on adamcore's
 * own per-instruction exec hook -- adamcore is a small emulator this
 * project's own code drives frame-by-frame), this engine is built entirely
 * on openMSX's own Tcl debug interface via core/openmsx/msx_host.hh's
 * msxhost_execute_sync()/msxhost_execute_sync_binary(): "debug break"/
 * "step"/"cont" (which openMSX's Debugger.cc forwards straight to
 * MSXCPUInterface::doBreak/doStep/doContinue), "debug set_bp"/"remove_bp",
 * and "debug read_block <debuggable> ..." for register/VDP/memory
 * snapshots. There is no exec hook of our own and no per-instruction trace
 * ring: openMSX runs autonomously on its own thread at full speed, and a
 * Tcl round-trip per instruction would be many orders of magnitude too
 * slow to keep up with a 3.5MHz Z80 -- see msxdebug_step_over/_out below
 * for how step-over/step-out are built instead (a conditional one-shot
 * breakpoint, since openMSX has no native step-over).
 *
 * Threading: control calls (pause/resume/step/breakpoints) are safe from
 * the UI thread and block until openMSX has confirmed the new state (they
 * are themselves built on the already-synchronous msxhost_execute_sync).
 * The stop callback fires from a small dedicated poll thread this engine
 * owns (see debugger.c) whenever it observes execution stop for a reason
 * the caller didn't just synchronously request itself (i.e. an autonomous
 * breakpoint hit while free-running) -- marshal to the UI thread the same
 * way the frame-ready notification already is (g_idle_add /
 * QMetaObject::invokeMethod). State reads are consistent while paused.
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

typedef enum {
    MSXDBG_STOP_PAUSE = 0,   /* explicit pause request */
    MSXDBG_STOP_BREAKPOINT,  /* a persistent breakpoint fired */
    MSXDBG_STOP_STEP,        /* step into/over/out completed */
    MSXDBG_STOP_RUNTO,       /* run-to-address reached */
} msxdebug_stop_reason;

/* ---- execution control -----------------------------------------------
 * pause/step_into/step_over/step_out all block until openMSX has confirmed
 * the resulting state (so msxdebug_get_regs() right after any of these is
 * guaranteed current). resume/run_to do not block -- they hand control
 * back to openMSX and the eventual stop (if any) is delivered later via
 * the stop callback. */
void msxdebug_pause(msxdebug *d);   /* halts at the next instruction boundary */
void msxdebug_resume(msxdebug *d);
int  msxdebug_is_paused(msxdebug *d);
void msxdebug_step_into(msxdebug *d);          /* one instruction; while paused */
void msxdebug_step_over(msxdebug *d);          /* while paused */
void msxdebug_step_out(msxdebug *d);           /* while paused */
void msxdebug_run_to(msxdebug *d, uint16_t addr);

/* Fires from the engine's own poll thread (NOT the UI thread, NOT the
 * openMSX thread) whenever execution stops for a reason the caller did not
 * just synchronously request via one of the calls above -- i.e. a
 * persistent breakpoint fired, or a step_over/step_out/run_to's transient
 * one-shot breakpoint fired, while the caller had already returned. */
void msxdebug_set_stop_callback(msxdebug *d,
                                void (*cb)(void *ud,
                                           msxdebug_stop_reason reason,
                                           uint16_t pc),
                                void *ud);

/* ---- breakpoints -------------------------------------------------------
 * Persistent, PC-only (openMSX also supports watchpoints/conditions via
 * "debug set_bp", but this engine's UI surface -- click a disassembly line
 * to toggle -- only needs the plain case). */
void msxdebug_bp_toggle(msxdebug *d, uint16_t addr);
void msxdebug_bp_set(msxdebug *d, uint16_t addr);
void msxdebug_bp_clear(msxdebug *d, uint16_t addr);
void msxdebug_bp_clear_all(msxdebug *d);
int  msxdebug_bp_is_set(msxdebug *d, uint16_t addr);
int  msxdebug_bp_list(msxdebug *d, uint16_t *out, int max);

/* ---- CPU / memory (consistent while paused) -----------------------------
 * Byte layout matches openMSX's own "CPU regs" debuggable exactly (see
 * src/cpu/MSXCPUInterface.cc's RegDebug) rather than inventing a different
 * one -- msxdebug_get_regs()/_set_regs() read/write it via debug
 * read_block/write. */
typedef struct {
    uint8_t a, f, b, c, d, e, h, l;
    uint8_t a2, f2, b2, c2, d2, e2, h2, l2;
    uint8_t ixh, ixl, iyh, iyl;
    uint8_t pch, pcl, sph, spl;
    uint8_t i, r, im, iff;
} msxdebug_z80_regs;

void msxdebug_get_regs(msxdebug *d, msxdebug_z80_regs *out);
void msxdebug_set_regs(msxdebug *d, const msxdebug_z80_regs *in);
int  msxdebug_read_mem(msxdebug *d, uint16_t addr, uint8_t *dst, int n);
int  msxdebug_write_mem(msxdebug *d, uint16_t addr, const uint8_t *src,
                        int n);

/* ---- disassembly ---------------------------------------------------------
 * Same flag bits and line shape as adam-desktop's adamdasm_line -- only
 * the prefix changed, so a frontend ported from there needs no logic
 * changes here. */
#define MSXDASM_JUMP     0x01
#define MSXDASM_CALL     0x02
#define MSXDASM_RET      0x04
#define MSXDASM_COND     0x08
#define MSXDASM_RELATIVE 0x10
#define MSXDASM_BLOCK    0x20 /* LDIR/CPIR/INIR/OTIR family */
#define MSXDASM_HALT     0x40

typedef struct {
    uint16_t addr;
    uint8_t len;         /* 1..4 */
    uint8_t bytes[4];
    char text[32];        /* "LD (IX+5),A" */
    uint16_t target;      /* jump/call destination when flags say so */
    uint8_t flags;        /* MSXDASM_* */
    const char *symbol;   /* label at addr from the symbol tables, or NULL */
} msxdasm_line;

/* Disassembles count instructions starting at addr, reading through the
 * live CPU memory map (side-effect free). Returns lines written. */
int msxdebug_disassemble(msxdebug *d, uint16_t addr, int count,
                         msxdasm_line *out);

/* ---- symbols ---------------------------------------------------------
 * .sym format: "HHHH NAME [; comment]" per line, '#' comments. A small
 * built-in MSX BIOS table (see core/debugger/symbols_builtin.c for what it
 * covers and why it is deliberately conservative) loads automatically when
 * the engine is created. */
int msxdebug_symbols_load(msxdebug *d, const char *path, const char *table);
const char *msxdebug_symbol_at(msxdebug *d, uint16_t addr, uint16_t *offset);
int msxdebug_symbol_find(msxdebug *d, const char *name, uint16_t *addr);

#ifdef __cplusplus
}
#endif

#endif /* MSXDEBUG_H */
