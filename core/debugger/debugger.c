/*
 * msxdebug engine: attach/detach, execution control (stop, step into/over/
 * out, frame, run-to), breakpoints and watchpoints with conditions and hit
 * counts, the console, disassembly and symbols, all built on openMSX's own
 * Tcl debug interface via core/openmsx/msx_host.hh's msxhost_execute_sync/
 * _binary. See msxdebug.h's header comment for the contract and why this
 * engine's shape differs from the siblings' (no exec hook, no trace ring --
 * openMSX owns its execution loop on its own thread).
 *
 * Breakpoints live in our own table, and are armed in openMSX only while a
 * window is attached. Each armed one carries a Tcl action,
 *
 *     incr ::fngo_hit(ID); set ::fngo_last ID; debug break
 *
 * which counts the hit and tells the poll thread which one stopped the
 * machine. The one-shot breakpoints that step over/out, frame and run-to
 * arm use openMSX's own default action, so a stop with no ::fngo_last is
 * one of those.
 *
 * Two ways a stop is discovered:
 *   1. Synchronously, when THIS thread just asked for one (stop, step) --
 *      the control function blocks for the RPC and the state is current when
 *      it returns.
 *   2. Asynchronously, when a breakpoint fires on its own while running
 *      free -- a small poll thread this engine owns notices via periodic
 *      "debug breaked" checks (openMSX has no push notification for this
 *      over the Tcl interface).
 * Either way the generation counter is bumped, which is how a window knows
 * to refresh.
 *
 * d->mtx guards all of the above; msxhost_execute_sync has its own
 * independent mutex, so holding d->mtx across an RPC is safe and used
 * freely to keep control operations atomic with respect to the poll thread.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../openmsx/msx_host.hh"
#include "../src/session_internal.h"
#include "fujitrace.h"
#include "msxdebug.h"
#include "symbols.h"
#include "z80dasm.h"

#define MSXDBG_MAX_BP 128
#define MSXDBG_POLL_INTERVAL_MS 40
#define VBLANK_VECTOR 0x0038

typedef struct {
    int id;                    /* ours, stable */
    int type;                  /* msxdebug_bp_type */
    uint16_t start, end;
    char cond[128];
    int enabled;
    char oid[16];              /* openMSX's id ("bp#1", "wp#2"); "" when not armed */
} bp_entry;

struct msxdebug {
    msxsession *session;
    pthread_mutex_t mtx;
    pthread_t poll_thread;
    int poll_thread_running;
    int poll_stop;

    int attached;
    int started;                       /* the machine has been brought up */
    int paused;                        /* our own last-known state */
    unsigned gen;
    char reason[128];
    int reason_addr;

    /* What the NEXT autonomous stop with no ::fngo_last means. */
    const char *pending_reason;
    int pending_addr;

    void (*stop_cb)(void *ud, int reason, uint16_t pc);
    void *stop_ud;

    bp_entry bps[MSXDBG_MAX_BP];
    int bp_count;
    int next_id;

    /* The one transient one-shot breakpoint a pending step_over/step_out/
     * frame/run_to may have live; "" when none is outstanding. */
    char transient_id[16];

    sym_tables syms;
    char *user_syms;                   /* "HHHH name\n" lines from set_label */
};

/* ---- small helpers --------------------------------------------------- */

static int rpc(const char *cmd, char *out, int max)
{
    if (out && max > 0) out[0] = '\0';
    return msxhost_execute_sync(cmd, out, max);
}

