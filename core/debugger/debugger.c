/*
 * msxdebug engine: execution control (pause/step into/over/out/run-to),
 * breakpoint management, and symbol annotation, all built on openMSX's own
 * Tcl debug interface via core/openmsx/msx_host.hh's msxhost_execute_sync/
 * _binary. See msxdebug.h's header comment for why this engine's shape
 * differs from fujinet-go-adam-desktop's adamdebug (no exec hook, no
 * per-instruction trace ring -- openMSX owns its own execution loop on its
 * own thread, entirely outside this project's control).
 *
 * Two ways a stop is discovered:
 *   1. Synchronously, when THIS thread just asked for one (pause,
 *      step_into) -- the control function itself blocks for the RPC and
 *      delivers the callback inline, no polling needed (msxhost_execute_
 *      sync's "debug break"/"debug step" already reflect the new state by
 *      the time they return -- confirmed empirically in rpc_test.c's
 *      broken-CPU case).
 *   2. Asynchronously, when a breakpoint (persistent, or a step_over/
 *      step_out/run_to's transient one-shot) fires on its own while
 *      running free -- a small poll thread this engine owns notices via
 *      periodic "debug breaked" checks (openMSX has no push notification
 *      for this over the Tcl interface) and delivers the callback then.
 *
 * d->mtx guards all of the above; msxhost_execute_sync has its own
 * independent mutex (g_rpc_mutex in msx_host.cc), so holding d->mtx across
 * an RPC call is safe and used freely below to keep control operations
 * atomic with respect to the poll thread.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../openmsx/msx_host.hh"
#include "../src/session_internal.h"
#include "msxdebug.h"
#include "symbols.h"
#include "z80dasm.h"

#define MSXDBG_MAX_BP 256
#define MSXDBG_POLL_INTERVAL_MS 40

struct msxdebug {
    msxsession *session;
    pthread_mutex_t mtx;
    pthread_t poll_thread;
    int poll_thread_running;
    int poll_stop;

    int paused;                        /* our own last-known state */
    msxdebug_stop_reason pending_reason; /* what the NEXT autonomous stop means */

    void (*stop_cb)(void *ud, msxdebug_stop_reason reason, uint16_t pc);
    void *stop_ud;

    /* Persistent breakpoints: our address -> openMSX's own bp id (needed
     * for "debug remove_bp <id>"). */
    struct {
        uint16_t addr;
        char id[16];
    } bps[MSXDBG_MAX_BP];
    int bp_count;

    /* The one transient one-shot breakpoint a pending step_over/step_out/
     * run_to may have live; "" when none is outstanding. */
    char transient_bp_id[16];

    sym_tables syms;
};

/* ---- small helpers --------------------------------------------------- */

static uint16_t read_pc_now(void)
{
    char buf[16];
    if (!msxhost_execute_sync("reg PC", buf, sizeof(buf))) return 0;
    return (uint16_t)strtol(buf, NULL, 10);
}

/* Removes any outstanding transient one-shot bp; safe to call whether or
 * not one actually fired already (a fired -once bp has already removed
 * itself on openMSX's side, so this "remove_bp" simply fails harmlessly).
 * Caller holds d->mtx. */
static void remove_transient_bp_locked(msxdebug *d)
{
    char cmd[48];
    if (!d->transient_bp_id[0]) return;
    snprintf(cmd, sizeof(cmd), "debug remove_bp %s", d->transient_bp_id);
    msxhost_execute_sync(cmd, NULL, 0);
    d->transient_bp_id[0] = '\0';
}

static int bp_find_locked(msxdebug *d, uint16_t addr)
{
    int i;
    for (i = 0; i < d->bp_count; i++)
        if (d->bps[i].addr == addr) return i;
    return -1;
}

/* ---- poll thread: catches autonomous stops ---------------------------- */

