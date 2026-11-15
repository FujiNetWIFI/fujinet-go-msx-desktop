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
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "../openmsx/msx_host.hh"
#include "../src/session_internal.h"
#include "debugger_internal.h"
#include "expr.h"
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
    char oid2[16];             /* an Access entry's second watchpoint (the write half) */
} bp_entry;

struct msxdebug {
    msxsession *session;
    pthread_mutex_t mtx;
    pthread_t poll_thread;
    int poll_thread_running;
    int poll_stop;
    pthread_cond_t wake;               /* poll thread sleeps here while idle */

    int attached;
    int started;                       /* the machine has been brought up */
    int paused;                        /* our own last-known state */
    unsigned gen;
    char reason[128];
    int reason_addr;
    uint64_t serial;                   /* bumped by every stop */
    msxdebug_stop_info stop;           /* the structured form of the same stop */
    int trace_want, hist_want;         /* the Trace / History tabs want the CPU ring */

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

static void apply_trace_locked(msxdebug *dbg, int force);

/* ---- small helpers --------------------------------------------------- */

static int rpc(const char *cmd, char *out, int max)
{
    if (out && max > 0) out[0] = '\0';
    if (!msxhost_is_running()) {
        /* nothing would answer, and the host would take its full 5 s to say so */
        if (out && max > 0) snprintf(out, (size_t)max, "openMSX is not running");
        return 0;
    }
    return msxhost_execute_sync(cmd, out, max);
}

int msxdebug_rpc(const char *command, char *out, int outsz)
{
    return rpc(command, out, outsz);
}

int msxdebug_rpcf(char *out, int outsz, const char *fmt, ...)
{
    char command[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(command, sizeof(command), fmt, args);
    va_end(args);
    return rpc(command, out, outsz);
}

msxsession *msxdebug_session(msxdebug *dbg)
{
    return dbg ? dbg->session : NULL;
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

/* Records a stop: the text the older API gives, and the structured form the
 * newer one does. The caller holds d->mtx. */
static void note_stop_info_locked(msxdebug *dbg, const msxdebug_stop_info *info, const char *reason)
{
    snprintf(dbg->reason, sizeof(dbg->reason), "%s", reason);
    dbg->reason_addr = info->pc;
    dbg->gen++;
    dbg->serial++;
    dbg->stop = *info;
    dbg->stop.serial = dbg->serial;
}

static void note_stop_locked(msxdebug *dbg, int kind, const char *reason, int addr)
{
    msxdebug_stop_info info;
    memset(&info, 0, sizeof(info));
    info.reason = kind;
    info.addr = (uint16_t)addr;
    info.pc = (uint16_t)addr;
    info.value = -1;
    info.old_value = -1;
    info.from_pc = -1;
    note_stop_info_locked(dbg, &info, reason);
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

/* ---- conditions --------------------------------------------------------------- */

/* A condition is one of the debugger's expressions (msxexpr), turned into the
 * Tcl expression openMSX evaluates when the breakpoint is hit; a text that is
 * not that grammar is taken as an openMSX Tcl expression as it stands, which
 * is what conditions have always been here. */

typedef struct {
    msxdebug *dbg;
    int type;            /* the breakpoint type the condition belongs to */
    int checking;        /* a trial run: no access variables exist yet, so they read 0 */
    int misuse;          /* it named value, addr or old on an execute breakpoint */
} cond_ctx;

static const struct { const char *name; const char *tcl; } k_tcl_names[] = {
    { "a", "[reg A]" }, { "f", "[reg F]" }, { "b", "[reg B]" }, { "c", "[reg C]" },
    { "d", "[reg D]" }, { "e", "[reg E]" }, { "h", "[reg H]" }, { "l", "[reg L]" },
    { "af", "[reg AF]" }, { "bc", "[reg BC]" }, { "de", "[reg DE]" }, { "hl", "[reg HL]" },
    { "ix", "[reg IX]" }, { "iy", "[reg IY]" }, { "sp", "[reg SP]" }, { "pc", "[reg PC]" },
    { "ixh", "[reg IXH]" }, { "ixl", "[reg IXL]" }, { "iyh", "[reg IYH]" }, { "iyl", "[reg IYL]" },
    { "i", "[reg I]" }, { "r", "[reg R]" }, { "im", "[reg IM]" },
    { "af'", "[reg AF2]" }, { "bc'", "[reg BC2]" }, { "de'", "[reg DE2]" }, { "hl'", "[reg HL2]" },
    { "iff1", "[reg IFF] & 1" }, { "iff2", "([reg IFF] >> 1) & 1" },
    { "cf", "[reg F] & 1" }, { "nf", "([reg F] >> 1) & 1" }, { "pf", "([reg F] >> 2) & 1" },
    { "xf", "([reg F] >> 3) & 1" }, { "hf", "([reg F] >> 4) & 1" }, { "yf", "([reg F] >> 5) & 1" },
    { "zf", "([reg F] >> 6) & 1" }, { "sf", "([reg F] >> 7) & 1" },
    { "frame", "[machine_info VDP_frame_count]" },
    { "vline", "[machine_info VDP_line_in_frame]" },
    { "cycles", "int([machine_info time] * [machine_info z80_freq])" },
};

static void lower_copy(char *dst, int dstsz, const char *src)
{
    int idx = 0;
    for (; src[idx] && idx + 1 < dstsz; idx++)
        dst[idx] = (char)tolower((unsigned char)src[idx]);
    dst[idx] = '\0';
}

static int tcl_name(void *user, const char *name, char *out, int outsz)
{
    cond_ctx *ctx = user;
    char low[64];
    uint16_t found;
    size_t idx;

    lower_copy(low, (int)sizeof(low), name);
    for (idx = 0; idx < sizeof(k_tcl_names) / sizeof(k_tcl_names[0]); idx++)
        if (!strcmp(low, k_tcl_names[idx].name)) {
            snprintf(out, (size_t)outsz, "%s", k_tcl_names[idx].tcl);
            return 1;
        }
    if (!strcmp(low, "value") || !strcmp(low, "addr") || !strcmp(low, "old")) {
        const int is_port = (ctx->type & (MSXDEBUG_BP_IN | MSXDEBUG_BP_OUT)) != 0;
        char value[160];
        if (ctx->type == MSXDEBUG_BP_EXEC) {
            ctx->misuse = 1;
            return 0;
        }
        if (ctx->checking) {
            snprintf(out, (size_t)outsz, "0");
            return 1;
        }
        /* a write sets wp_last_value; a read has no value of its own, so the
         * byte is looked at now. A port read has none at all. */
        snprintf(value, sizeof(value), is_port
                 ? "[info exists ::wp_last_value] ? $::wp_last_value : 0"
                 : "[info exists ::wp_last_value] ? $::wp_last_value : [peek $::wp_last_address]");
        if (!strcmp(low, "addr"))
            snprintf(out, (size_t)outsz, "$::wp_last_address");
        else if (!strcmp(low, "value"))
            snprintf(out, (size_t)outsz, "%s", value);
        else
            snprintf(out, (size_t)outsz, "[info exists ::wp_last_old] ? $::wp_last_old : (%s)", value);
        return 1;
    }
    if (ctx->dbg && symtabs_find(&ctx->dbg->syms, name, &found)) {
        snprintf(out, (size_t)outsz, "%u", (unsigned)found);
        return 1;
    }
    return 0;
}

/* Does a text that is not the debugger's grammar read as Tcl: a command or
 * variable substitution, or a math function? Anything else is a typo in the
 * grammar, and says so. */
static int looks_like_tcl(const char *text)
{
    const char *ptr;
    for (ptr = text; *ptr; ptr++) {
        if (*ptr == '[' || *ptr == '$') return 1;
        if (*ptr == '(' && ptr > text && isalpha((unsigned char)ptr[-1])) return 1;
    }
    return 0;
}

/* The Tcl expression for a breakpoint's condition into out. Returns 1 when
 * it is empty or fine, 0 with the reason in err. checking: the trial run at
 * add time. */
static int build_condition(msxdebug *dbg, int type, const char *text, int checking,
                           char *out, int outsz, char *err, int errsz)
{
    cond_ctx ctx;
    msxexpr_tcl_ctx tcl_ctx;
    char grammar_err[96] = "";

    out[0] = '\0';
    if (!text || !*text) return 1;
    memset(&ctx, 0, sizeof(ctx));
    ctx.dbg = dbg;
    ctx.type = type;
    ctx.checking = checking;
    tcl_ctx.name = tcl_name;
    tcl_ctx.user = &ctx;
    if (msxexpr_to_tcl(text, &tcl_ctx, out, outsz, grammar_err, (int)sizeof(grammar_err)) == 0)
        return 1;
    if (ctx.misuse) {
        if (err && errsz > 0)
            snprintf(err, (size_t)errsz, "value, addr and old belong to read and write conditions");
        return 0;
    }
    /* not the debugger's grammar: an openMSX Tcl expression, as given */
    if (!looks_like_tcl(text) || strchr(text, '{') || strchr(text, '}')) {
        if (err && errsz > 0)
            snprintf(err, (size_t)errsz, "%s", grammar_err[0] ? grammar_err : "no braces in a Tcl condition");
        return 0;
    }
    if ((int)strlen(text) >= outsz) {
        if (err && errsz > 0) snprintf(err, (size_t)errsz, "the condition is too long");
        return 0;
    }
    snprintf(out, (size_t)outsz, "%s", text);
    return 1;
}

/* ---- arming ------------------------------------------------------------------ */

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

static char type_letter(int type)
{
    switch (type) {
    case MSXDEBUG_BP_READ:  return 'R';
    case MSXDEBUG_BP_WRITE: return 'W';
    case MSXDEBUG_BP_IN:    return 'I';
    default:                return 'O';
    }
}

/* Arms one openMSX watchpoint of a single type for entry b. */
static int arm_watch_locked(bp_entry *bpt, int type, const char *tcl_cond, char *oid, char *err, int errsz)
{
    char cmd[2048], res[160], action[400], range[48];
    int ok;

    /* the action counts the hit, says which entry fired, and keeps what was
     * touched: openMSX drops the access variables when the action returns */
    snprintf(action, sizeof(action),
             "incr ::fngo_hit(%d); set ::fngo_last %d; "
             "set ::fngo_acc [list %c $::wp_last_address "
             "[expr {[info exists ::wp_last_value] ? $::wp_last_value : -1}] "
             "[expr {[info exists ::wp_last_old] ? $::wp_last_old : -1}]]; debug break",
             bpt->id, bpt->id, type_letter(type));
    if (bpt->end > bpt->start)
        snprintf(range, sizeof(range), "{0x%04X 0x%04X}", (unsigned)bpt->start, (unsigned)bpt->end);
    else
        snprintf(range, sizeof(range), "0x%04X", (unsigned)bpt->start);
    snprintf(cmd, sizeof(cmd), "debug set_watchpoint %s %s {%s} {%s}",
             type_word(type), range, tcl_cond, action);
    ok = rpc(cmd, res, sizeof(res));
    if (!ok) {
        if (err && errsz > 0) snprintf(err, (size_t)errsz, "%s", res[0] ? res : "openMSX refused it");
        return 0;
    }
    snprintf(oid, 16, "%s", res);
    return 1;
}

/* Arms one entry in openMSX; on failure the reason goes to err. Caller
 * holds d->mtx. */
static int arm_locked(msxdebug *dbg, bp_entry *bpt, char *err, int errsz)
{
    char cmd[2048], res[160], action[96], tcl_cond[1024];

    if (bpt->oid[0]) return 1;
    if (!build_condition(dbg, bpt->type, bpt->cond, 0, tcl_cond, (int)sizeof(tcl_cond), err, errsz))
        return 0;
    if (bpt->type == MSXDEBUG_BP_EXEC) {
        snprintf(action, sizeof(action),
                 "incr ::fngo_hit(%d); set ::fngo_last %d; debug break", bpt->id, bpt->id);
        snprintf(cmd, sizeof(cmd), "debug set_bp 0x%04X {%s} {%s}",
                 (unsigned)bpt->start, tcl_cond, action);
        if (!rpc(cmd, res, sizeof(res))) {
            if (err && errsz > 0) snprintf(err, (size_t)errsz, "%s", res[0] ? res : "openMSX refused it");
            return 0;
        }
        snprintf(bpt->oid, sizeof(bpt->oid), "%s", res);
        return 1;
    }
    if ((bpt->type & MSXDEBUG_BP_ACCESS) == MSXDEBUG_BP_ACCESS) {
        if (!arm_watch_locked(bpt, MSXDEBUG_BP_READ, tcl_cond, bpt->oid, err, errsz)) return 0;
        if (!arm_watch_locked(bpt, MSXDEBUG_BP_WRITE, tcl_cond, bpt->oid2, err, errsz)) {
            rpcf(NULL, 0, "debug remove_watchpoint %s", bpt->oid);
            bpt->oid[0] = '\0';
            return 0;
        }
        return 1;
    }
    return arm_watch_locked(bpt, bpt->type, tcl_cond, bpt->oid, err, errsz);
}

static void disarm_locked(bp_entry *bpt)
{
    if (bpt->oid[0]) {
        if (strncmp(bpt->oid, "bp#", 3) == 0)
            rpcf(NULL, 0, "debug remove_bp %s", bpt->oid);
        else
            rpcf(NULL, 0, "debug remove_watchpoint %s", bpt->oid);
        bpt->oid[0] = '\0';
    }
    if (bpt->oid2[0]) {
        rpcf(NULL, 0, "debug remove_watchpoint %s", bpt->oid2);
        bpt->oid2[0] = '\0';
    }
}

static void arm_all_locked(msxdebug *dbg)
{
    int idx;
    for (idx = 0; idx < dbg->bp_count; idx++) {
        bp_entry *bpt = &dbg->bps[idx];
        bpt->oid[0] = '\0';
        bpt->oid2[0] = '\0';
        if (bpt->enabled && !arm_locked(dbg, bpt, NULL, 0))
            bpt->enabled = 0;     /* a condition openMSX will not take */
    }
}

static void disarm_all_locked(msxdebug *dbg)
{
    int idx;
    for (idx = 0; idx < dbg->bp_count; idx++) disarm_locked(&dbg->bps[idx]);
    remove_transient_locked(dbg);
}

/* ---- stop discovery ------------------------------------------------------- */

static const char *kind_word(int type)
{
    switch (type) {
    case MSXDEBUG_BP_READ:   return "read";
    case MSXDEBUG_BP_WRITE:  return "write";
    case MSXDEBUG_BP_IN:     return "in";
    case MSXDEBUG_BP_ACCESS: return "access";
    default:                 return "out";
    }
}

static void describe_bp(bp_entry *bpt, uint16_t pc, char *dst, int dstsz)
{
    switch (bpt->type) {
    case MSXDEBUG_BP_EXEC:
        snprintf(dst, (size_t)dstsz, "breakpoint %d at $%04X", bpt->id, (unsigned)bpt->start);
        break;
    default:
        if (bpt->end > bpt->start)
            snprintf(dst, (size_t)dstsz, "watchpoint %d: %s $%04X-$%04X (PC $%04X)", bpt->id,
                     kind_word(bpt->type), (unsigned)bpt->start, (unsigned)bpt->end, (unsigned)pc);
        else
            snprintf(dst, (size_t)dstsz, "watchpoint %d: %s $%04X (PC $%04X)", bpt->id,
                     kind_word(bpt->type), (unsigned)bpt->start, (unsigned)pc);
    }
}

/* What a watchpoint's action left behind: the entry that fired and what was
 * touched (kind R, W, I, O; value and old are -1 when openMSX does not say). */
typedef struct {
    int bp_id;
    char kind;
    int addr, value, old;
} hit_info;

/* Reads and clears which breakpoint last fired (bp_id 0: none). */
static void take_last(hit_info *hit)
{
    char buf[96];
    int addr = 0, value = -1, old = -1;
    char kind = 0;

    memset(hit, 0, sizeof(*hit));
    hit->value = hit->old = -1;
    if (!rpc("set ::fngo_res [list [expr {[info exists ::fngo_last] ? $::fngo_last : 0}] "
             "[expr {[info exists ::fngo_acc] ? $::fngo_acc : {}}]]; "
             "unset -nocomplain ::fngo_last ::fngo_acc; set ::fngo_res", buf, sizeof(buf)))
        return;
    hit->bp_id = atoi(buf);
    if (sscanf(buf, "%*d {%c %d %d %d}", &kind, &addr, &value, &old) == 4) {
        hit->kind = kind;
        hit->addr = addr;
        hit->value = value;
        hit->old = old;
    }
}

/* The instruction that ran last, from the trace ring when it is on; -1. */
static int last_instruction_pc(void)
{
    MsxCpuTraceRec rec;
    unsigned count;
    if (!msxhost_trace_is_enabled()) return -1;
    count = msxhost_trace_count();
    if (!count || msxhost_trace_read(count - 1, &rec, 1) != 1) return -1;
    return rec.pc;
}

/* Fills a stop record for a watchpoint that fired. Caller holds d->mtx. */
static void apply_hit_locked(msxdebug_stop_info *info, const hit_info *hit)
{
    switch (hit->kind) {
    case 'R': info->reason = MSXDEBUG_STOP_WATCH_READ; break;
    case 'W': info->reason = MSXDEBUG_STOP_WATCH_WRITE; break;
    case 'I': info->reason = MSXDEBUG_STOP_WATCH_IN; break;
    case 'O': info->reason = MSXDEBUG_STOP_WATCH_OUT; break;
    default:  return;
    }
    info->addr = (uint16_t)hit->addr;
    info->value = hit->value;
    info->old_value = hit->old;
    if (hit->kind == 'R' && info->value < 0) {
        /* the access watch fires as the byte is read: it is still there */
        char res[16];
        if (rpcf(res, sizeof(res), "debug read memory %d", hit->addr & 0xFFFF))
            info->value = atoi(res) & 0xFF;
    }
    if (hit->kind == 'R' || hit->kind == 'W')
        info->old_value = hit->kind == 'W' ? info->old_value : info->value;
    info->from_pc = last_instruction_pc();
}

/* Caller holds d->mtx. Every place that can end an idle state signals
 * d->wake (attach, machine_changed, resume, the run-to-target start). */
static int poll_idle_locked(const msxdebug *d)
{
    return d->paused || !d->started || (!d->attached && !d->transient_id[0]);
}

static void *poll_thread_fn(void *arg)
{
    msxdebug *d = arg;
    for (;;) {
        struct timespec ts;
        int stop_requested, idle;
        char buf[16];

        /* Nothing to watch for (not attached, paused, or no machine yet):
         * sleep until a state change signals d->wake, instead of waking
         * every 40 ms for the life of the session. */
        pthread_mutex_lock(&d->mtx);
        while (!d->poll_stop && poll_idle_locked(d))
            pthread_cond_wait(&d->wake, &d->mtx);
        pthread_mutex_unlock(&d->mtx);

        ts.tv_sec = 0;
        ts.tv_nsec = MSXDBG_POLL_INTERVAL_MS * 1000000L;
        nanosleep(&ts, NULL);

        pthread_mutex_lock(&d->mtx);
        stop_requested = d->poll_stop;
        idle = poll_idle_locked(d);
        pthread_mutex_unlock(&d->mtx);
        if (stop_requested) break;
        if (idle) continue;

        if (!rpc("debug breaked", buf, sizeof(buf))) continue;
        if (strcmp(buf, "1") != 0 && strcmp(buf, "true") != 0) continue;

        {
            void (*cb)(void *, int, uint16_t);
            void *ud;
            char reason[128];
            hit_info hit;
            msxdebug_stop_info info;
            int kind = MSXDBG_STOP_BREAKPOINT;
            uint16_t pc;

            pthread_mutex_lock(&d->mtx);
            if (d->paused) {            /* lost the race to a synchronous caller */
                pthread_mutex_unlock(&d->mtx);
                continue;
            }
            take_last(&hit);
            pc = read_pc_now();
            d->paused = 1;
            memset(&info, 0, sizeof(info));
            info.addr = pc;
            info.pc = pc;
            info.value = info.old_value = info.from_pc = -1;
            if (hit.bp_id) {
                bp_entry *entry = bp_by_id(d, hit.bp_id);
                info.bp_id = hit.bp_id;
                if (entry) describe_bp(entry, pc, reason, sizeof(reason));
                else       snprintf(reason, sizeof(reason), "breakpoint %d", hit.bp_id);
                if (entry && entry->type == MSXDEBUG_BP_EXEC) info.reason = MSXDEBUG_STOP_BREAKPOINT;
                else apply_hit_locked(&info, &hit);
                if (!info.reason) info.reason = MSXDEBUG_STOP_BREAKPOINT;
            } else if (d->pending_reason) {
                snprintf(reason, sizeof(reason), "%s", d->pending_reason);
                kind = MSXDBG_STOP_STEP;
                info.reason = strcmp(d->pending_reason, "frame") == 0
                                  ? MSXDEBUG_STOP_FRAME : MSXDEBUG_STOP_ONESHOT;
            } else {
                snprintf(reason, sizeof(reason), "stopped");
                info.reason = MSXDEBUG_STOP_PAUSE;
            }
            d->pending_reason = NULL;
            d->transient_id[0] = '\0';  /* a fired -once bp already self-removed */
            note_stop_info_locked(d, &info, reason);
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
    pthread_cond_init(&d->wake, NULL);
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
    pthread_cond_signal(&d->wake);
    pthread_mutex_unlock(&d->mtx);
    if (d->poll_thread_running) pthread_join(d->poll_thread, NULL);
    symtabs_free(&d->syms);
    free(d->user_syms);
    pthread_cond_destroy(&d->wake);
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
    note_stop_locked(d, MSXDEBUG_STOP_PAUSE, why, pc);
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
    pthread_cond_signal(&d->wake);
    pthread_mutex_unlock(&d->mtx);
}

void msxdebug_detach(msxdebug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (!d->attached) { pthread_mutex_unlock(&d->mtx); return; }
    d->attached = 0;
    d->trace_want = d->hist_want = 0;       /* the tabs went with the window */
    apply_trace_locked(d, 0);
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
    /* openMSX's cputrace setting belongs to one machine: set it again */
    if (d->started && (d->trace_want || d->hist_want)) apply_trace_locked(d, 1);
    if (power_cycled) {
        /* a new machine: its breakpoints and run state are gone */
        int i;
        for (i = 0; i < d->bp_count; i++) {
            d->bps[i].oid[0] = '\0';
            d->bps[i].oid2[0] = '\0';
        }
        d->transient_id[0] = '\0';
        d->paused = 0;
        d->pending_reason = NULL;
        if (d->attached && d->started) {
            arm_all_locked(d);
            stop_locked(d, "stopped at power-up");
        }
    }
    d->gen++;
    pthread_cond_signal(&d->wake);
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
    pthread_cond_signal(&d->wake);
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
    {
        /* "debug step" returns before the instruction has run; with the ring
         * on, its record is what says it did */
        const int traced = msxhost_trace_is_enabled();
        const uint64_t before = msxhost_trace_total();
        int waited;
        rpc("debug step", NULL, 0);
        for (waited = 0; traced && waited < 500 && msxhost_trace_total() == before; waited++) {
            struct timespec nap = { 0, 1000000L };
            nanosleep(&nap, NULL);
        }
    }
    pc = read_pc_now();
    note_stop_locked(d, MSXDEBUG_STOP_STEP, "step", pc);
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
    pthread_cond_signal(&d->wake);
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
    if (!msxhost_is_running()) return;
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
    if (msxhost_trace_is_enabled()) {
        MsxCpuTraceRec last;
        const unsigned held = msxhost_trace_count();
        if (held && msxhost_trace_read(held - 1, &last, 1) == 1) out->halted = last.halted;
    }
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
    if (!msxhost_is_running()) {
        memset(dst, 0xFF, (size_t)n);
        return 0;
    }
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

/* ---- expressions ------------------------------------------------------------- */

const char *const msxdebug_reg_names[] = {
    "a", "f", "b", "c", "d", "e", "h", "l", "af", "bc", "de", "hl", "ix", "iy",
    "sp", "pc", "ixh", "ixl", "iyh", "iyl", "i", "r", "im", "iff1", "iff2",
    "af'", "bc'", "de'", "hl'", "cf", "nf", "pf", "xf", "hf", "yf", "zf", "sf",
    "frame", "cycles", "vline", "value", "old", "addr", NULL,
};

/* A register or flag as a slice of a msxdebug_cpu field. */
static const struct { const char *name; size_t offset; int shift; int mask; } k_cpu_names[] = {
    { "pc", offsetof(msxdebug_cpu, pc), 0, 0xFFFF }, { "sp", offsetof(msxdebug_cpu, sp), 0, 0xFFFF },
    { "af", offsetof(msxdebug_cpu, af), 0, 0xFFFF }, { "bc", offsetof(msxdebug_cpu, bc), 0, 0xFFFF },
    { "de", offsetof(msxdebug_cpu, de), 0, 0xFFFF }, { "hl", offsetof(msxdebug_cpu, hl), 0, 0xFFFF },
    { "ix", offsetof(msxdebug_cpu, ix), 0, 0xFFFF }, { "iy", offsetof(msxdebug_cpu, iy), 0, 0xFFFF },
    { "af'", offsetof(msxdebug_cpu, af2), 0, 0xFFFF }, { "bc'", offsetof(msxdebug_cpu, bc2), 0, 0xFFFF },
    { "de'", offsetof(msxdebug_cpu, de2), 0, 0xFFFF }, { "hl'", offsetof(msxdebug_cpu, hl2), 0, 0xFFFF },
    { "a", offsetof(msxdebug_cpu, af), 8, 0xFF }, { "f", offsetof(msxdebug_cpu, af), 0, 0xFF },
    { "b", offsetof(msxdebug_cpu, bc), 8, 0xFF }, { "c", offsetof(msxdebug_cpu, bc), 0, 0xFF },
    { "d", offsetof(msxdebug_cpu, de), 8, 0xFF }, { "e", offsetof(msxdebug_cpu, de), 0, 0xFF },
    { "h", offsetof(msxdebug_cpu, hl), 8, 0xFF }, { "l", offsetof(msxdebug_cpu, hl), 0, 0xFF },
    { "ixh", offsetof(msxdebug_cpu, ix), 8, 0xFF }, { "ixl", offsetof(msxdebug_cpu, ix), 0, 0xFF },
    { "iyh", offsetof(msxdebug_cpu, iy), 8, 0xFF }, { "iyl", offsetof(msxdebug_cpu, iy), 0, 0xFF },
    { "i", offsetof(msxdebug_cpu, i), 0, 0xFF }, { "r", offsetof(msxdebug_cpu, r), 0, 0xFF },
    { "im", offsetof(msxdebug_cpu, im), 0, 3 },
    { "iff1", offsetof(msxdebug_cpu, iff1), 0, 1 }, { "iff2", offsetof(msxdebug_cpu, iff2), 0, 1 },
    { "cf", offsetof(msxdebug_cpu, af), 0, 1 }, { "nf", offsetof(msxdebug_cpu, af), 1, 1 },
    { "pf", offsetof(msxdebug_cpu, af), 2, 1 }, { "xf", offsetof(msxdebug_cpu, af), 3, 1 },
    { "hf", offsetof(msxdebug_cpu, af), 4, 1 }, { "yf", offsetof(msxdebug_cpu, af), 5, 1 },
    { "zf", offsetof(msxdebug_cpu, af), 6, 1 }, { "sf", offsetof(msxdebug_cpu, af), 7, 1 },
};

typedef struct {
    msxdebug *dbg;
    int have_cpu;
    msxdebug_cpu cpu;
} eval_ctx;

static uint8_t eval_read_byte(void *user, uint16_t addr)
{
    eval_ctx *ctx = user;
    uint8_t byte = 0xFF;
    msxdebug_read(ctx->dbg, addr, &byte, 1);
    return byte;
}

static int eval_name(void *user, const char *name, long *out)
{
    eval_ctx *ctx = user;
    char low[64], text[48];
    uint16_t found;
    size_t idx;

    lower_copy(low, (int)sizeof(low), name);
    if (!strcmp(low, "value") || !strcmp(low, "addr") || !strcmp(low, "old")) {
        /* the access the machine is stopped at, if it stopped at one */
        msxdebug_stop_info info;
        if (!msxdebug_last_stop(ctx->dbg, &info) || !msxdebug_is_stopped(ctx->dbg)) return 0;
        if (info.reason < MSXDEBUG_STOP_WATCH_READ || info.reason > MSXDEBUG_STOP_WATCH_OUT) return 0;
        if (!strcmp(low, "addr")) *out = info.addr;
        else if (!strcmp(low, "value")) *out = info.value < 0 ? 0 : info.value;
        else *out = info.old_value < 0 ? (info.value < 0 ? 0 : info.value) : info.old_value;
        return 1;
    }
    for (idx = 0; idx < sizeof(k_cpu_names) / sizeof(k_cpu_names[0]); idx++)
        if (!strcmp(low, k_cpu_names[idx].name)) {
            if (!ctx->have_cpu) {
                msxdebug_cpu_get(ctx->dbg, &ctx->cpu);
                ctx->have_cpu = 1;
            }
            *out = (*(const int *)((const char *)&ctx->cpu + k_cpu_names[idx].offset) >> k_cpu_names[idx].shift)
                   & k_cpu_names[idx].mask;
            return 1;
        }
    if (!strcmp(low, "frame")) {
        *out = rpc("machine_info VDP_frame_count", text, sizeof(text)) ? atol(text) : 0;
        return 1;
    }
    if (!strcmp(low, "vline")) {
        *out = rpc("machine_info VDP_line_in_frame", text, sizeof(text)) ? atol(text) : 0;
        return 1;
    }
    if (!strcmp(low, "cycles")) {
        *out = rpc("expr {int([machine_info time] * [machine_info z80_freq])}", text, sizeof(text))
                   ? atol(text) : 0;
        return 1;
    }
    if (symtabs_find(&ctx->dbg->syms, name, &found)) {
        *out = found;
        return 1;
    }
    return 0;
}

int msxdebug_eval(msxdebug *dbg, const char *text, long *out, char *err, int errsz)
{
    eval_ctx ctx;
    msxexpr_ctx expr;

    if (err && errsz > 0) err[0] = '\0';
    if (!dbg) {
        if (err && errsz > 0) snprintf(err, (size_t)errsz, "no debugger");
        return -1;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.dbg = dbg;
    expr.name = eval_name;
    expr.read_byte = eval_read_byte;
    expr.user = &ctx;
    return msxexpr_eval(text, &expr, out, err, errsz);
}

/* ---- the stop, in words -------------------------------------------------------- */

static int format_stop(const msxdebug_stop_info *info, char *dst, int dstsz)
{
    char value[24] = "", was[24] = "";
    const unsigned pc = info->pc;

    if (info->value >= 0) snprintf(value, sizeof(value), " = $%02X", info->value);
    if (info->old_value >= 0 && info->reason == MSXDEBUG_STOP_WATCH_WRITE)
        snprintf(was, sizeof(was), " (was $%02X)", info->old_value);
    switch (info->reason) {
    case MSXDEBUG_STOP_PAUSE:       return snprintf(dst, (size_t)dstsz, "paused at $%04X", pc);
    case MSXDEBUG_STOP_STEP:        return snprintf(dst, (size_t)dstsz, "step at $%04X", pc);
    case MSXDEBUG_STOP_FRAME:       return snprintf(dst, (size_t)dstsz, "frame at $%04X", pc);
    case MSXDEBUG_STOP_BREAKPOINT:  return snprintf(dst, (size_t)dstsz, "breakpoint: exec $%04X", pc);
    case MSXDEBUG_STOP_WATCH_WRITE:
        return snprintf(dst, (size_t)dstsz, "wrote $%04X%s%s at $%04X", info->addr, value, was, pc);
    case MSXDEBUG_STOP_WATCH_READ:
        return snprintf(dst, (size_t)dstsz, "read $%04X%s at $%04X", info->addr, value, pc);
    case MSXDEBUG_STOP_WATCH_IN:
        return snprintf(dst, (size_t)dstsz, "in $%02X%s at $%04X", info->addr & 0xFF, value, pc);
    case MSXDEBUG_STOP_WATCH_OUT:
        return snprintf(dst, (size_t)dstsz, "out $%02X%s at $%04X", info->addr & 0xFF, value, pc);
    default:                        return snprintf(dst, (size_t)dstsz, "stopped at $%04X", pc);
    }
}

int msxdebug_stop_text(msxdebug *dbg, char *dst, int dstsz, int *address)
{
    int len;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (address) *address = -1;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    if (dbg->paused) {
        len = format_stop(&dbg->stop, dst, dstsz);
        if (address) *address = dbg->stop.pc;
    } else {
        len = snprintf(dst, (size_t)dstsz, "running");
    }
    pthread_mutex_unlock(&dbg->mtx);
    return len < dstsz ? len : dstsz - 1;
}

int msxdebug_last_stop(msxdebug *dbg, msxdebug_stop_info *out)
{
    int paused;
    if (!dbg || !out) return 0;
    pthread_mutex_lock(&dbg->mtx);
    *out = dbg->stop;
    paused = dbg->paused;
    pthread_mutex_unlock(&dbg->mtx);
    return paused ? 1 : 0;
}

uint64_t msxdebug_stop_serial(msxdebug *dbg)
{
    uint64_t serial;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    serial = dbg->serial;
    pthread_mutex_unlock(&dbg->mtx);
    return serial;
}

int msxdebug_stop_snapshot(msxdebug *dbg, msxdebug_snapshot *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->address = -1;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    out->paused = dbg->paused;
    out->serial = dbg->serial;
    out->stop = dbg->stop;
    if (dbg->paused) {
        format_stop(&dbg->stop, out->text, (int)sizeof(out->text));
        out->address = dbg->stop.pc;
    } else {
        snprintf(out->text, sizeof(out->text), "running");
    }
    pthread_mutex_unlock(&dbg->mtx);
    return out->paused;
}

/* ---- registers, in bulk ---------------------------------------------------------- */

int msxdebug_regs_set(msxdebug *dbg, const msxdebug_cpu *regs, char *err, int errsz)
{
    char cmd[400], note[16];
    int len, idx, answered;
    const int bytes[28] = {
        regs ? regs->af >> 8 : 0, regs ? regs->af : 0, regs ? regs->bc >> 8 : 0, regs ? regs->bc : 0,
        regs ? regs->de >> 8 : 0, regs ? regs->de : 0, regs ? regs->hl >> 8 : 0, regs ? regs->hl : 0,
        regs ? regs->af2 >> 8 : 0, regs ? regs->af2 : 0, regs ? regs->bc2 >> 8 : 0, regs ? regs->bc2 : 0,
        regs ? regs->de2 >> 8 : 0, regs ? regs->de2 : 0, regs ? regs->hl2 >> 8 : 0, regs ? regs->hl2 : 0,
        regs ? regs->ix >> 8 : 0, regs ? regs->ix : 0, regs ? regs->iy >> 8 : 0, regs ? regs->iy : 0,
        regs ? regs->pc >> 8 : 0, regs ? regs->pc : 0, regs ? regs->sp >> 8 : 0, regs ? regs->sp : 0,
        regs ? regs->i : 0, regs ? regs->r : 0, regs ? regs->im : 0,
        regs ? ((regs->iff1 ? 1 : 0) | (regs->iff2 ? 2 : 0)) : 0,
    };

    if (err && errsz > 0) err[0] = '\0';
    if (!dbg || !regs) return 0;
    if (regs->im < 0 || regs->im > 2) {
        if (err && errsz > 0) snprintf(err, (size_t)errsz, "IM is 0, 1 or 2");
        return 0;
    }
    pthread_mutex_lock(&dbg->mtx);
    if (!dbg->started || !dbg->paused) {
        pthread_mutex_unlock(&dbg->mtx);
        if (err && errsz > 0) snprintf(err, (size_t)errsz, "the machine is running (stop it first)");
        return 0;
    }
    len = snprintf(cmd, sizeof(cmd), "debug write_block {CPU regs} 0 [binary format c28 {");
    for (idx = 0; idx < 28; idx++)
        len += snprintf(cmd + len, sizeof(cmd) - (size_t)len, "%d ", bytes[idx] & 0xFF);
    snprintf(cmd + len, sizeof(cmd) - (size_t)len, "}]");
    answered = rpc(cmd, note, sizeof(note));
    if (!answered && err && errsz > 0) snprintf(err, (size_t)errsz, "openMSX refused the registers");
    dbg->gen++;
    pthread_mutex_unlock(&dbg->mtx);
    return answered;
}

/* ---- hit counts ---------------------------------------------------------------- */

void msxdebug_breakpoint_clear_hits(msxdebug *dbg)
{
    if (!dbg) return;
    pthread_mutex_lock(&dbg->mtx);
    rpc("array unset ::fngo_hit", NULL, 0);
    dbg->gen++;
    pthread_mutex_unlock(&dbg->mtx);
}

/* ---- the symbol list ------------------------------------------------------------ */

int msxdebug_symbols_count(msxdebug *dbg)
{
    int total = 0, idx;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    for (idx = 0; idx < dbg->syms.count; idx++) total += dbg->syms.tables[idx].count;
    pthread_mutex_unlock(&dbg->mtx);
    return total;
}

/* Caller holds d->mtx. */
static const sym_entry *symbol_entry_locked(const msxdebug *dbg, int index)
{
    int idx;
    if (index < 0) return NULL;
    for (idx = 0; idx < dbg->syms.count; idx++) {
        const sym_table *table = &dbg->syms.tables[idx];
        if (index < table->count) return &table->entries[index];
        index -= table->count;
    }
    return NULL;
}

int msxdebug_symbols_get(msxdebug *dbg, int index, uint16_t *addr, char *name, int namesz)
{
    const sym_entry *entry;
    int found = 0;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    entry = symbol_entry_locked(dbg, index);
    if (entry) {
        if (addr) *addr = entry->addr;
        if (name && namesz > 0) snprintf(name, (size_t)namesz, "%s", entry->name);
        found = 1;
    }
    pthread_mutex_unlock(&dbg->mtx);
    return found;
}

typedef struct { uint16_t addr; const char *name; int index; } sym_pick;

static int sym_pick_cmp(const void *left, const void *right)
{
    const sym_pick *lhs = left, *rhs = right;
    if (lhs->addr != rhs->addr) return lhs->addr < rhs->addr ? -1 : 1;
    return strcasecmp(lhs->name, rhs->name);
}

static int contains_nocase(const char *hay, const char *needle)
{
    const size_t len = strlen(needle);
    if (!len) return 1;
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, len)) return 1;
    return 0;
}

int msxdebug_symbols_filter(msxdebug *dbg, const char *text, int *out, int max)
{
    sym_pick *pick;
    int total, count = 0, idx, base = 0, table_idx;

    if (!dbg || !out || max <= 0) return 0;
    total = msxdebug_symbols_count(dbg);
    if (total <= 0) return 0;
    pick = malloc((size_t)total * sizeof(*pick));
    if (!pick) return 0;
    pthread_mutex_lock(&dbg->mtx);
    for (table_idx = 0; table_idx < dbg->syms.count; table_idx++) {
        const sym_table *table = &dbg->syms.tables[table_idx];
        for (idx = 0; idx < table->count && count < total; idx++)
            if (contains_nocase(table->entries[idx].name, text ? text : "")) {
                pick[count].addr = table->entries[idx].addr;
                pick[count].name = table->entries[idx].name;
                pick[count].index = base + idx;
                count++;
            }
        base += table->count;
    }
    qsort(pick, (size_t)count, sizeof(*pick), sym_pick_cmp);
    pthread_mutex_unlock(&dbg->mtx);
    if (count > max) count = max;
    for (idx = 0; idx < count; idx++) out[idx] = pick[idx].index;
    free(pick);
    return count;
}

void msxdebug_clear_symbols(msxdebug *dbg)
{
    if (!dbg) return;
    pthread_mutex_lock(&dbg->mtx);
    symtabs_free(&dbg->syms);
    free(dbg->user_syms);
    dbg->user_syms = NULL;
    symtabs_load_text(&dbg->syms, msxdebug_builtin_bios_sym, "bios");
    dbg->gen++;
    pthread_mutex_unlock(&dbg->mtx);
}

const sym_tables *msxdebug_symtabs(msxdebug *dbg)
{
    return dbg ? &dbg->syms : NULL;
}

int msxdebug_tcl(msxdebug *dbg, const char *command, char *out, int outsz)
{
    int answered;
    if (!dbg) return 0;
    /* under the engine's lock so the poll thread cannot see a stop this
     * command made (a "debug cont" typed at the console) half way */
    pthread_mutex_lock(&dbg->mtx);
    answered = rpc(command, out, outsz);
    dbg->gen++;
    pthread_mutex_unlock(&dbg->mtx);
    return answered;
}

void msxdebug_touch(msxdebug *dbg)
{
    if (!dbg) return;
    pthread_mutex_lock(&dbg->mtx);
    dbg->gen++;
    pthread_mutex_unlock(&dbg->mtx);
}

/* ---- search ------------------------------------------------------------------------ */

static int match_at(const uint8_t *mem, unsigned addr, const uint8_t *pat, int count, uint8_t mask, int flags)
{
    int idx;
    for (idx = 0; idx < count; idx++) {
        uint8_t have = mem[addr + (unsigned)idx];
        uint8_t want = pat[idx];
        if (flags & MSXDEBUG_SEARCH_NOCASE) {
            if (have >= 'a' && have <= 'z') have = (uint8_t)(have - 32);
            if (want >= 'a' && want <= 'z') want = (uint8_t)(want - 32);
        }
        if ((have & mask) != (want & mask)) return 0;
    }
    return 1;
}

/* The CPU's 64K as mapped now; NULL unless stopped. */
static uint8_t *snapshot_64k(msxdebug *dbg)
{
    uint8_t *mem;
    if (!msxdebug_is_stopped(dbg)) return NULL;
    mem = malloc(0x10000);
    if (!mem) return NULL;
    if (msxdebug_read(dbg, 0, mem, 0x10000) != 0x10000) {
        free(mem);
        return NULL;
    }
    return mem;
}

static int search_buf(const uint8_t *mem, const uint8_t *pat, int count, uint8_t mask, int flags,
                      unsigned from, unsigned to, uint16_t *out, int max)
{
    unsigned addr;
    int found = 0;
    for (addr = from; addr <= to && addr + (unsigned)count - 1 <= 0xFFFF && found < max; addr++)
        if (match_at(mem, addr, pat, count, mask, flags)) out[found++] = (uint16_t)addr;
    return found;
}

int msxdebug_search(msxdebug *dbg, const uint8_t *pat, int count, uint8_t mask, int flags,
                    uint16_t from, uint16_t to, uint16_t *out, int max)
{
    uint8_t *mem;
    int found;

    if (!dbg || !pat || count <= 0 || !out || max <= 0) return -1;
    mem = snapshot_64k(dbg);
    if (!mem) return -1;
    if (from > to) { uint16_t swap = from; from = to; to = swap; }
    found = search_buf(mem, pat, count, mask, flags, from, to, out, max);
    free(mem);
    return found;
}

int msxdebug_search_next(msxdebug *dbg, const uint8_t *pat, int count, uint8_t mask, int flags,
                         uint16_t from, int backward, uint16_t *found)
{
    uint8_t *mem;
    uint16_t hit = 0;
    int result = 0;

    if (!dbg || !pat || count <= 0 || !found) return -1;
    mem = snapshot_64k(dbg);
    if (!mem) return -1;
    if (!backward) {
        /* after `from` to the end, then 0..from (which includes `from`) */
        if ((unsigned)from + 1 <= 0xFFFF &&
            search_buf(mem, pat, count, mask, flags, (unsigned)from + 1, 0xFFFF, &hit, 1))
            result = 1;
        else if (search_buf(mem, pat, count, mask, flags, 0, from, &hit, 1))
            result = 1;
    } else {
        /* the nearest match before `from`, else the last one from `from` up */
        int pass;
        for (pass = 0; pass < 2 && !result; pass++) {
            const int start = pass == 0 ? (int)from - 1 : 0xFFFF;
            const int stop = pass == 0 ? 0 : (int)from;
            int addr;
            for (addr = start; addr >= stop; addr--)
                if ((unsigned)addr + (unsigned)count - 1 <= 0xFFFF &&
                    match_at(mem, (unsigned)addr, pat, count, mask, flags)) {
                    hit = (uint16_t)addr;
                    result = 1;
                    break;
                }
        }
    }
    if (result) *found = hit;
    free(mem);
    return result;
}

/* ---- the trace ring --------------------------------------------------------------- */

/* Puts the CPU ring in the state the tabs want. Caller holds d->mtx. */
static void apply_trace_locked(msxdebug *dbg, int force)
{
    const int want = dbg->trace_want || dbg->hist_want;
    if (want) {
        if (dbg->started && (force || !msxhost_trace_is_enabled())) msxhost_trace_enable(1);
    } else if (msxhost_trace_is_enabled()) {
        msxhost_trace_enable(0);
    }
}

void msxdebug_trace_enable(msxdebug *dbg, int enable)
{
    if (!dbg) return;
    pthread_mutex_lock(&dbg->mtx);
    dbg->trace_want = enable != 0;
    apply_trace_locked(dbg, 0);
    dbg->gen++;
    pthread_mutex_unlock(&dbg->mtx);
}

int msxdebug_trace_enabled(msxdebug *dbg)
{
    return dbg && dbg->trace_want && msxhost_trace_is_enabled();
}

void msxdebug_history_enable(msxdebug *dbg, int enable)
{
    if (!dbg) return;
    pthread_mutex_lock(&dbg->mtx);
    dbg->hist_want = enable != 0;
    apply_trace_locked(dbg, 0);
    dbg->gen++;
    pthread_mutex_unlock(&dbg->mtx);
}

int msxdebug_history_enabled(msxdebug *dbg)
{
    return dbg && dbg->hist_want && msxhost_trace_is_enabled();
}

static void trace_convert(const MsxCpuTraceRec *src, msxdebug_trace *dst)
{
    z80d_insn insn;
    uint8_t code[4];
    int idx;

    memset(dst, 0, sizeof(*dst));
    dst->pc = src->pc;
    dst->sp = src->sp;
    dst->af = src->af;
    dst->bc = src->bc;
    dst->de = src->de;
    dst->hl = src->hl;
    dst->ix = src->ix;
    dst->iy = src->iy;
    dst->af2 = src->af2;
    dst->bc2 = src->bc2;
    dst->de2 = src->de2;
    dst->hl2 = src->hl2;
    dst->reg_i = src->reg_i;
    dst->reg_r = src->reg_r;
    dst->im = src->im;
    dst->iff = src->iff;
    dst->halted = src->halted;
    dst->cycles = src->cycle;
    for (idx = 0; idx < 4; idx++) {
        dst->bytes[idx] = src->op[idx];
        code[idx] = src->op[idx];
    }
    z80_disassemble(&insn, src->pc, code);
    dst->len = insn.len;
}

/* The newest `max` records, oldest first, into out. Returns how many. */
static int ring_newest(msxdebug_trace *out, int max)
{
    MsxCpuTraceRec *recs;
    unsigned count, take, idx;

    if (max <= 0) return 0;
    count = msxhost_trace_count();
    take = count < (unsigned)max ? count : (unsigned)max;
    if (!take) return 0;
    recs = malloc(take * sizeof(*recs));
    if (!recs) return 0;
    take = msxhost_trace_read(count - take, recs, take);
    for (idx = 0; idx < take; idx++) trace_convert(&recs[idx], &out[idx]);
    free(recs);
    return (int)take;
}

int msxdebug_trace_read(msxdebug *dbg, msxdebug_trace *out, int max)
{
    int count, idx;
    (void)dbg;
    count = ring_newest(out, max);
    for (idx = 0; idx < count / 2; idx++) {     /* newest first */
        msxdebug_trace swap = out[idx];
        out[idx] = out[count - 1 - idx];
        out[count - 1 - idx] = swap;
    }
    return count;
}

void msxdebug_trace_clear(msxdebug *dbg)
{
    msxhost_trace_clear();
    msxdebug_touch(dbg);
}

int msxdebug_trace_count(msxdebug *dbg)
{
    (void)dbg;
    return (int)msxhost_trace_count();
}

uint64_t msxdebug_trace_total(msxdebug *dbg)
{
    (void)dbg;
    return msxhost_trace_total();
}

int msxdebug_trace_format(const msxdebug_trace *rec, char *dst, int dstsz)
{
    z80d_insn insn;

    if (!rec || !dst || dstsz <= 0) return 0;
    z80_disassemble(&insn, rec->pc, rec->bytes);
    return snprintf(dst, (size_t)dstsz, "%04X  %-18s AF=%04X BC=%04X DE=%04X HL=%04X SP=%04X",
                    rec->pc, insn.text, rec->af, rec->bc, rec->de, rec->hl, rec->sp);
}

int msxdebug_trace_match(const msxdebug_trace *rec, int filter, uint16_t lo, uint16_t hi)
{
    z80d_insn insn;

    if (!rec) return 0;
    if (filter == MSXDEBUG_TRACE_RANGE) {
        if (lo > hi) { uint16_t swap = lo; lo = hi; hi = swap; }
        return rec->pc >= lo && rec->pc <= hi;
    }
    if (filter == MSXDEBUG_TRACE_BRANCHES) {
        z80_disassemble(&insn, rec->pc, rec->bytes);
        return (insn.flags & (MSXDASM_JUMP | MSXDASM_CALL | MSXDASM_RET)) != 0;
    }
    return 1;
}

void msxdebug_history_clear(msxdebug *dbg)
{
    msxdebug_trace_clear(dbg);
}

uint64_t msxdebug_history_count(msxdebug *dbg)
{
    return msxdebug_trace_total(dbg);
}

int msxdebug_history_copy(msxdebug *dbg, msxdebug_hist *out, int max)
{
    (void)dbg;
    return ring_newest(out, max);
}

int msxdebug_history_format(const msxdebug_hist *rec, char *dst, int dstsz)
{
    z80d_insn insn;
    char flags[9];
    static const char k_names[] = "SZYHXPNC";
    int idx;

    if (!rec || !dst || dstsz <= 0) return 0;
    z80_disassemble(&insn, rec->pc, rec->bytes);
    for (idx = 0; idx < 8; idx++)
        flags[idx] = ((rec->af >> (7 - idx)) & 1) ? k_names[idx] : '.';
    flags[8] = '\0';
    return snprintf(dst, (size_t)dstsz,
                    "%04X  %-18s AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X SP=%04X  %s%s",
                    rec->pc, insn.text, rec->af, rec->bc, rec->de, rec->hl, rec->ix, rec->iy, rec->sp,
                    flags, rec->halted ? " HALT" : "");
}

int msxdebug_history_match(const msxdebug_hist *rec, int filter, uint16_t lo, uint16_t hi)
{
    return msxdebug_trace_match(rec, filter, lo, hi);
}

/* ---- the Video tab's console text ------------------------------------------------- */

static const char *(*g_video_text)(const char *section);

void msxdebug_set_video_text(const char *(*callback)(const char *section))
{
    g_video_text = callback;
}

const char *(*msxdebug_video_text_hook(void))(const char *section)
{
    return g_video_text;
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
        type != MSXDEBUG_BP_WRITE && type != MSXDEBUG_BP_IN && type != MSXDEBUG_BP_OUT &&
        type != MSXDEBUG_BP_ACCESS) {
        if (err) snprintf(err, (size_t)errsz, "unknown breakpoint type");
        return -1;
    }
    if (end < start) end = start;
    if ((type == MSXDEBUG_BP_IN || type == MSXDEBUG_BP_OUT) && end > 0xFF) {
        if (err) snprintf(err, (size_t)errsz, "ports are $00-$FF");
        return -1;
    }
    if (condition && *condition) {
        /* openMSX only evaluates a condition when the breakpoint is hit, so
         * a typo would sit silent: turn it into Tcl and try it once now (the
         * access values read 0 in the trial) */
        char tcl[1024], res[160], cmd[1100];
        if (!build_condition(d, type, condition, 1, tcl, (int)sizeof(tcl), err, errsz))
            return -1;
        if (d->started) {
            snprintf(cmd, sizeof(cmd), "expr {%s}", tcl);
            pthread_mutex_lock(&d->mtx);
            if (!rpc(cmd, res, sizeof(res))) {
                pthread_mutex_unlock(&d->mtx);
                if (err && errsz > 0)
                    snprintf(err, (size_t)errsz, "%s", res[0] ? res : "bad condition");
                return -1;
            }
            pthread_mutex_unlock(&d->mtx);
        }
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