static int rpcf(char *out, int max, const char *fmt, ...)
{
    char cmd[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    return rpc(cmd, out, max);
}

static uint16_t read_pc_now(void)
{
    char buf[16];
    if (!rpc("reg PC", buf, sizeof(buf))) return 0;
    return (uint16_t)strtol(buf, NULL, 10);
}

static bp_entry *bp_by_id(msxdebug *d, int id)
{
    int i;
    for (i = 0; i < d->bp_count; i++)
        if (d->bps[i].id == id) return &d->bps[i];
    return NULL;
}

static bp_entry *bp_exec_at(msxdebug *d, uint16_t addr)
{
    int i;
    for (i = 0; i < d->bp_count; i++)
        if (d->bps[i].type == MSXDEBUG_BP_EXEC && d->bps[i].start == addr)
            return &d->bps[i];
    return NULL;
}

static void note_stop_locked(msxdebug *d, const char *reason, int addr)
{
    snprintf(d->reason, sizeof(d->reason), "%s", reason);
    d->reason_addr = addr;
    d->gen++;
}

/* Removes any outstanding transient one-shot bp; safe whether or not it
 * fired already (a fired -once bp has removed itself on openMSX's side, so
 * this simply fails harmlessly). Caller holds d->mtx. */
static void remove_transient_locked(msxdebug *d)
{
    if (!d->transient_id[0]) return;
    rpcf(NULL, 0, "debug remove_bp %s", d->transient_id);
    d->transient_id[0] = '\0';
}

/* ---- arming ------------------------------------------------------------ */

static const char *type_word(int type)
{
    switch (type) {
    case MSXDEBUG_BP_READ:  return "read_mem";
    case MSXDEBUG_BP_WRITE: return "write_mem";
    case MSXDEBUG_BP_IN:    return "read_io";
    case MSXDEBUG_BP_OUT:   return "write_io";
    default:                return "";
    }
}

/* Arms one entry in openMSX; on failure the reason goes to err. Caller
 * holds d->mtx. */
static int arm_locked(msxdebug *d, bp_entry *b, char *err, int errsz)
{
    char cmd[640], res[160], action[96];
    int ok;

    (void)d;
    if (b->oid[0]) return 1;
    snprintf(action, sizeof(action),
             "incr ::fngo_hit(%d); set ::fngo_last %d; debug break", b->id, b->id);
    if (b->type == MSXDEBUG_BP_EXEC) {
        snprintf(cmd, sizeof(cmd), "debug set_bp 0x%04X {%s} {%s}",
                 (unsigned)b->start, b->cond, action);
    } else if (b->end > b->start) {
        snprintf(cmd, sizeof(cmd), "debug set_watchpoint %s {0x%04X 0x%04X} {%s} {%s}",
                 type_word(b->type), (unsigned)b->start, (unsigned)b->end,
                 b->cond, action);
    } else {
        snprintf(cmd, sizeof(cmd), "debug set_watchpoint %s 0x%04X {%s} {%s}",
                 type_word(b->type), (unsigned)b->start, b->cond, action);
    }
    ok = rpc(cmd, res, sizeof(res));
    if (!ok) {
        if (err && errsz > 0) snprintf(err, (size_t)errsz, "%s", res[0] ? res : "openMSX refused it");
        return 0;
    }
    snprintf(b->oid, sizeof(b->oid), "%s", res);
    return 1;
}

static void disarm_locked(bp_entry *b)
{
    if (!b->oid[0]) return;
    if (strncmp(b->oid, "bp#", 3) == 0)
        rpcf(NULL, 0, "debug remove_bp %s", b->oid);
    else
        rpcf(NULL, 0, "debug remove_watchpoint %s", b->oid);
    b->oid[0] = '\0';
}

static void arm_all_locked(msxdebug *d)
{
    int i;
    for (i = 0; i < d->bp_count; i++) {
        bp_entry *b = &d->bps[i];
        b->oid[0] = '\0';
        if (b->enabled && !arm_locked(d, b, NULL, 0))
            b->enabled = 0;     /* a condition openMSX will not take */
    }
}

static void disarm_all_locked(msxdebug *d)
{
    int i;
    for (i = 0; i < d->bp_count; i++) disarm_locked(&d->bps[i]);
    remove_transient_locked(d);
}

/* ---- stop discovery ------------------------------------------------------- */

static void describe_bp(bp_entry *b, uint16_t pc, char *dst, int dstsz)
{
    switch (b->type) {
    case MSXDEBUG_BP_EXEC:
        snprintf(dst, (size_t)dstsz, "breakpoint %d at $%04X", b->id, (unsigned)b->start);
        break;
    default:
        if (b->end > b->start)
            snprintf(dst, (size_t)dstsz, "watchpoint %d: %s $%04X-$%04X (PC $%04X)", b->id,
                     b->type == MSXDEBUG_BP_READ ? "read" : b->type == MSXDEBUG_BP_WRITE ? "write"
                     : b->type == MSXDEBUG_BP_IN ? "in" : "out",
                     (unsigned)b->start, (unsigned)b->end, (unsigned)pc);
        else
            snprintf(dst, (size_t)dstsz, "watchpoint %d: %s $%04X (PC $%04X)", b->id,
                     b->type == MSXDEBUG_BP_READ ? "read" : b->type == MSXDEBUG_BP_WRITE ? "write"
                     : b->type == MSXDEBUG_BP_IN ? "in" : "out",
                     (unsigned)b->start, (unsigned)pc);
    }
}

/* Reads and clears which breakpoint last fired (0: none). */
static int take_last(void)
{
    char buf[16];
    if (!rpc("expr {[info exists ::fngo_last] ? $::fngo_last : 0}", buf, sizeof(buf)))
        return 0;
    rpc("unset -nocomplain ::fngo_last", NULL, 0);
    return atoi(buf);
}

static void *poll_thread_fn(void *arg)
{
    msxdebug *d = arg;
    for (;;) {
        struct timespec ts;
        int stop_requested, idle;
        char buf[16];

        ts.tv_sec = 0;
        ts.tv_nsec = MSXDBG_POLL_INTERVAL_MS * 1000000L;
        nanosleep(&ts, NULL);

        pthread_mutex_lock(&d->mtx);
        stop_requested = d->poll_stop;
        idle = d->paused || !d->started || (!d->attached && !d->transient_id[0]);
        pthread_mutex_unlock(&d->mtx);
        if (stop_requested) break;
        if (idle) continue;

        if (!rpc("debug breaked", buf, sizeof(buf))) continue;
        if (strcmp(buf, "1") != 0 && strcmp(buf, "true") != 0) continue;

        {
            void (*cb)(void *, int, uint16_t);
            void *ud;
            char reason[128];
            int fired, kind = MSXDBG_STOP_BREAKPOINT;
            uint16_t pc;

            pthread_mutex_lock(&d->mtx);
            if (d->paused) {            /* lost the race to a synchronous caller */
                pthread_mutex_unlock(&d->mtx);
                continue;
            }
            fired = take_last();
            pc = read_pc_now();
            d->paused = 1;
            if (fired) {
                bp_entry *b = bp_by_id(d, fired);
                if (b) describe_bp(b, pc, reason, sizeof(reason));
                else   snprintf(reason, sizeof(reason), "breakpoint %d", fired);
            } else if (d->pending_reason) {
                snprintf(reason, sizeof(reason), "%s", d->pending_reason);
                kind = MSXDBG_STOP_STEP;
            } else {
                snprintf(reason, sizeof(reason), "stopped");
            }
            d->pending_reason = NULL;
            d->transient_id[0] = '\0';  /* a fired -once bp already self-removed */
            note_stop_locked(d, reason, pc);
            cb = d->stop_cb;
            ud = d->stop_ud;
            pthread_mutex_unlock(&d->mtx);
            if (cb) cb(ud, kind, pc);
        }
    }
    return NULL;
}

/* ---- lifecycle ---------------------------------------------------------- */

msxdebug *msxdebug_create(msxsession *s)
{
    msxdebug *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->session = s;
    d->next_id = 1;
    d->reason_addr = -1;
    snprintf(d->reason, sizeof(d->reason), "running");
    pthread_mutex_init(&d->mtx, NULL);
    d->started = msxsession_is_running(s);
    symtabs_load_text(&d->syms, msxdebug_builtin_bios_sym, "bios");
    if (pthread_create(&d->poll_thread, NULL, poll_thread_fn, d) == 0)
        d->poll_thread_running = 1;
    return d;
}

void msxdebug_destroy(msxdebug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->poll_stop = 1;
    pthread_mutex_unlock(&d->mtx);
    if (d->poll_thread_running) pthread_join(d->poll_thread, NULL);
    symtabs_free(&d->syms);
    free(d->user_syms);
    pthread_mutex_destroy(&d->mtx);
    free(d);
}

msxdebug *msxsession_debugger(msxsession *s)
{
    pthread_mutex_lock(&s->lifecycle_mtx);
    if (!s->debugger)
        s->debugger = msxdebug_create(s);
    pthread_mutex_unlock(&s->lifecycle_mtx);
    return s->debugger;
}

/* ---- attach / detach -------------------------------------------------------- */

/* Stops the machine and records why. Caller holds d->mtx. */
static void stop_locked(msxdebug *d, const char *why)
{
    uint16_t pc;
    remove_transient_locked(d);
    rpc("debug break", NULL, 0);
    pc = read_pc_now();
    d->paused = 1;
    d->pending_reason = NULL;
    note_stop_locked(d, why, pc);
}

void msxdebug_attach(msxdebug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->attached) { pthread_mutex_unlock(&d->mtx); return; }
    d->attached = 1;
    d->started = msxsession_is_running(d->session);
    if (d->started) {
        arm_all_locked(d);
        stop_locked(d, "stopped");
    } else {
        d->gen++;       /* armed and stopped when the machine comes up */
    }
    pthread_mutex_unlock(&d->mtx);
}

void msxdebug_detach(msxdebug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (!d->attached) { pthread_mutex_unlock(&d->mtx); return; }
    d->attached = 0;
    if (d->started) {
        disarm_all_locked(d);
        if (d->paused) {
            d->paused = 0;
            rpc("debug cont", NULL, 0);
        }
    }
    snprintf(d->reason, sizeof(d->reason), "running");
    d->reason_addr = -1;
    d->gen++;
    pthread_mutex_unlock(&d->mtx);
}

int msxdebug_is_attached(msxdebug *d)
{
    int a;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    a = d->attached;
    pthread_mutex_unlock(&d->mtx);
    return a;
}

void msxdebug_machine_changed(msxdebug *d, int power_cycled)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->started = msxsession_is_running(d->session);
    if (power_cycled) {
        /* a new machine: its breakpoints and run state are gone */
        int i;
        for (i = 0; i < d->bp_count; i++) d->bps[i].oid[0] = '\0';
        d->transient_id[0] = '\0';
        d->paused = 0;
        d->pending_reason = NULL;
        if (d->attached && d->started) {
            arm_all_locked(d);
            stop_locked(d, "stopped at power-up");
        }
    }
    d->gen++;
    pthread_mutex_unlock(&d->mtx);
}

/* ---- execution control -------------------------------------------------- */

void msxdebug_stop(msxdebug *d)
{
    void (*cb)(void *, int, uint16_t);
    void *ud;
    uint16_t pc;

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->paused) { pthread_mutex_unlock(&d->mtx); return; }
    stop_locked(d, "stopped");
    pc = (uint16_t)d->reason_addr;
    cb = d->stop_cb;
    ud = d->stop_ud;
    pthread_mutex_unlock(&d->mtx);
    if (cb) cb(ud, MSXDBG_STOP_PAUSE, pc);
}