static void *poll_thread_fn(void *arg)
{
    msxdebug *d = arg;
    for (;;) {
        struct timespec ts;
        int stop_requested, already_paused;
        char buf[16];
        int breaked;
        msxdebug_stop_reason reason;
        void (*cb)(void *, msxdebug_stop_reason, uint16_t);
        void *ud;
        uint16_t pc;

        ts.tv_sec = 0;
        ts.tv_nsec = MSXDBG_POLL_INTERVAL_MS * 1000000L;
        nanosleep(&ts, NULL);

        pthread_mutex_lock(&d->mtx);
        stop_requested = d->poll_stop;
        already_paused = d->paused;
        pthread_mutex_unlock(&d->mtx);
        if (stop_requested) break;
        if (already_paused) continue; /* nothing new to notice */

        if (!msxhost_execute_sync("debug breaked", buf, sizeof(buf))) continue;
        breaked = (strcmp(buf, "1") == 0 || strcmp(buf, "true") == 0);
        if (!breaked) continue;

        pthread_mutex_lock(&d->mtx);
        if (d->paused) { /* lost the race to a synchronous caller */
            pthread_mutex_unlock(&d->mtx);
            continue;
        }
        d->paused = 1;
        reason = d->pending_reason;
        d->pending_reason = MSXDBG_STOP_BREAKPOINT;
        d->transient_bp_id[0] = '\0'; /* a fired -once bp already self-removed */
        cb = d->stop_cb;
        ud = d->stop_ud;
        pthread_mutex_unlock(&d->mtx);

        pc = read_pc_now();
        if (cb) cb(ud, reason, pc);
    }
    return NULL;
}

/* ---- lifecycle ---------------------------------------------------------- */

msxdebug *msxdebug_create(msxsession *s)
{
    msxdebug *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->session = s;
    pthread_mutex_init(&d->mtx, NULL);
    /* calloc zeroes this to MSXDBG_STOP_PAUSE (enum value 0), which is
     * wrong for the default case: an autonomous stop nobody explicitly
     * requested (a plain persistent breakpoint) must default to
     * MSXDBG_STOP_BREAKPOINT until step_over/step_out/run_to overrides it
     * for their own transient one-shot. */
    d->pending_reason = MSXDBG_STOP_BREAKPOINT;
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

/* ---- execution control -------------------------------------------------- */

void msxdebug_pause(msxdebug *d)
{
    uint16_t pc;
    void (*cb)(void *, msxdebug_stop_reason, uint16_t);
    void *ud;

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->paused) { pthread_mutex_unlock(&d->mtx); return; }
    remove_transient_bp_locked(d);
    msxhost_execute_sync("debug break", NULL, 0);
    pc = read_pc_now();
    d->paused = 1;
    cb = d->stop_cb;
    ud = d->stop_ud;
    pthread_mutex_unlock(&d->mtx);
    if (cb) cb(ud, MSXDBG_STOP_PAUSE, pc);
}

void msxdebug_resume(msxdebug *d)
{
    if (!d) return;
    /* d->mtx stays held across "debug cont" itself (safe: it is a
     * different mutex than msxhost_execute_sync's own g_rpc_mutex, no
     * deadlock risk) -- otherwise there is a window, after paused is set
     * to 0 but before the CPU has actually been told to continue, where
     * the poll thread can observe paused==0, immediately check "debug
     * breaked" (still true from the PRIOR break), and misreport a stale
     * stop at the old PC. Caught by debugger_test.c's autonomous-
     * breakpoint case reporting the pre-resume PC instead of the
     * breakpoint's address. */
    pthread_mutex_lock(&d->mtx);
    remove_transient_bp_locked(d);
    d->paused = 0;
    msxhost_execute_sync("debug cont", NULL, 0);
    pthread_mutex_unlock(&d->mtx);
}

int msxdebug_is_paused(msxdebug *d)
{
    int p;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    p = d->paused;
    pthread_mutex_unlock(&d->mtx);
    return p;
}

void msxdebug_step_into(msxdebug *d)
{
    uint16_t pc;
    void (*cb)(void *, msxdebug_stop_reason, uint16_t);
    void *ud;

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (!d->paused) { pthread_mutex_unlock(&d->mtx); return; }
    msxhost_execute_sync("debug step", NULL, 0);
    pc = read_pc_now();
    cb = d->stop_cb;
    ud = d->stop_ud;
    pthread_mutex_unlock(&d->mtx);
    if (cb) cb(ud, MSXDBG_STOP_STEP, pc);
}

/* Shared by step_over/step_out: arms a one-shot conditional breakpoint at
 * target (only triggering once the stack has unwound back to/above
 * sp_floor, so it survives recursion and any interrupt firing while the
 * callee runs -- the same SP-floor trick adam-desktop's exec-hook version
 * uses, expressed here as a Tcl breakpoint condition since openMSX has no
 * native step-over), then resumes. Asynchronous: the eventual stop is
 * delivered by the poll thread, like a persistent breakpoint firing. */
