/*
 * fujitrace -- see fujitrace.h.
 *
 * The decoder is fujinet-go-msdos-desktop's FujiTrace (core/src/debug.cpp)
 * in C, with three byte sources rather than two: the cartridge acks DBC
 * packets from its own reader thread, so those frames get an accumulator
 * of their own instead of landing in the middle of an MSX frame being
 * written a byte at a time from the openMSX thread. Two rules come from the
 * family's shared design: identical exchanges in a row fold into one entry
 * with a repeat count, and a TX byte throws away a partial RX frame.
 *
 * Only a packet's first 64 bytes are kept; its length and a hash of all of
 * it are what the folding compares.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fujitrace.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuji_names.h"

#define FUJI_DEVICE_DBC 0xFF /* openMSX src/serial/fujiDeviceID.h */

#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

#define FNV_OFFSET 1469598103934665603ULL
#define FNV_PRIME  1099511628211ULL

#define ARROW "\xe2\x86\x92"

typedef struct {
    msx_fuji_packet p; /* text without the repeat count */
    int src;           /* FUJITRACE_* */
    uint64_t hash;     /* FNV-1a over all nbytes */
} entry;

/* One source's frame so far. */
typedef struct {
    uint8_t head[64];
    int n;
    uint64_t hash;
    int esc;
} frame;

struct fujitrace {
    pthread_mutex_t mtx;
    entry ring[FUJITRACE_MAX];
    int first, count;
    uint64_t serial;
    frame acc[3];
    uint64_t bytes[3], packets[3];
    uint64_t dbc_in;
};

static const char *command_name(int id)
{
    size_t i;
    for (i = 0; i < sizeof(fuji_command_names) / sizeof(fuji_command_names[0]); i++)
        if (fuji_command_names[i].id == id) return fuji_command_names[i].name;
    return "";
}

static void frame_reset(frame *f)
{
    f->n = 0;
    f->hash = FNV_OFFSET;
    f->esc = 0;
}

static void frame_add(frame *f, uint8_t b)
{
    if (f->n < (int)sizeof(f->head)) f->head[f->n] = b;
    f->n++;
    f->hash = (f->hash ^ b) * FNV_PRIME;
}

/* i < 0 counts back from the newest (-1). */
static entry *nth(fujitrace *t, int i)
{
    return &t->ring[(t->first + t->count + i) % FUJITRACE_MAX];
}

static int same(const entry *a, const entry *b)
{
    const int n = a->p.nbytes < 64 ? a->p.nbytes : 64;
    return a->src == b->src && a->p.nbytes == b->p.nbytes &&
           a->hash == b->hash && memcmp(a->p.data, b->p.data, (size_t)n) == 0;
}

/* A repeat of the newest packet, or of the newest two (a request and its
 * reply), folds into the entries it repeats. */
static void fold(fujitrace *t)
{
    if (t->count >= 2 && same(nth(t, -1), nth(t, -2))) {
        nth(t, -2)->p.repeat += nth(t, -1)->p.repeat;
        nth(t, -2)->p.serial = nth(t, -1)->p.serial;
        t->count--;
    } else if (t->count >= 4 && same(nth(t, -1), nth(t, -3)) &&
               same(nth(t, -2), nth(t, -4))) {
        nth(t, -4)->p.repeat += nth(t, -2)->p.repeat;
        nth(t, -3)->p.repeat += nth(t, -1)->p.repeat;
        nth(t, -3)->p.serial = nth(t, -1)->p.serial;
        t->count -= 2;
    }
}

/* A SLIP END: the source's frame so far is a packet (nothing, between
 * frames). */
static void emit(fujitrace *t, int src)
{
    frame *f = &t->acc[src];
    const int n = f->n;
    const char *who;
    entry *e;
    int dbc;

    if (n == 0) {
        frame_reset(f);
        return;
    }
    if (t->count == FUJITRACE_MAX) {
        t->first = (t->first + 1) % FUJITRACE_MAX;
        t->count--;
    }
    e = &t->ring[(t->first + t->count) % FUJITRACE_MAX];
    t->count++;
    memset(e, 0, sizeof(*e));
    e->src = src;
    e->hash = f->hash;
    e->p.serial = ++t->serial;
    e->p.to_host = src == FUJITRACE_RX;
    e->p.nbytes = n;
    e->p.repeat = 1;
    memcpy(e->p.data, f->head, (size_t)(n < 64 ? n : 64));
    e->p.device = n >= 6 ? f->head[0] : -1;
    e->p.command = n >= 6 ? f->head[1] : -1;
    e->p.length = n >= 6 ? (f->head[2] | (f->head[3] << 8)) : -1;
    dbc = e->p.device == FUJI_DEVICE_DBC;
    t->packets[src]++;
    if (dbc && src == FUJITRACE_RX) t->dbc_in++;

    if (src == FUJITRACE_TX) who = "MSX " ARROW " FujiNet";
    else if (src == FUJITRACE_TX_CART) who = "Cartridge " ARROW " FujiNet";
    else if (dbc) who = "FujiNet " ARROW " cartridge";
    else who = "FujiNet " ARROW " MSX";
    if (e->p.device < 0) {
        snprintf(e->p.text, sizeof(e->p.text), "%s %d bytes", who, n);
    } else {
        /* Requests are named; a reply's command byte is not a request. */
        const char *name = (!e->p.to_host || dbc) ? command_name(e->p.command) : "";
        snprintf(e->p.text, sizeof(e->p.text), "%s %02X %02X%s%s (%s%d bytes)",
                 who, e->p.device, e->p.command, *name ? " " : "", name,
                 dbc ? "DBC, " : "", n);
    }
    frame_reset(f);
    fold(t);
}