void msxdebug_resume(msxdebug *d)
{
    if (!d) return;
    /* d->mtx stays held across "debug cont" itself (safe: it is a different
     * mutex than msxhost_execute_sync's own): otherwise there is a window,
     * after paused is cleared but before the CPU has been told to continue,
     * where the poll thread can see paused == 0, check "debug breaked"
     * (still true from the PRIOR break) and misreport a stale stop. */
    pthread_mutex_lock(&d->mtx);
    remove_transient_locked(d);
    d->paused = 0;
    d->pending_reason = NULL;
    snprintf(d->reason, sizeof(d->reason), "running");
    d->reason_addr = -1;
    d->gen++;
    rpc("debug cont", NULL, 0);
    pthread_mutex_unlock(&d->mtx);
}

int msxdebug_is_stopped(msxdebug *d)
{
    int p;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    p = d->paused;
    pthread_mutex_unlock(&d->mtx);
    return p;
}

int msxdebug_stop_reason(msxdebug *d, char *dst, int dstsz, int *address)
{
    int n;
    if (!d || !dst || dstsz <= 0) return 0;
    pthread_mutex_lock(&d->mtx);
    n = snprintf(dst, (size_t)dstsz, "%s", d->paused ? d->reason : "running");
    if (address) *address = d->paused ? d->reason_addr : -1;
    pthread_mutex_unlock(&d->mtx);
    return n;
}

unsigned msxdebug_generation(msxdebug *d)
{
    unsigned g;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    g = d->gen;
    pthread_mutex_unlock(&d->mtx);
    return g;
}

void msxdebug_step(msxdebug *d)
{
    uint16_t pc;
    void (*cb)(void *, int, uint16_t);
    void *ud;

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (!d->paused) { pthread_mutex_unlock(&d->mtx); return; }
    rpc("debug step", NULL, 0);
    pc = read_pc_now();
    note_stop_locked(d, "step", pc);
    cb = d->stop_cb;
    ud = d->stop_ud;
    pthread_mutex_unlock(&d->mtx);
    if (cb) cb(ud, MSXDBG_STOP_STEP, pc);
}

/* Arms a one-shot breakpoint at target (optionally only once the stack has
 * unwound to/above sp_floor, so it survives recursion and an interrupt firing
 * while the callee runs -- the SP-floor trick, expressed as a Tcl condition),
 * then resumes. The eventual stop is delivered by the poll thread, like a
 * persistent breakpoint firing. */
static void arm_transient_and_go(msxdebug *d, uint16_t target, int use_floor,
                                 uint16_t sp_floor, int strict, const char *why)
{
    char cmd[96], idbuf[16];

    if (use_floor)
        snprintf(cmd, sizeof(cmd), "debug set_bp -once 0x%04X {[reg SP] %s %u}",
                 (unsigned)target, strict ? ">" : ">=", (unsigned)sp_floor);
    else
        snprintf(cmd, sizeof(cmd), "debug set_bp -once 0x%04X", (unsigned)target);

    /* See msxdebug_resume(): d->mtx is held across "debug cont" itself. */
    pthread_mutex_lock(&d->mtx);
    remove_transient_locked(d);
    if (rpc(cmd, idbuf, sizeof(idbuf)))
        snprintf(d->transient_id, sizeof(d->transient_id), "%s", idbuf);
    d->pending_reason = why;
    d->paused = 0;
    d->gen++;
    rpc("debug cont", NULL, 0);
    pthread_mutex_unlock(&d->mtx);
}

void msxdebug_step_over(msxdebug *d)
{
    msxdebug_cpu c;
    uint8_t code[4] = {0, 0, 0, 0};
    z80d_insn insn;
    uint16_t pc, sp;

    if (!d || !msxdebug_is_stopped(d)) return;
    msxdebug_cpu_get(d, &c);
    pc = (uint16_t)c.pc;
    sp = (uint16_t)c.sp;
    msxdebug_read(d, pc, code, 4);
    z80_disassemble(&insn, pc, code);

    if (!(insn.flags & (MSXDASM_CALL | MSXDASM_HALT | MSXDASM_BLOCK))) {
        /* A plain instruction: stepping into it IS stepping over it. */
        msxdebug_step(d);
        return;
    }
    arm_transient_and_go(d, (uint16_t)(pc + insn.len), 1, sp, 0, "step over");
}

void msxdebug_step_out(msxdebug *d)
{
    msxdebug_cpu c;
    uint8_t retbytes[2] = {0, 0};
    uint16_t sp, target;

    if (!d || !msxdebug_is_stopped(d)) return;
    msxdebug_cpu_get(d, &c);
    sp = (uint16_t)c.sp;
    msxdebug_read(d, sp, retbytes, 2);
    target = (uint16_t)(retbytes[0] | (retbytes[1] << 8));
    arm_transient_and_go(d, target, 1, sp, 1, "step out");
}

void msxdebug_frame(msxdebug *d)
{
    if (!d) return;
    arm_transient_and_go(d, VBLANK_VECTOR, 0, 0, 0, "frame");
}

void msxdebug_run_to(msxdebug *d, uint16_t addr)
{
    if (!d) return;
    arm_transient_and_go(d, addr, 0, 0, 0, "run to");
}

void msxdebug_set_stop_callback(msxdebug *d,
                                void (*cb)(void *, int, uint16_t), void *ud)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->stop_cb = cb;
    d->stop_ud = ud;
    pthread_mutex_unlock(&d->mtx);
}

/* ---- CPU ------------------------------------------------------------------- */

void msxdebug_cpu_get(msxdebug *d, msxdebug_cpu *out)
{
    uint8_t r[32];
    int len = 0;
    (void)d;
    memset(out, 0, sizeof(*out));
    if (!msxhost_execute_sync_binary("debug read_block {CPU regs} 0 28", r,
                                     (int)sizeof(r), &len) || len < 28)
        return;
    /* the layout of openMSX's "CPU regs" debuggable: A F B C D E H L, the
     * primes, IXH IXL IYH IYL, PCH PCL SPH SPL, I R IM IFF */
    out->af = r[0] << 8 | r[1];
    out->bc = r[2] << 8 | r[3];
    out->de = r[4] << 8 | r[5];
    out->hl = r[6] << 8 | r[7];
    out->af2 = r[8] << 8 | r[9];
    out->bc2 = r[10] << 8 | r[11];
    out->de2 = r[12] << 8 | r[13];
    out->hl2 = r[14] << 8 | r[15];
    out->ix = r[16] << 8 | r[17];
    out->iy = r[18] << 8 | r[19];
    out->pc = r[20] << 8 | r[21];
    out->sp = r[22] << 8 | r[23];
    out->i = r[24];
    out->r = r[25];
    out->im = r[26];
    out->iff1 = r[27] & 1;
    out->iff2 = (r[27] >> 1) & 1;
    out->sf = (r[1] >> 7) & 1;
    out->zf = (r[1] >> 6) & 1;
    out->yf = (r[1] >> 5) & 1;
    out->hf = (r[1] >> 4) & 1;
    out->xf = (r[1] >> 3) & 1;
    out->pf = (r[1] >> 2) & 1;
    out->nf = (r[1] >> 1) & 1;
    out->cf = r[1] & 1;
}