static void arm_transient_and_go(msxdebug *d, uint16_t target,
                                 uint16_t sp_floor, int strict,
                                 msxdebug_stop_reason reason)
{
    char cmd[96], idbuf[16];

    snprintf(cmd, sizeof(cmd), "debug set_bp -once 0x%04X {[reg SP] %s %u}",
            (unsigned)target, strict ? ">" : ">=", (unsigned)sp_floor);

    /* See msxdebug_resume()'s comment: hold d->mtx across "debug cont"
     * itself, not just the state update before it, to close the same
     * stale-stop race window. */
    pthread_mutex_lock(&d->mtx);
    remove_transient_bp_locked(d);
    if (msxhost_execute_sync(cmd, idbuf, sizeof(idbuf)))
        snprintf(d->transient_bp_id, sizeof(d->transient_bp_id), "%s", idbuf);
    d->pending_reason = reason;
    d->paused = 0;
    msxhost_execute_sync("debug cont", NULL, 0);
    pthread_mutex_unlock(&d->mtx);
}

void msxdebug_step_over(msxdebug *d)
{
    msxdebug_z80_regs r;
    uint8_t code[4] = {0, 0, 0, 0};
    z80d_insn insn;
    uint16_t pc, sp;

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (!d->paused) { pthread_mutex_unlock(&d->mtx); return; }
    pthread_mutex_unlock(&d->mtx);

    msxdebug_get_regs(d, &r);
    pc = (uint16_t)((r.pch << 8) | r.pcl);
    sp = (uint16_t)((r.sph << 8) | r.spl);
    msxdebug_read_mem(d, pc, code, 4);
    z80_disassemble(&insn, pc, code);

    if (!(insn.flags & (MSXDASM_CALL | MSXDASM_HALT | MSXDASM_BLOCK))) {
        /* A plain instruction: stepping into it IS stepping over it. */
        msxdebug_step_into(d);
        return;
    }
    arm_transient_and_go(d, (uint16_t)(pc + insn.len), sp, 0, MSXDBG_STOP_STEP);
}

void msxdebug_step_out(msxdebug *d)
{
    msxdebug_z80_regs r;
    uint8_t retbytes[2] = {0, 0};
    uint16_t sp, target;

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (!d->paused) { pthread_mutex_unlock(&d->mtx); return; }
    pthread_mutex_unlock(&d->mtx);

    msxdebug_get_regs(d, &r);
    sp = (uint16_t)((r.sph << 8) | r.spl);
    msxdebug_read_mem(d, sp, retbytes, 2);
    target = (uint16_t)(retbytes[0] | (retbytes[1] << 8));

    arm_transient_and_go(d, target, sp, 1, MSXDBG_STOP_STEP);
}

void msxdebug_run_to(msxdebug *d, uint16_t addr)
{
    char cmd[48], idbuf[16];

    if (!d) return;
    snprintf(cmd, sizeof(cmd), "debug set_bp -once 0x%04X", (unsigned)addr);
    /* See msxdebug_resume()'s comment: hold d->mtx across "debug cont"
     * itself to close the stale-stop race window. */
    pthread_mutex_lock(&d->mtx);
    remove_transient_bp_locked(d);
    if (msxhost_execute_sync(cmd, idbuf, sizeof(idbuf)))
        snprintf(d->transient_bp_id, sizeof(d->transient_bp_id), "%s", idbuf);
    d->pending_reason = MSXDBG_STOP_RUNTO;
    d->paused = 0;
    msxhost_execute_sync("debug cont", NULL, 0);
    pthread_mutex_unlock(&d->mtx);
}

void msxdebug_set_stop_callback(msxdebug *d,
                                void (*cb)(void *, msxdebug_stop_reason,
                                           uint16_t),
                                void *ud)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->stop_cb = cb;
    d->stop_ud = ud;
    pthread_mutex_unlock(&d->mtx);
}

/* ---- breakpoints ---------------------------------------------------------- */

void msxdebug_bp_set(msxdebug *d, uint16_t addr)
{
    char cmd[48], idbuf[16];
    int idx;

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (bp_find_locked(d, addr) >= 0 || d->bp_count >= MSXDBG_MAX_BP) {
        pthread_mutex_unlock(&d->mtx);
        return;
    }
    snprintf(cmd, sizeof(cmd), "debug set_bp 0x%04X", (unsigned)addr);
    if (!msxhost_execute_sync(cmd, idbuf, sizeof(idbuf))) {
        pthread_mutex_unlock(&d->mtx);
        return;
    }
    idx = d->bp_count++;
    d->bps[idx].addr = addr;
    snprintf(d->bps[idx].id, sizeof(d->bps[idx].id), "%s", idbuf);
    pthread_mutex_unlock(&d->mtx);
}

void msxdebug_bp_clear(msxdebug *d, uint16_t addr)
{
    char cmd[48];
    int idx;

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    idx = bp_find_locked(d, addr);
    if (idx < 0) { pthread_mutex_unlock(&d->mtx); return; }
    snprintf(cmd, sizeof(cmd), "debug remove_bp %s", d->bps[idx].id);
    msxhost_execute_sync(cmd, NULL, 0);
    d->bps[idx] = d->bps[d->bp_count - 1];
    d->bp_count--;
    pthread_mutex_unlock(&d->mtx);
}

