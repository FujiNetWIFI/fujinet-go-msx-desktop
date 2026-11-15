/*
 * msxdebug -- the debugger contract for FujiNet Go MSX Desktop: a Z80
 * debugger engine behind a C API the four native debugger windows share.
 * Same shape as the SMS sibling's smsdebug.h (the family's newest Z80
 * debugger), so the windows carry over; the engine is this repository's own
 * and is built entirely on openMSX's Tcl debug interface via
 * core/openmsx/msx_host.hh's msxhost_execute_sync()/_binary(): "debug break"
 * /"step"/"cont", "debug set_bp"/"set_watchpoint", "debug read_block <
 * debuggable> ...", "reg", "slotmap". There is no exec hook of our own:
 * openMSX runs autonomously on its own thread at full speed, and a Tcl
 * round-trip per instruction would be many orders of magnitude too slow for a
 * 3.5 MHz Z80. Step-over/step-out/frame are therefore one-shot breakpoints,
 * and each persistent breakpoint carries a small Tcl action that counts its
 * hits and says which one fired (and, for a watchpoint, what was touched).
 * The Trace and History tabs read the ring the patched CPU core fills
 * (msx_host.hh, msxhost_trace_*), turned on only while one of them is open.
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
 * run, stop, frame, runto <addr>, break <addr> [cond], bpr/bpw/bpa
 * <addr>[-<end>] [cond] (read / write / read-or-write), bpin/bpout
 * <port>[-<end>] [cond], delete <id|addr>, enable/disable <id>, clear, hits
 * [clear], breaks, print <expr>, regs, set <reg> <expr>, mem <addr> [len],
 * poke <addr> <val...>, disasm [addr] [n], trace [on|off|clear|n], watch
 * [expr|lo-hi], unwatch <n|all>, find [hex] <bytes> | find text <string>,
 * symbols [text], label <addr> <name>, labels [file], map, io, cart, sound,
 * video [section], save <dis|mem|vram|regs> <file>. Anything else is a Tcl
 * command for openMSX's own console ("debug list", "reg A 5", "slotmap",
 * "ext scc", ...). Addresses and values are expressions (see msxdebug_eval);
 * a condition is one too, or an openMSX Tcl expression ("[reg A] == 5").
 * Output text into dst; returns its length. */
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
    int halted;                            /* in HALT: known only while the trace ring is on, else 0 */
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
    MSXDEBUG_BP_IN = 8, MSXDEBUG_BP_OUT = 16,
    /* Read and write as one entry ("Access"): READ | WRITE, armed as a read
     * and a write watchpoint that share one id and one hit count. */
    MSXDEBUG_BP_ACCESS = 6
} msxdebug_bp_type;
typedef struct {
    int id;
    int type;                /* msxdebug_bp_type bit */
    uint16_t start, end;     /* a port range for IN/OUT (0-255) */
    int enabled;
    unsigned hits;
    char condition[128];     /* "" for none; an expression (see msxdebug_eval) or a Tcl one */
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

/* ---- expressions ----------------------------------------------------------------
 * The one grammar for conditions, print, set, watches and every address the
 * prompt takes: numbers ($C000 0xC000 C000h 49152 %1010), the Z80 registers
 * (a f b c d e h l af bc de hl ix iy sp pc i r im iff1 iff2 af' bc' de' hl'
 * ixh ixl iyh iyl), the flags (cf nf pf xf hf yf zf sf), labels, [addr] the
 * byte and {addr} the word at addr (through the slot map the CPU sees, no
 * side effects), frame (VDP frames), cycles (Z80 clocks), vline (the VDP line
 * in the frame) and, for an access, value, addr and old; C's operators with
 * C's precedence. In a breakpoint condition `value`/`addr`/`old` name the
 * byte the access moved (old = the byte a write replaced, for a read the
 * value itself). A condition that is not this grammar is taken as an openMSX
 * Tcl expression ("[reg A] == 5") exactly as before; either way it is tried
 * once when added, and refused with openMSX's reason if it fails.
 * 0 and *out, or -1 and a message. Registers and [addr] read the machine as
 * it is now; with the machine stopped at a watchpoint, value/addr/old name
 * that access. */
int  msxdebug_eval(msxdebug *dbg, const char *text, long *out, char *err, int errsz);

/* Register names the grammar knows, NULL-terminated, for completion. */
extern const char *const msxdebug_reg_names[];

/* ---- stops, in words ----------------------------------------------------------
 * Why the machine last stopped, worded by the core so every window agrees:
 * "paused at $4010", "step at $4010", "frame at $0038", "stopped at $4013" (a
 * one-shot from Over/Out/Run to), "breakpoint: exec $4010",
 * "wrote $C000 = $12 (was $11) at $4A3C", "read $C000 = $12 at $4A3C",
 * "in $A8 at $4A3C", "out $98 = $12 at $4A3C". While running it says
 * "running". *address gets the PC to show, -1 when running. A watchpoint
 * stop is reported at the instruction AFTER the access (the CPU breaks when
 * it finishes). Returns the length. */
int  msxdebug_stop_text(msxdebug *dbg, char *dst, int dstsz, int *address);

enum {
    MSXDEBUG_STOP_NONE = 0,
    MSXDEBUG_STOP_PAUSE,
    MSXDEBUG_STOP_STEP,
    MSXDEBUG_STOP_BREAKPOINT,   /* an EXEC entry */
    MSXDEBUG_STOP_WATCH_READ,
    MSXDEBUG_STOP_WATCH_WRITE,
    MSXDEBUG_STOP_WATCH_IN,
    MSXDEBUG_STOP_WATCH_OUT,
    MSXDEBUG_STOP_ONESHOT,      /* step over / out / run to */
    MSXDEBUG_STOP_FRAME
};

typedef struct {
    int      reason;        /* MSXDEBUG_STOP_* */
    int      bp_id;         /* the entry that fired, 0 if none */
    uint16_t addr;          /* the byte or port touched (watch), or the PC */
    int      value;         /* the byte moved, -1 when openMSX does not say (an IN) */
    int      old_value;     /* the byte a write replaced, -1 when not known */
    uint16_t pc;            /* the PC at the stop */
    int      from_pc;       /* the instruction that made the access, -1 unless the trace ring is on */
    uint64_t serial;        /* msxdebug_stop_serial at the stop */
} msxdebug_stop_info;
int  msxdebug_last_stop(msxdebug *dbg, msxdebug_stop_info *out);
/* Bumped by every stop (pause, breakpoint, step); frontends recentre their
 * views on the PC when it changes. */
uint64_t msxdebug_stop_serial(msxdebug *dbg);

/* Everything about the current stop read in one critical section, so a
 * poller never sees a stop with the old serial or the old reason: paused,
 * the serial, the stop info and its text agree. Returns paused. */
typedef struct {
    int      paused;
    uint64_t serial;
    msxdebug_stop_info stop;
    char     text[160];
    int      address;       /* the PC to show, -1 when running */
} msxdebug_snapshot;
int  msxdebug_stop_snapshot(msxdebug *dbg, msxdebug_snapshot *out);

/* ---- registers, in bulk ------------------------------------------------------
 * Writes every register of `regs` in one go (pc sp af bc de hl ix iy the
 * shadows i r im iff1 iff2; the flag fields are the bits of af and are
 * ignored, and HALT cannot be written). Only while stopped: returns 1, or 0
 * with a reason in err (running, IM above 2, no machine). */
int  msxdebug_regs_set(msxdebug *dbg, const msxdebug_cpu *regs, char *err, int errsz);

/* ---- breakpoint hit counts ----------------------------------------------------- */
void msxdebug_breakpoint_clear_hits(msxdebug *dbg);

/* ---- symbols, for a Symbols tab -------------------------------------------------
 * The built-in BIOS names, loaded files and user labels as one list. */
int  msxdebug_symbols_count(msxdebug *dbg);
/* Entry `index` (0 .. count-1). Returns 1, or 0 past the end. The order is
 * the tables'; it can change when symbols are loaded or cleared. */
int  msxdebug_symbols_get(msxdebug *dbg, int index, uint16_t *addr, char *name, int namesz);
/* Indices of the entries whose name contains `text` (case-insensitive; empty
 * matches all), ordered by address then name. Returns the count written. */
int  msxdebug_symbols_filter(msxdebug *dbg, const char *text, int *out, int max);
/* Drop every loaded file and user label; the built-ins stay. */
void msxdebug_clear_symbols(msxdebug *dbg);

/* ---- memory search and the memory map -------------------------------------------- */
#define MSXDEBUG_SEARCH_NOCASE 1
/* Finds up to max start addresses in from..to of the CPU's 64K as mapped now,
 * where (mem & mask) == (pat & mask) for all n bytes. Returns the count, or
 * -1 while the machine is running or on an error. */
int  msxdebug_search(msxdebug *dbg, const uint8_t *pat, int count, uint8_t mask, int flags,
                     uint16_t from, uint16_t to, uint16_t *out, int max);
/* The next match after `from` (before it if backward), wrapping once.
 * Returns 1 and *found, 0 for none, -1 while running. */
int  msxdebug_search_next(msxdebug *dbg, const uint8_t *pat, int count, uint8_t mask, int flags,
                          uint16_t from, int backward, uint16_t *found);

typedef struct {
    uint16_t lo, hi;        /* inclusive */
    int      readable, writable;
    char     name[48];      /* "Main RAM (slot 3-2)", "empty (slot 2)", "sub-slot register (slot 3)" */
} msxdebug_region;
/* Ordered and gap-free from $0000 to $FFFF as the slots are selected now: one
 * region per 16K page (adjacent pages of the same device in the same slot are
 * joined), the expanded slot's register at $FFFF split out. Returns how many
 * (up to max; 4 to 8). */
int  msxdebug_memory_map(msxdebug *dbg, msxdebug_region *out, int max);

/* ---- the trace ring: Trace and History ------------------------------------------
 * One record per instruction executed since it was enabled, in the ring the
 * patched CPU core fills (msx_host.hh): the registers are the state AFTER the
 * instruction at pc ran. Enabled only while a tab needs it (it costs the
 * CPU's slow path, the same as armed breakpoints); the engine re-enables it
 * itself after a machine switch or reset. A breakpoint stop never records an
 * instruction that did not run. The ring keeps the newest 65536. Safe while
 * running. */
typedef struct {
    uint16_t pc, sp, af, bc, de, hl, ix, iy;
    uint16_t af2, bc2, de2, hl2;
    uint8_t  reg_i, reg_r, im;
    uint8_t  iff;           /* bit 0 IFF1, bit 1 IFF2 */
    uint8_t  halted;        /* in HALT after it */
    uint8_t  len;           /* the instruction's length, 1-4 */
    uint8_t  bytes[4];      /* the bytes at pc */
    uint64_t cycles;        /* Z80 clocks since the machine started */
} msxdebug_trace;
#define MSXDEBUG_TRACE_SIZE 65536
void msxdebug_trace_enable(msxdebug *dbg, int on);
int  msxdebug_trace_enabled(msxdebug *dbg);
/* Newest first; returns entries copied. */
int  msxdebug_trace_read(msxdebug *dbg, msxdebug_trace *out, int max);
void msxdebug_trace_clear(msxdebug *dbg);
/* Entries held. */
int  msxdebug_trace_count(msxdebug *dbg);
/* Records pushed since the last clear: a tab polls this to see new lines. */
uint64_t msxdebug_trace_total(msxdebug *dbg);
/* "0006  IN A,($DC)  AF=.... BC=.... DE=.... HL=.... SP=....". Returns the length. */
int  msxdebug_trace_format(const msxdebug_trace *rec, char *dst, int dstsz);
enum { MSXDEBUG_TRACE_ALL = 0, MSXDEBUG_TRACE_BRANCHES, MSXDEBUG_TRACE_RANGE };
/* 1 if the entry passes the filter (BRANCHES: jumps, calls, returns;
 * RANGE: pc in lo..hi). */
int  msxdebug_trace_match(const msxdebug_trace *rec, int filter, uint16_t lo, uint16_t hi);

/* The same ring as the History tab: what ran before the machine stopped, while
 * stopped. Same enable (the ring is on while either tab wants it), same
 * entries; the differences are the order (oldest first) and the format. */
typedef msxdebug_trace msxdebug_hist;
void msxdebug_history_enable(msxdebug *dbg, int on);
int  msxdebug_history_enabled(msxdebug *dbg);
void msxdebug_history_clear(msxdebug *dbg);
uint64_t msxdebug_history_count(msxdebug *dbg);
/* Up to max of the newest entries, oldest first; returns how many. */
int  msxdebug_history_copy(msxdebug *dbg, msxdebug_hist *out, int max);
/* "50CD  CALL $50D5   AF=.... BC=.... DE=.... HL=.... IX=.... IY=.... SP=....  SZ.H.PNC". */
int  msxdebug_history_format(const msxdebug_hist *rec, char *dst, int dstsz);
int  msxdebug_history_match(const msxdebug_hist *rec, int filter, uint16_t lo, uint16_t hi);

/* ---- the machine: Cart & I/O ------------------------------------------------------
 * Read through openMSX's debuggables; valid while stopped, a stale value
 * while running is only a stale display. Everything absent is zero / -1. */
typedef struct {
    int      valid;                 /* the machine answered */
    char     machine[64];           /* the machine id ("C-BIOS_MSX2") */
    char     type[16];              /* "MSX", "MSX2", "MSX2+", "MSXturboR" */
    int      page_slot[4];          /* the primary slot selected for each 16K CPU page */
    int      page_subslot[4];       /* its secondary slot, -1 when that slot is not expanded */
    int      expanded[4];           /* the primary slot is expanded */
    int      subslot_reg[4];        /* each expanded slot's secondary-slot register ($FFFF), only
                                     * readable for the slot on page 3: -1 for the others */
    char     page_device[4][48];    /* what answers in each page ("" when nothing does) */
    uint8_t  ppi[4];                /* $A8 slot select, $A9 keyboard column, $AA row/misc, $AB control */
    int      mapper;                /* a memory mapper answers on $FC-$FF */
    uint8_t  mapper_reg[4];         /* the segment behind each page */
    uint8_t  keymatrix[16];         /* the keyboard rows as the PPI would read them (0 = pressed) */
    int      keymatrix_rows;        /* how many rows this machine has (11, 9 on some) */
    uint8_t  joyport[2];            /* the joystick ports' pin states (bit set = not pressed) */
    int      rom_mappers;           /* entries used in rom[] */
    struct {
        char    device[48];
        uint8_t block[8];           /* the block mapped at $0000 $2000 ... $E000, 255 = none */
    } rom[6];
    char     slotmap[1536];         /* openMSX's own "slotmap" text */
} msxdebug_io;
int  msxdebug_io_get(msxdebug *dbg, msxdebug_io *out);

typedef struct {
    char     machine[64];
    char     type[16];
    int      z80_hz;                /* the CPU clock */
    int      fujinet;               /* the FujiNet cartridge is in a slot */
    int      fujinet_link;          /* its socket is connected */
    int      fujinet_external;      /* it dials an external FujiNet */
    int      scc, music, audio;        /* an SCC, MSX-MUSIC, MSX-AUDIO chip answers */
    int      mapper;                /* a memory mapper */
    char     devices[512];          /* "C-BIOS Main ROM, Main RAM, ppi, VDP, PSG, ..." */
} msxdebug_cart;
void msxdebug_cart_info(msxdebug *dbg, msxdebug_cart *out);
/* One line: "C-BIOS_MSX2 (MSX2), FujiNet cartridge (link up), SCC, MSX-AUDIO". */
int  msxdebug_cart_summary(msxdebug *dbg, char *dst, int dstsz);

/* ---- Sound: the PSG, and the SCC, MSX-MUSIC and MSX-AUDIO chips the machine has.
 * Found with `debug list` (only the ones present); valid while stopped. */
typedef struct {
    int      present;
    uint8_t  reg[16];       /* the registers; R7 mixer is active-low; R14/R15 the joystick ports */
    int      period[3];     /* tone periods A-C, 12 bits */
    double   freq[3];         /* the tone each makes: 111860.8 / period */
    int      tone_on[3], noise_on[3];   /* decoded from the mixer */
    int      level[3];      /* 0-15 */
    int      envelope[3];   /* the voice follows the envelope instead of its level */
    int      noise_period;  /* 5 bits */
    int      env_period;    /* 16 bits */
    int      env_shape;     /* R13, low 4 bits */
} msxdebug_psg;

typedef struct {
    int      present;
    char     name[64];      /* openMSX's device name */
    uint8_t  reg[256];      /* SCC+ format: waves 0-$9F, frequencies $A0-$A9, volumes $AA-$AE, enables $AF */
    uint8_t  wave[5][32];
    int      period[5];     /* 12 bits */
    double   freq[5];
    int      volume[5];     /* 0-15 */
    int      enabled[5];
} msxdebug_scc;

typedef struct {
    int      present;
    char     name[64];
    uint8_t  reg[0x40];
    int      fnum[9], block[9], key_on[9], instrument[9], volume[9];
    double   freq[9];
    int      rhythm;        /* R0E bit 5: percussion mode */
} msxdebug_opll;

typedef struct {
    int      present;
    char     name[64];
    uint8_t  reg[256];
} msxdebug_y8950;

typedef struct {
    msxdebug_psg psg;
    msxdebug_scc scc;
    msxdebug_opll opll;
    msxdebug_y8950 y8950;
} msxdebug_sound;
int  msxdebug_sound_get(msxdebug *dbg, msxdebug_sound *out);

/* ---- the Video tab's text for the console `video` command -------------------------
 * fn gets a section name (or "") and returns text that stays valid until its
 * next call, NULL when there is no frame yet. Set by whoever owns the Video
 * controller. */
void msxdebug_set_video_text(const char *(*callback)(const char *section));


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