void msxdebug_cpu_set(msxdebug *d, int reg, int value)
{
    static const char *const names[] = {
        "PC", "SP", "AF", "BC", "DE", "HL", "IX", "IY", "AF2", "BC2", "DE2", "HL2",
        "I", "R", "IM", NULL, NULL, "A", "F", "B", "C", "D", "E", "H", "L",
    };
    static const int flag_bit[] = { 7, 6, 4, 2, 1, 0 };   /* S Z H PV N C */
    msxdebug_cpu c;

    if (!d || reg < 0 || reg >= MSX_REG_COUNT) return;
    pthread_mutex_lock(&d->mtx);
    if (reg == MSX_REG_IFF1 || reg == MSX_REG_IFF2) {
        const int bit = reg == MSX_REG_IFF1 ? 0 : 1;
        char buf[16];
        int cur = 0;
        if (rpc("debug read {CPU regs} 27", buf, sizeof(buf))) cur = atoi(buf);
        cur = value ? (cur | (1 << bit)) : (cur & ~(1 << bit));
        rpcf(NULL, 0, "debug write {CPU regs} 27 %d", cur);
    } else if (reg >= MSX_FLAG_S) {
        const int bit = flag_bit[reg - MSX_FLAG_S];
        int f;
        msxdebug_cpu_get(d, &c);
        f = c.af & 0xFF;
        f = value ? (f | (1 << bit)) : (f & ~(1 << bit));
        rpcf(NULL, 0, "reg F %d", f);
    } else if (names[reg]) {
        rpcf(NULL, 0, "reg %s %d", names[reg], value);
    }
    d->gen++;
    pthread_mutex_unlock(&d->mtx);
}

/* ---- memory ----------------------------------------------------------------- */

int msxdebug_read(msxdebug *d, uint16_t addr, uint8_t *dst, int n)
{
    char cmd[64];
    int len = 0;
    (void)d;
    if (n <= 0) return 0;
    if ((int)addr + n > 0x10000) n = 0x10000 - (int)addr;
    snprintf(cmd, sizeof(cmd), "debug read_block memory %u %d", (unsigned)addr, n);
    if (!msxhost_execute_sync_binary(cmd, dst, n, &len)) {
        memset(dst, 0, (size_t)n);
        return 0;
    }
    return len < n ? len : n;
}

void msxdebug_write(msxdebug *d, uint16_t addr, uint8_t value)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    rpcf(NULL, 0, "debug write memory %u %u", (unsigned)addr, (unsigned)value);
    d->gen++;
    pthread_mutex_unlock(&d->mtx);
}

/* ---- slots and I/O ---------------------------------------------------------- */

int msxdebug_io_state(msxdebug *d, char *dst, int dstsz)
{
    char res[4096], tmp[64];
    int n = 0, page, i;
    uint8_t psg[16];
    int len = 0;

#define PUT(...) do { if (n < dstsz) n += snprintf(dst + n, (size_t)(dstsz - n), __VA_ARGS__); } while (0)
    (void)d;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';

    if (rpc("machine_info type", tmp, sizeof(tmp)))
        PUT("Machine: %s\n\n", tmp);

    PUT("Selected slots (CPU pages):\n");
    for (page = 0; page < 4; page++) {
        char sel[32];
        if (rpcf(sel, sizeof(sel), "get_selected_slot %d", page)) {
            int prim = 0;
            char sub[8] = "X";
            sscanf(sel, "%d %7s", &prim, sub);
            if (sub[0] == 'X')
                PUT("  page %d ($%04X): slot %d\n", page, page * 0x4000, prim);
            else
                PUT("  page %d ($%04X): slot %d-%s\n", page, page * 0x4000, prim, sub);
        }
    }

    if (rpc("debug read ioports 0xA8", tmp, sizeof(tmp)))
        PUT("\nPPI $A8 (slot register): $%02X\n", atoi(tmp) & 0xFF);

    if (rpc("debug read {Main RAM regs} 0", tmp, sizeof(tmp))) {
        PUT("Memory mapper ($FC-$FF):");
        for (i = 0; i < 4; i++) {
            char c[16];
            if (rpcf(c, sizeof(c), "debug read {Main RAM regs} %d", i))
                PUT(" $%02X", atoi(c) & 0xFF);
        }
        PUT("\n");
    }

    if (msxhost_execute_sync_binary("debug read_block {PSG regs} 0 16", psg,
                                    (int)sizeof(psg), &len) && len >= 14) {
        PUT("\nPSG (AY-3-8910) registers:\n  ");
        for (i = 0; i < 14 && i < len; i++)
            PUT("R%X=%02X%s", i, psg[i], (i == 6 || i == 13) ? "\n  " : " ");
        PUT("\n  tones: A %u, B %u, C %u  noise %u  mixer $%02X  volumes A %u B %u C %u\n",
            psg[0] | (psg[1] & 0xF) << 8, psg[2] | (psg[3] & 0xF) << 8,
            psg[4] | (psg[5] & 0xF) << 8, psg[6] & 0x1F, psg[7], psg[8] & 0x1F,
            psg[9] & 0x1F, psg[10] & 0x1F);
    }

    if (rpc("slotmap", res, sizeof(res)))
        PUT("\nSlot map:\n%s\n", res);
#undef PUT
    return n;
}

/* ---- symbols ---------------------------------------------------------------- */

int msxdebug_load_symbols(msxdebug *d, const char *path, char *msg, int msgsz)
{
    int rc, before;
    if (!d || !path) return -1;
    pthread_mutex_lock(&d->mtx);
    before = d->syms.count;
    rc = symtabs_load_file(&d->syms, path, "user");
    if (rc < 0 && msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "could not read %s", path);
    else if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "%d label%s from %s", rc, rc == 1 ? "" : "s", path);
    (void)before;
    d->gen++;
    pthread_mutex_unlock(&d->mtx);
    return rc;
}

const char *msxdebug_symbol_at(msxdebug *d, uint16_t addr, uint16_t *offset)
{
    if (!d) return NULL;
    return symtabs_at(&d->syms, addr, offset);
}

int msxdebug_symbol_find(msxdebug *d, const char *name, uint16_t *addr)
{
    if (!d) return 0;
    return symtabs_find(&d->syms, name, addr);
}

int msxdebug_label_address(msxdebug *d, const char *label)
{
    uint16_t a;
    if (!d || !label || !msxdebug_symbol_find(d, label, &a)) return -1;
    return a;
}

int msxdebug_address_label(msxdebug *d, uint16_t addr, char *dst, int dstsz)
{
    uint16_t off = 0;
    const char *s;
    if (dst && dstsz > 0) dst[0] = '\0';
    s = d ? symtabs_at(&d->syms, addr, &off) : NULL;
    if (!s || off != 0) return 0;
    if (dst && dstsz > 0) snprintf(dst, (size_t)dstsz, "%s", s);
    return (int)strlen(s);
}

int msxdebug_set_label(msxdebug *d, uint16_t addr, const char *label)
{
    char line[96];
    size_t n, add;
    char *p;
    if (!d || !label || !*label || strpbrk(label, " \t;\n")) return -1;
    snprintf(line, sizeof(line), "%04X %s\n", (unsigned)addr, label);
    pthread_mutex_lock(&d->mtx);
    n = d->user_syms ? strlen(d->user_syms) : 0;
    add = strlen(line);
    p = realloc(d->user_syms, n + add + 1);
    if (!p) { pthread_mutex_unlock(&d->mtx); return -1; }
    memcpy(p + n, line, add + 1);
    d->user_syms = p;
    symtabs_load_text(&d->syms, d->user_syms, "labels");
    d->gen++;
    pthread_mutex_unlock(&d->mtx);
    return 0;
}

/* ---- disassembly ------------------------------------------------------------- */