fujitrace *fujitrace_new(void)
{
    fujitrace *t = calloc(1, sizeof(*t));
    int i;
    if (!t) return NULL;
    pthread_mutex_init(&t->mtx, NULL);
    for (i = 0; i < 3; i++) frame_reset(&t->acc[i]);
    return t;
}

void fujitrace_free(fujitrace *t)
{
    if (!t) return;
    pthread_mutex_destroy(&t->mtx);
    free(t);
}

void fujitrace_feed(fujitrace *t, int dir, const uint8_t *buf, size_t len)
{
    frame *f;
    size_t i;

    if (!t || dir < FUJITRACE_RX || dir > FUJITRACE_TX_CART || !len) return;
    pthread_mutex_lock(&t->mtx);
    f = &t->acc[dir];
    t->bytes[dir] += len;
    if (dir == FUJITRACE_TX) frame_reset(&t->acc[FUJITRACE_RX]);
    for (i = 0; i < len; i++) {
        const uint8_t b = buf[i];
        if (b == SLIP_END) {
            emit(t, dir);
        } else if (f->esc) {
            frame_add(f, b == SLIP_ESC_END ? SLIP_END : b == SLIP_ESC_ESC ? SLIP_ESC : b);
            f->esc = 0;
        } else if (b == SLIP_ESC) {
            f->esc = 1;
        } else {
            frame_add(f, b);
        }
    }
    pthread_mutex_unlock(&t->mtx);
}

int fujitrace_packets(fujitrace *t, msx_fuji_packet *out, int max)
{
    int n, i;

    if (!t || max <= 0) return 0;
    pthread_mutex_lock(&t->mtx);
    n = t->count < max ? t->count : max;
    for (i = 0; i < n; i++) {
        msx_fuji_packet *p = &out[i];
        *p = nth(t, i - n)->p;
        if (p->repeat > 1) {
            size_t l = strlen(p->text);
            snprintf(p->text + l, sizeof(p->text) - l, " \xc3\x97%u", (unsigned)p->repeat);
        }
    }
    pthread_mutex_unlock(&t->mtx);
    return n;
}

void fujitrace_clear(fujitrace *t)
{
    if (!t) return;
    pthread_mutex_lock(&t->mtx);
    t->first = 0;
    t->count = 0;
    pthread_mutex_unlock(&t->mtx);
}

int fujitrace_format_state(fujitrace *t, int running, int external, int link,
                           const char *where, char *dst, int dstsz)
{
    unsigned long long bytes[3] = {0, 0, 0}, packets[3] = {0, 0, 0}, dbc = 0;
    int count = 0, i, n;

    if (!dst || dstsz <= 0) return 0;
    if (t) {
        pthread_mutex_lock(&t->mtx);
        for (i = 0; i < 3; i++) {
            bytes[i] = t->bytes[i];
            packets[i] = t->packets[i];
        }
        dbc = t->dbc_in;
        count = t->count;
        pthread_mutex_unlock(&t->mtx);
    }
    /* Narrow lines: the pane is about 36 columns. */
    n = snprintf(dst, (size_t)dstsz,
                 "FujiNet    %s\n"
                 "Cartridge  %s\n"
                 "           %s\n"
                 "\n"
                 "MSX " ARROW " FujiNet\n"
                 "  %llu bytes, %llu packets\n"
                 "FujiNet " ARROW " MSX\n"
                 "  %llu bytes, %llu packets\n"
                 "  %llu of them DBC: a ROM pushed\n"
                 "  into the cartridge, never\n"
                 "  seen by the MSX\n"
                 "Cartridge " ARROW " FujiNet\n"
                 "  %llu bytes, %llu DBC acks\n"
                 "\n"
                 "Trace      %d of %d packets\n",
                 external ? "external" : running ? "running" : "off",
                 link ? "connected to" : "not connected to", where,
                 bytes[FUJITRACE_TX], packets[FUJITRACE_TX],
                 bytes[FUJITRACE_RX], packets[FUJITRACE_RX], dbc,
                 bytes[FUJITRACE_TX_CART], packets[FUJITRACE_TX_CART],
                 count, FUJITRACE_MAX);
    if (n < 0) return 0;
    return n < dstsz ? n : dstsz - 1;
}