void msxdebug_bp_toggle(msxdebug *d, uint16_t addr)
{
    if (msxdebug_bp_is_set(d, addr))
        msxdebug_bp_clear(d, addr);
    else
        msxdebug_bp_set(d, addr);
}

void msxdebug_bp_clear_all(msxdebug *d)
{
    int i;
    char cmd[48];

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->bp_count; i++) {
        snprintf(cmd, sizeof(cmd), "debug remove_bp %s", d->bps[i].id);
        msxhost_execute_sync(cmd, NULL, 0);
    }
    d->bp_count = 0;
    pthread_mutex_unlock(&d->mtx);
}

int msxdebug_bp_is_set(msxdebug *d, uint16_t addr)
{
    int found;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    found = bp_find_locked(d, addr) >= 0;
    pthread_mutex_unlock(&d->mtx);
    return found;
}

int msxdebug_bp_list(msxdebug *d, uint16_t *out, int max)
{
    int n, i;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    n = d->bp_count < max ? d->bp_count : max;
    for (i = 0; i < n; i++) out[i] = d->bps[i].addr;
    pthread_mutex_unlock(&d->mtx);
    return n;
}

/* ---- CPU / memory --------------------------------------------------------- */

void msxdebug_get_regs(msxdebug *d, msxdebug_z80_regs *out)
{
    int len = 0;
    (void)d;
    if (!msxhost_execute_sync_binary("debug read_block {CPU regs} 0 28",
                                     (uint8_t *)out, (int)sizeof(*out),
                                     &len) ||
        len < (int)sizeof(*out)) {
        memset(out, 0, sizeof(*out));
    }
}

void msxdebug_set_regs(msxdebug *d, const msxdebug_z80_regs *in)
{
    const uint8_t *bytes = (const uint8_t *)in;
    char cmd[48];
    int i;
    (void)d;
    for (i = 0; i < (int)sizeof(*in); i++) {
        snprintf(cmd, sizeof(cmd), "debug write {CPU regs} %d %u", i,
                (unsigned)bytes[i]);
        msxhost_execute_sync(cmd, NULL, 0);
    }
}

int msxdebug_read_mem(msxdebug *d, uint16_t addr, uint8_t *dst, int n)
{
    char cmd[64];
    int len = 0;
    (void)d;
    if (n <= 0) return 0;
    snprintf(cmd, sizeof(cmd), "debug read_block memory %u %d",
            (unsigned)addr, n);
    if (!msxhost_execute_sync_binary(cmd, dst, n, &len)) {
        memset(dst, 0, (size_t)n);
        return 0;
    }
    return len < n ? len : n;
}

int msxdebug_write_mem(msxdebug *d, uint16_t addr, const uint8_t *src, int n)
{
    char cmd[48];
    int i;
    (void)d;
    for (i = 0; i < n; i++) {
        snprintf(cmd, sizeof(cmd), "debug write memory %u %u",
                (unsigned)(addr + i), (unsigned)src[i]);
        msxhost_execute_sync(cmd, NULL, 0);
    }
    return n;
}

/* ---- disassembly ----------------------------------------------------------- */

int msxdebug_disassemble(msxdebug *d, uint16_t addr, int count,
                         msxdasm_line *out)
{
    int i;
    uint16_t pc = addr;

    if (!d) return 0;
    for (i = 0; i < count; i++) {
        uint8_t code[4] = {0, 0, 0, 0};
        z80d_insn insn;
        msxdasm_line *l = &out[i];
        uint16_t off = 0;
        const char *sym;

        msxdebug_read_mem(d, pc, code, 4);
        z80_disassemble(&insn, pc, code);
        l->addr = pc;
        l->len = insn.len;
        memcpy(l->bytes, insn.bytes, 4);
        memcpy(l->text, insn.text, sizeof(l->text));
        l->target = insn.target;
        l->flags = insn.flags;
        sym = symtabs_at(&d->syms, pc, &off);
        l->symbol = (sym && off == 0) ? sym : NULL;
        pc = (uint16_t)(pc + insn.len);
    }
    return count;
}

/* ---- symbols ---------------------------------------------------------------- */

int msxdebug_symbols_load(msxdebug *d, const char *path, const char *table)
{
    int rc;
    if (!d) return -1;
    pthread_mutex_lock(&d->mtx);
    rc = symtabs_load_file(&d->syms, path, table);
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