static void fill_line(msxdebug *d, msxdebug_line *l, uint16_t pc,
                      const uint8_t code[4], uint16_t cur_pc)
{
    z80d_insn insn;
    int i;
    uint16_t off = 0;
    const char *sym;
    bp_entry *b;

    z80_disassemble(&insn, pc, code);
    memset(l, 0, sizeof(*l));
    l->address = pc;
    l->length = insn.len;
    l->is_pc = pc == cur_pc;
    for (i = 0; i < insn.len && i < 4; i++)
        snprintf(l->bytes + i * 3, sizeof(l->bytes) - (size_t)i * 3, "%02X ", insn.bytes[i]);
    snprintf(l->disasm, sizeof(l->disasm), "%s", insn.text);
    sym = symtabs_at(&d->syms, pc, &off);
    if (sym && off == 0) snprintf(l->label, sizeof(l->label), "%s", sym);
    b = bp_exec_at(d, pc);
    if (b) {
        l->has_breakpoint = 1;
        l->breakpoint_enabled = b->enabled;
    }
    if (insn.flags & (MSXDASM_JUMP | MSXDASM_CALL)) {
        sym = symtabs_at(&d->syms, insn.target, &off);
        if (sym && off == 0) snprintf(l->comment, sizeof(l->comment), "%s", sym);
    }
}

int msxdebug_disassemble(msxdebug *d, uint16_t addr, msxdebug_line *out,
                         int max, int *pc_line)
{
    uint8_t *mem;
    int span, got, i, off = 0;
    msxdebug_cpu c;
    uint16_t pc;

    if (pc_line) *pc_line = -1;
    if (!d || !out || max <= 0) return 0;
    msxdebug_cpu_get(d, &c);
    pc = (uint16_t)c.pc;
    span = max * 4 + 4;
    if (span > 0x10000) span = 0x10000;
    mem = calloc(1, (size_t)span + 4);
    if (!mem) return 0;
    got = msxdebug_read(d, addr, mem, span);
    (void)got;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < max; i++) {
        uint16_t a = (uint16_t)(addr + off);
        uint8_t code[4];
        int k;
        if (off + 4 > span) {       /* wraps or ran out of the block: read apart */
            uint8_t tmp[4] = {0, 0, 0, 0};
            msxdebug_read(d, a, tmp, 4);
            memcpy(code, tmp, 4);
        } else {
            for (k = 0; k < 4; k++) code[k] = mem[off + k];
        }
        fill_line(d, &out[i], a, code, pc);
        if (out[i].is_pc && pc_line) *pc_line = i;
        off += out[i].length;
    }
    pthread_mutex_unlock(&d->mtx);
    free(mem);
    return max;
}

int msxdebug_row_address(msxdebug *d, uint16_t addr, int rows)
{
    uint8_t buf[512 + 8];
    int i;

    if (!d || rows == 0) return addr;
    if (rows > 0) {
        uint16_t a = addr;
        for (i = 0; i < rows; i++) {
            uint8_t code[4];
            z80d_insn insn;
            msxdebug_read(d, a, code, 4);
            z80_disassemble(&insn, a, code);
            a = (uint16_t)(a + insn.len);
        }
        return a;
    } else {
        /* A best guess: decode forward from a little way back and take the
         * row boundary `rows` before addr on the path that lands on addr. */
        const int want = -rows;
        int back = want * 3 + 6, start;
        uint16_t from, pos[256];
        int count = 0, hit = -1;
        if (back > 500) back = 500;
        start = (int)addr - back;
        if (start < 0) start = 0;
        from = (uint16_t)start;
        msxdebug_read(d, from, buf, (int)(addr - from) + 8 > (int)sizeof(buf)
                                        ? (int)sizeof(buf) : (int)(addr - from) + 8);
        {
            uint16_t a = from;
            while ((int)a < (int)addr && count < 256) {
                z80d_insn insn;
                uint8_t code[4];
                int o = a - from, k;
                for (k = 0; k < 4; k++) code[k] = (o + k < (int)sizeof(buf)) ? buf[o + k] : 0;
                z80_disassemble(&insn, a, code);
                pos[count++] = a;
                a = (uint16_t)(a + insn.len);
            }
            if (a == addr) hit = count - want;
        }
        if (hit >= 0 && hit < count) return pos[hit];
        return (addr + rows) & 0xFFFF;
    }
}

/* ---- breakpoints ---------------------------------------------------------------- */

int msxdebug_breakpoint_check(msxdebug *d, uint16_t addr)
{
    int r;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    r = bp_exec_at(d, addr) != NULL;
    pthread_mutex_unlock(&d->mtx);
    return r;
}

static void bp_remove_at(msxdebug *d, int idx)
{
    disarm_locked(&d->bps[idx]);
    d->bps[idx] = d->bps[d->bp_count - 1];
    d->bp_count--;
}

int msxdebug_breakpoint_add(msxdebug *d, int type, uint16_t start, uint16_t end,
                            const char *condition, char *err, int errsz)
{
    bp_entry *b;
    int id;

    if (err && errsz > 0) err[0] = '\0';
    if (!d) return -1;
    if (type != MSXDEBUG_BP_EXEC && type != MSXDEBUG_BP_READ &&
        type != MSXDEBUG_BP_WRITE && type != MSXDEBUG_BP_IN && type != MSXDEBUG_BP_OUT) {
        if (err) snprintf(err, (size_t)errsz, "unknown breakpoint type");
        return -1;
    }
    if (condition && (strchr(condition, '{') || strchr(condition, '}'))) {
        if (err) snprintf(err, (size_t)errsz, "no braces in a condition");
        return -1;
    }
    if (end < start) end = start;
    if ((type == MSXDEBUG_BP_IN || type == MSXDEBUG_BP_OUT) && end > 0xFF) {
        if (err) snprintf(err, (size_t)errsz, "ports are $00-$FF");
        return -1;
    }
    if (condition && *condition && d->started) {
        /* openMSX only evaluates a condition when the breakpoint is hit, so
         * a typo would sit silent: try the expression once now */
        char res[160];
        pthread_mutex_lock(&d->mtx);
        {
            char cmd[200];
            snprintf(cmd, sizeof(cmd), "expr {%s}", condition);
            if (!rpc(cmd, res, sizeof(res))) {
                pthread_mutex_unlock(&d->mtx);
                if (err && errsz > 0)
                    snprintf(err, (size_t)errsz, "%s", res[0] ? res : "bad condition");
                return -1;
            }
        }
        pthread_mutex_unlock(&d->mtx);
    }
    pthread_mutex_lock(&d->mtx);
    if (d->bp_count >= MSXDBG_MAX_BP) {
        pthread_mutex_unlock(&d->mtx);
        if (err) snprintf(err, (size_t)errsz, "too many breakpoints");
        return -1;
    }
    b = &d->bps[d->bp_count];
    memset(b, 0, sizeof(*b));
    b->id = d->next_id;
    b->type = type;
    b->start = start;
    b->end = type == MSXDEBUG_BP_EXEC ? start : end;
    b->enabled = 1;
    snprintf(b->cond, sizeof(b->cond), "%s", condition ? condition : "");
    if (d->attached && d->started && !arm_locked(d, b, err, errsz)) {
        pthread_mutex_unlock(&d->mtx);
        return -1;
    }
    id = b->id;
    d->next_id++;
    d->bp_count++;
    d->gen++;
    pthread_mutex_unlock(&d->mtx);
    return id;
}

int msxdebug_breakpoint_toggle(msxdebug *d, uint16_t addr)
{
    int idx, now_set;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    {
        bp_entry *b = bp_exec_at(d, addr);
        if (b) {
            idx = (int)(b - d->bps);
            bp_remove_at(d, idx);
            d->gen++;
            pthread_mutex_unlock(&d->mtx);
            return 0;
        }
    }
    pthread_mutex_unlock(&d->mtx);
    now_set = msxdebug_breakpoint_add(d, MSXDEBUG_BP_EXEC, addr, addr, "", NULL, 0) >= 0;
    return now_set;
}

void msxdebug_breakpoint_remove(msxdebug *d, int id)
{
    int i;
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->bp_count; i++)
        if (d->bps[i].id == id) {
            bp_remove_at(d, i);
            d->gen++;
            break;
        }
    pthread_mutex_unlock(&d->mtx);
}

void msxdebug_breakpoint_enable(msxdebug *d, int id, int enabled)
{
    bp_entry *b;
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    b = bp_by_id(d, id);
    if (b && b->enabled != (enabled != 0)) {
        b->enabled = enabled != 0;
        if (d->attached && d->started) {
            if (b->enabled) {
                if (!arm_locked(d, b, NULL, 0)) b->enabled = 0;
            } else {
                disarm_locked(b);
            }
        }
        d->gen++;
    }
    pthread_mutex_unlock(&d->mtx);
}

int msxdebug_breakpoint_list(msxdebug *d, msxdebug_breakpoint *out, int max)
{
    char res[2048];
    int n, i;
    unsigned hits[MSXDBG_MAX_BP * 2 + 2];
    int hit_ids[MSXDBG_MAX_BP * 2 + 2];
    int nhits = 0;

    if (!d) return 0;
    /* hit counts live in a Tcl array openMSX's actions bump */
    if (rpc("array get ::fngo_hit", res, sizeof(res))) {
        char *p = res;
        while (*p && nhits < (int)(sizeof(hits) / sizeof(hits[0]))) {
            char *e;
            long id = strtol(p, &e, 10);
            long h;
            if (e == p) break;
            p = e;
            h = strtol(p, &e, 10);
            if (e == p) break;
            p = e;
            hit_ids[nhits] = (int)id;
            hits[nhits++] = (unsigned)h;
        }
    }
    pthread_mutex_lock(&d->mtx);
    n = d->bp_count < max ? d->bp_count : max;
    for (i = 0; i < n; i++) {
        int k;
        out[i].id = d->bps[i].id;
        out[i].type = d->bps[i].type;
        out[i].start = d->bps[i].start;
        out[i].end = d->bps[i].end;
        out[i].enabled = d->bps[i].enabled;
        out[i].hits = 0;
        for (k = 0; k < nhits; k++)
            if (hit_ids[k] == d->bps[i].id) out[i].hits = hits[k];
        snprintf(out[i].condition, sizeof(out[i].condition), "%s", d->bps[i].cond);
    }
    pthread_mutex_unlock(&d->mtx);
    return n;
}

void msxdebug_breakpoint_clear(msxdebug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    while (d->bp_count > 0) bp_remove_at(d, d->bp_count - 1);
    rpc("array unset ::fngo_hit", NULL, 0);
    d->gen++;
    pthread_mutex_unlock(&d->mtx);
}

/* ---- the console ------------------------------------------------------------- */

static int parse_number(msxdebug *d, const char *s, long *out)
{
    char *end;
    long v;
    size_t n;
    char tmp[64];

    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return 0;
    if (*s == '$') { v = strtol(s + 1, &end, 16); if (end != s + 1 && !*end) { *out = v; return 1; } return 0; }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        v = strtol(s + 2, &end, 16);
        if (end != s + 2 && !*end) { *out = v; return 1; }
        return 0;
    }
    n = strlen(s);
    if (n < sizeof(tmp) && (s[n - 1] == 'h' || s[n - 1] == 'H')) {
        memcpy(tmp, s, n - 1);
        tmp[n - 1] = '\0';
        v = strtol(tmp, &end, 16);
        if (end != tmp && !*end) { *out = v; return 1; }
    }
    v = strtol(s, &end, 10);
    if (end != s && !*end) { *out = v; return 1; }
    {
        uint16_t a;
        if (d && symtabs_find(&d->syms, s, &a)) { *out = a; return 1; }
    }
    return 0;
}

/* "ADDR" or "ADDR-END" */
static int parse_range(msxdebug *d, const char *s, long *a, long *b)
{
    char tmp[64], *dash;
    snprintf(tmp, sizeof(tmp), "%s", s);
    dash = strchr(tmp + 1, '-');
    if (dash) {
        *dash = '\0';
        return parse_number(d, tmp, a) && parse_number(d, dash + 1, b);
    }
    if (!parse_number(d, tmp, a)) return 0;
    *b = *a;
    return 1;
}

static int out_add(char *dst, int dstsz, int n, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (n >= dstsz - 1) return n;
    va_start(ap, fmt);
    k = vsnprintf(dst + n, (size_t)(dstsz - n), fmt, ap);
    va_end(ap);
    return k < 0 ? n : (n + k < dstsz ? n + k : dstsz - 1);
}

static int where_text(msxdebug *d, char *dst, int dstsz, int n)
{
    msxdebug_line l;
    msxdebug_cpu c;
    uint16_t pc;
    msxdebug_cpu_get(d, &c);
    pc = (uint16_t)c.pc;
    msxdebug_disassemble(d, pc, &l, 1, NULL);
    return out_add(dst, dstsz, n, "$%04X  %-12s %s%s%s\n", pc, l.bytes, l.disasm,
                   l.label[0] ? "   ; " : "", l.label);
}

static const char *const kCommands[] = {
    "help", "step", "over", "out", "run", "stop", "frame", "runto", "break",
    "bpr", "bpw", "bpin", "bpout", "delete", "enable", "disable", "clear",
    "breaks", "regs", "mem", "poke", "disasm", "label", "labels", NULL,
};

int msxdebug_command(msxdebug *d, const char *command, char *dst, int dstsz)
{
    char line[512], *argv[8];
    int argc = 0, n = 0;
    char *p;
    const char *cmd;
    long a, b;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!d || !command) return 0;
    snprintf(line, sizeof(line), "%s", command);
    for (p = line; *p == ' ' || *p == '\t'; p++) {}
    if (!*p) return 0;
    {
        char *q = p;
        while (*q && argc < 8) {
            argv[argc++] = q;
            while (*q && *q != ' ' && *q != '\t') q++;
            if (!*q) break;
            *q++ = '\0';
            while (*q == ' ' || *q == '\t') q++;
        }
    }
    cmd = argv[0];

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        n = out_add(dst, dstsz, n,
            "step [n]            one instruction (F7)\n"
            "over / out          step over a CALL (F8) / out to the RET (Shift+F8)\n"
            "run / stop          let it go (F5) / stop it\n"
            "frame               run to the next VDP interrupt\n"
            "runto <addr>        run until PC == addr\n"
            "break <addr> [cond] execute breakpoint\n"
            "bpr/bpw <addr>[-<end>] [cond]    memory read / write watchpoint\n"
            "bpin/bpout <port>[-<end>] [cond] I/O port watchpoint\n"
            "delete <id|addr>, enable <id>, disable <id>, clear, breaks\n"
            "regs                the CPU registers\n"
            "mem <addr> [len]    hex dump;  poke <addr> <val...>\n"
            "disasm [addr] [n]   disassemble\n"
            "label <addr> <name>, labels [file]\n"
            "Addresses: $C000, 0xC000, C000h, decimal or a label. A condition is a\n"
            "Tcl expression, e.g. [reg A] == 5. Anything else is passed to openMSX's\n"
            "Tcl console (debug list, reg A 5, slotmap, ...); Tab completes.\n");
    } else if (!strcmp(cmd, "step") || !strcmp(cmd, "s")) {
        long count = 1, i;
        if (argc > 1) parse_number(d, argv[1], &count);
        if (count < 1) count = 1;
        if (count > 1000) count = 1000;
        if (!msxdebug_is_stopped(d)) { n = out_add(dst, dstsz, n, "not stopped\n"); }
        else {
            for (i = 0; i < count; i++) msxdebug_step(d);
            n = where_text(d, dst, dstsz, n);
        }
    } else if (!strcmp(cmd, "over") || !strcmp(cmd, "n")) {
        msxdebug_step_over(d);
        n = out_add(dst, dstsz, n, "stepping over...\n");
    } else if (!strcmp(cmd, "out")) {
        msxdebug_step_out(d);
        n = out_add(dst, dstsz, n, "stepping out...\n");
    } else if (!strcmp(cmd, "run") || !strcmp(cmd, "go") || !strcmp(cmd, "c")) {
        msxdebug_resume(d);
        n = out_add(dst, dstsz, n, "running\n");
    } else if (!strcmp(cmd, "stop")) {
        msxdebug_stop(d);
        n = where_text(d, dst, dstsz, n);
    } else if (!strcmp(cmd, "frame")) {
        msxdebug_frame(d);
        n = out_add(dst, dstsz, n, "running to the next VDP interrupt...\n");
    } else if (!strcmp(cmd, "runto") && argc > 1 && parse_number(d, argv[1], &a)) {
        msxdebug_run_to(d, (uint16_t)a);
        n = out_add(dst, dstsz, n, "running to $%04X...\n", (unsigned)(a & 0xFFFF));
    } else if (!strcmp(cmd, "break") || !strcmp(cmd, "bp") || !strcmp(cmd, "bpr") ||
               !strcmp(cmd, "bpw") || !strcmp(cmd, "bpin") || !strcmp(cmd, "bpout")) {
        int type = !strcmp(cmd, "bpr") ? MSXDEBUG_BP_READ : !strcmp(cmd, "bpw") ? MSXDEBUG_BP_WRITE
                 : !strcmp(cmd, "bpin") ? MSXDEBUG_BP_IN : !strcmp(cmd, "bpout") ? MSXDEBUG_BP_OUT
                 : MSXDEBUG_BP_EXEC;
        char cond[128] = "", err[160];
        int id, i;
        if (argc < 2 || !parse_range(d, argv[1], &a, &b)) {
            n = out_add(dst, dstsz, n, "usage: %s <addr>[-<end>] [condition]\n", cmd);
        } else {
            /* the rest of the original line is the condition */
            const char *rest = command;
            for (i = 0; i < 2 && *rest; i++) {
                while (*rest == ' ' || *rest == '\t') rest++;
                while (*rest && *rest != ' ' && *rest != '\t') rest++;
            }
            while (*rest == ' ' || *rest == '\t') rest++;
            snprintf(cond, sizeof(cond), "%s", rest);
            id = msxdebug_breakpoint_add(d, type, (uint16_t)a, (uint16_t)b, cond, err, sizeof(err));
            if (id < 0) n = out_add(dst, dstsz, n, "error: %s\n", err);
            else        n = out_add(dst, dstsz, n, "breakpoint %d set\n", id);
        }
    } else if (!strcmp(cmd, "delete") && argc > 1 && parse_number(d, argv[1], &a)) {
        pthread_mutex_lock(&d->mtx);
        {
            bp_entry *byid = bp_by_id(d, (int)a), *byaddr = bp_exec_at(d, (uint16_t)a);
            int target = byid ? byid->id : byaddr ? byaddr->id : 0;
            pthread_mutex_unlock(&d->mtx);
            if (target) { msxdebug_breakpoint_remove(d, target); n = out_add(dst, dstsz, n, "deleted %d\n", target); }
            else n = out_add(dst, dstsz, n, "no such breakpoint\n");
        }
    } else if ((!strcmp(cmd, "enable") || !strcmp(cmd, "disable")) && argc > 1 &&
               parse_number(d, argv[1], &a)) {
        msxdebug_breakpoint_enable(d, (int)a, !strcmp(cmd, "enable"));
        n = out_add(dst, dstsz, n, "%sd %ld\n", cmd, a);
    } else if (!strcmp(cmd, "clear")) {
        msxdebug_breakpoint_clear(d);
        n = out_add(dst, dstsz, n, "all breakpoints cleared\n");
    } else if (!strcmp(cmd, "breaks")) {
        /* listed below, with hit counts */
    } else if (!strcmp(cmd, "regs")) {
        msxdebug_cpu c;
        msxdebug_cpu_get(d, &c);
        n = out_add(dst, dstsz, n,
            "AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X SP=%04X PC=%04X\n"
            "AF'=%04X BC'=%04X DE'=%04X HL'=%04X  I=%02X R=%02X IM=%d IFF1=%d IFF2=%d\n"
            "flags: %c%c%c%c%c%c\n",
            c.af, c.bc, c.de, c.hl, c.ix, c.iy, c.sp, c.pc, c.af2, c.bc2, c.de2, c.hl2,
            c.i, c.r, c.im, c.iff1, c.iff2,
            c.sf ? 'S' : '-', c.zf ? 'Z' : '-', c.hf ? 'H' : '-', c.pf ? 'P' : '-',
            c.nf ? 'N' : '-', c.cf ? 'C' : '-');
    } else if (!strcmp(cmd, "mem") && argc > 1 && parse_number(d, argv[1], &a)) {
        long len = 64, i;
        uint8_t buf[512];
        if (argc > 2) parse_number(d, argv[2], &len);
        if (len > (long)sizeof(buf)) len = (long)sizeof(buf);
        if (len < 1) len = 1;
        len = msxdebug_read(d, (uint16_t)a, buf, (int)len);
        for (i = 0; i < len; i += 16) {
            long k;
            n = out_add(dst, dstsz, n, "%04X: ", (unsigned)((a + i) & 0xFFFF));
            for (k = 0; k < 16 && i + k < len; k++) n = out_add(dst, dstsz, n, "%02X ", buf[i + k]);
            for (; k < 16; k++) n = out_add(dst, dstsz, n, "   ");
            n = out_add(dst, dstsz, n, " ");
            for (k = 0; k < 16 && i + k < len; k++)
                n = out_add(dst, dstsz, n, "%c", buf[i + k] >= 32 && buf[i + k] < 127 ? buf[i + k] : '.');
            n = out_add(dst, dstsz, n, "\n");
        }
    } else if (!strcmp(cmd, "poke") && argc > 2 && parse_number(d, argv[1], &a)) {
        int i;
        for (i = 2; i < argc; i++) {
            long v;
            if (!parse_number(d, argv[i], &v)) break;
            msxdebug_write(d, (uint16_t)(a + i - 2), (uint8_t)v);
        }
        n = out_add(dst, dstsz, n, "ok\n");
    } else if (!strcmp(cmd, "disasm") || !strcmp(cmd, "dis")) {
        long count = 8, i;
        msxdebug_line lines[64];
        msxdebug_cpu c;
        int pcl;
        msxdebug_cpu_get(d, &c);
        a = c.pc;
        if (argc > 1 && !parse_number(d, argv[1], &a)) a = c.pc;
        if (argc > 2) parse_number(d, argv[2], &count);
        if (count > 64) count = 64;
        if (count < 1) count = 1;
        msxdebug_disassemble(d, (uint16_t)a, lines, (int)count, &pcl);
        for (i = 0; i < count; i++)
            n = out_add(dst, dstsz, n, "%c%04X  %-12s %s%s%s\n", lines[i].is_pc ? '>' : ' ',
                        lines[i].address, lines[i].bytes, lines[i].disasm,
                        lines[i].label[0] ? "   ; " : "", lines[i].label);
    } else if (!strcmp(cmd, "label") && argc > 2 && parse_number(d, argv[1], &a)) {
        if (msxdebug_set_label(d, (uint16_t)a, argv[2]) == 0)
            n = out_add(dst, dstsz, n, "$%04X = %s\n", (unsigned)(a & 0xFFFF), argv[2]);
        else n = out_add(dst, dstsz, n, "bad label\n");
    } else if (!strcmp(cmd, "labels")) {
        char msg[160];
        if (argc > 1) { msxdebug_load_symbols(d, argv[1], msg, sizeof(msg)); n = out_add(dst, dstsz, n, "%s\n", msg); }
        else n = out_add(dst, dstsz, n, "usage: labels <file>\n");
    } else {
        /* openMSX's own console */
        char res[8192];
        int ok;
        pthread_mutex_lock(&d->mtx);
        ok = rpc(command, res, sizeof(res));
        d->gen++;
        pthread_mutex_unlock(&d->mtx);
        if (!ok) n = out_add(dst, dstsz, n, "error: %s\n", res);
        else if (res[0]) n = out_add(dst, dstsz, n, "%s\n", res);
    }

    /* "breaks" lists with hit counts */
    if (!strcmp(cmd, "breaks")) {
        msxdebug_breakpoint list[MSXDBG_MAX_BP];
        int cnt = msxdebug_breakpoint_list(d, list, MSXDBG_MAX_BP), i;
        if (!cnt) n = out_add(dst, dstsz, n, "no breakpoints\n");
        for (i = 0; i < cnt; i++) {
            const char *tn = list[i].type == MSXDEBUG_BP_EXEC ? "exec"
                : list[i].type == MSXDEBUG_BP_READ ? "read" : list[i].type == MSXDEBUG_BP_WRITE ? "write"
                : list[i].type == MSXDEBUG_BP_IN ? "in" : "out";
            n = out_add(dst, dstsz, n, "%d %s $%04X", list[i].id, tn, list[i].start);
            if (list[i].end > list[i].start) n = out_add(dst, dstsz, n, "-$%04X", list[i].end);
            n = out_add(dst, dstsz, n, " %s hits %u%s%s\n", list[i].enabled ? "on" : "off",
                        list[i].hits, list[i].condition[0] ? "  if " : "", list[i].condition);
        }
    }
    return n;
}

int msxdebug_completions(msxdebug *d, const char *prefix, char *dst, int dstsz)
{
    int count = 0, n = 0, i;
    const char *word;
    size_t wl;
    char res[512];

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!d || !prefix) return 0;
    word = strrchr(prefix, ' ');
    word = word ? word + 1 : prefix;
    wl = strlen(word);

    if (word == prefix) {       /* the command word */
        for (i = 0; kCommands[i]; i++)
            if (!strncmp(kCommands[i], word, wl)) {
                n = out_add(dst, dstsz, n, "%s\n", kCommands[i]);
                count++;
            }
    }
    if (wl) {                   /* labels */
        pthread_mutex_lock(&d->mtx);
        for (i = 0; i < d->syms.count; i++) {
            const sym_table *t = &d->syms.tables[i];
            int k;
            for (k = 0; k < t->count && count < 40; k++)
                if (!strncasecmp(t->entries[k].name, word, wl)) {
                    n = out_add(dst, dstsz, n, "%s\n", t->entries[k].name);
                    count++;
                }
        }
        pthread_mutex_unlock(&d->mtx);
    }
    /* what openMSX itself completes (a Tcl command line) */
    if (strchr(prefix, ' ') || !count) {
        char cmd[640];
        snprintf(cmd, sizeof(cmd), "tabcompletion {%s}", prefix);
        if (!strchr(prefix, '{') && !strchr(prefix, '}') && rpc(cmd, res, sizeof(res)) &&
            strlen(res) > strlen(prefix) && !strncmp(res, prefix, strlen(prefix))) {
            n = out_add(dst, dstsz, n, "%s\n", res + (word - prefix));
            count++;
        }
    }
    return count;
}

/* ---- files ------------------------------------------------------------------ */

int msxdebug_save(msxdebug *d, const char *kind, const char *path,
                  char *msg, int msgsz)
{
    FILE *f;
    int rc = 0;

    if (msg && msgsz > 0) msg[0] = '\0';
    if (!d || !kind || !path) return -1;
    f = fopen(path, kind[0] == 'd' || kind[0] == 'r' ? "w" : "wb");
    if (!f) {
        if (msg) snprintf(msg, (size_t)msgsz, "could not write %s", path);
        return -1;
    }
    if (!strcmp(kind, "dis")) {
        uint16_t a = 0;
        int rows = 0;
        while (rows < 0x10000) {
            msxdebug_line l[256];
            int got = msxdebug_disassemble(d, a, l, 256, NULL), i;
            for (i = 0; i < got; i++) {
                uint32_t next = (uint32_t)l[i].address + (uint32_t)l[i].length;
                if (l[i].label[0]) fprintf(f, "%s:\n", l[i].label);
                fprintf(f, "  %04X  %-12s %s\n", l[i].address, l[i].bytes, l[i].disasm);
                rows++;
                if (next > 0xFFFF) { rows = 0x10000; break; }
                a = (uint16_t)next;
            }
        }
    } else if (!strcmp(kind, "mem")) {
        static uint8_t buf[0x10000];
        msxdebug_read(d, 0, buf, 0x10000);
        fwrite(buf, 1, sizeof(buf), f);
    } else if (!strcmp(kind, "vram")) {
        char res[32], cmd[64];
        int size = 0, len = 0;
        uint8_t *buf;
        if (rpc("debug size {physical VRAM}", res, sizeof(res))) size = atoi(res);
        buf = size > 0 ? malloc((size_t)size) : NULL;
        snprintf(cmd, sizeof(cmd), "debug read_block {physical VRAM} 0 %d", size);
        if (buf && msxhost_execute_sync_binary(cmd, buf, size, &len))
            fwrite(buf, 1, (size_t)(len < size ? len : size), f);
        else rc = -1;
        free(buf);
    } else if (!strcmp(kind, "regs")) {
        char text[1024];
        msxdebug_command(d, "regs", text, sizeof(text));
        fputs(text, f);
    } else {
        rc = -1;
    }
    fclose(f);
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, rc == 0 ? "saved %s" : "could not save %s", path);
    return rc;
}

/* ---- FujiNet ---------------------------------------------------------------- */

int msxdebug_fujinet_trace(msxdebug *d, msx_fuji_packet *out, int max)
{
    if (!d) return 0;
    return fujitrace_packets(d->session->fuji_trace, out, max);
}

void msxdebug_fujinet_trace_clear(msxdebug *d)
{
    if (!d) return;
    fujitrace_clear(d->session->fuji_trace);
}

int msxdebug_fujinet_state(msxdebug *d, char *dst, int dstsz)
{
    if (!d) return 0;
    return fujitrace_format_state(d->session->fuji_trace,
                                  msxsession_fujinet_running(d->session),
                                  msxsession_fujinet_external(d->session),
                                  msxsession_fujinet_link(d->session),
                                  msxsession_fujinet_endpoint(d->session),
                                  dst, dstsz);
}
