/*
 * fujitrace_test -- the debugger's FujiNet trace decoder
 * (core/debugger/fujitrace.c) on made-up bytes: both directions, frames
 * split across feeds and mid-escape, DBC packets, folding of repeats, the
 * TX-drops-partial-RX rule, the ring's cap, and the empty state. No openMSX
 * and no FujiNet runtime: it runs anywhere.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fujitrace.h"

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond);       \
            failures++;                                                   \
        }                                                                 \
    } while (0)

/* A FujiBus packet (6-byte header, then payload), SLIP-framed. Returns the
 * framed length. */
static size_t frame(uint8_t *out, uint8_t device, uint8_t command,
                    const uint8_t *payload, size_t n)
{
    uint8_t raw[512];
    size_t len = 6 + n, i, o = 0;
    raw[0] = device;
    raw[1] = command;
    raw[2] = (uint8_t)(len & 0xFF);
    raw[3] = (uint8_t)(len >> 8);
    raw[4] = 0; /* checksum: the trace does not check it */
    raw[5] = 0;
    memcpy(raw + 6, payload, n);
    out[o++] = 0xC0;
    for (i = 0; i < len; i++) {
        if (raw[i] == 0xC0) { out[o++] = 0xDB; out[o++] = 0xDC; }
        else if (raw[i] == 0xDB) { out[o++] = 0xDB; out[o++] = 0xDD; }
        else out[o++] = raw[i];
    }
    out[o++] = 0xC0;
    return o;
}

/* The MSX writes its bytes one IO_PUTC at a time. */
static void feed_bytewise(fujitrace *t, int dir, const uint8_t *buf, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) fujitrace_feed(t, dir, buf + i, 1);
}

static msx_fuji_packet p[FUJITRACE_MAX];

int main(void)
{
    fujitrace *t = fujitrace_new();
    uint8_t req[600], rep[600], dbc[600], ack[64];
    size_t nreq, nrep, ndbc, nack, i;
    uint8_t payload[300];
    char state[1024];
    int n;

    /* Empty. */
    CHECK(fujitrace_packets(t, p, FUJITRACE_MAX) == 0);
    fujitrace_format_state(t, 0, 0, 65505, state, sizeof(state));
    CHECK(strstr(state, "FujiNet    off") != NULL);
    CHECK(strstr(state, "not connected to\n           127.0.0.1:65505") != NULL);
    CHECK(strstr(state, "0 bytes, 0 packets") != NULL);
    CHECK(strstr(state, "Trace      0 of 512 packets") != NULL);

    /* A request, byte by byte, and its reply in three pieces, one of them
     * cut between an escape and what it escapes. */
    nreq = frame(req, 0x70, 0xE8, NULL, 0);
    feed_bytewise(t, FUJITRACE_TX, req, nreq);
    for (i = 0; i < 140; i++) payload[i] = (uint8_t)i;
    payload[10] = 0xC0;
    payload[11] = 0xDB;
    nrep = frame(rep, 0x70, 0x01, payload, 140);
    fujitrace_feed(t, FUJITRACE_RX, rep, 5);
    fujitrace_feed(t, FUJITRACE_RX, rep + 5, 13); /* ends on the DB of DB DC */
    CHECK(rep[17] == 0xDB && rep[18] == 0xDC);
    fujitrace_feed(t, FUJITRACE_RX, rep + 18, nrep - 18);
    n = fujitrace_packets(t, p, FUJITRACE_MAX);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(p[0].to_host == 0 && p[0].device == 0x70 && p[0].command == 0xE8);
        CHECK(p[0].length == 6 && p[0].nbytes == 6 && p[0].repeat == 1);
        CHECK(strstr(p[0].text, "MSX \xe2\x86\x92 FujiNet 70 E8 GET ADAPTERCONFIG (6 bytes)") != NULL);
        CHECK(p[1].to_host == 1 && p[1].command == 0x01);
        CHECK(p[1].nbytes == 146 && p[1].length == 146);
        CHECK(p[1].data[16] == 0xC0 && p[1].data[17] == 0xDB);
        CHECK(strcmp(p[1].text, "FujiNet \xe2\x86\x92 MSX 70 01 (146 bytes)") == 0);
        CHECK(p[1].serial > p[0].serial);
    }

    /* The same exchange twice more folds into those two entries. */
    for (i = 0; i < 2; i++) {
        feed_bytewise(t, FUJITRACE_TX, req, nreq);
        fujitrace_feed(t, FUJITRACE_RX, rep, nrep);
    }
    n = fujitrace_packets(t, p, FUJITRACE_MAX);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(p[0].repeat == 3 && p[1].repeat == 3);
        CHECK(strstr(p[0].text, " \xc3\x97" "3") != NULL);
        CHECK(strstr(p[1].text, "(146 bytes) \xc3\x97" "3") != NULL);
    }

    /* One packet repeated on its own folds too; a different one does not. */
    fujitrace_clear(t);
    CHECK(fujitrace_packets(t, p, FUJITRACE_MAX) == 0);
    feed_bytewise(t, FUJITRACE_TX, req, nreq);
    feed_bytewise(t, FUJITRACE_TX, req, nreq);
    nreq = frame(req, 0x70, 0xF2, NULL, 0);
    feed_bytewise(t, FUJITRACE_TX, req, nreq);
    n = fujitrace_packets(t, p, FUJITRACE_MAX);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(p[0].repeat == 2 && p[1].repeat == 1);
        CHECK(strstr(p[1].text, "READ DEVICE SLOTS") != NULL);
    }

    /* A ROM push: FujiNet's DBC packet to the cartridge and the
     * cartridge's ack, which the MSX never sees. */
    fujitrace_clear(t);
    payload[0] = 0;
    payload[1] = 2;
    ndbc = frame(dbc, 0xFF, 0x4F, payload, 2);
    fujitrace_feed(t, FUJITRACE_RX, dbc, ndbc);
    nack = frame(ack, 0xFF, 0x06, NULL, 0);
    fujitrace_feed(t, FUJITRACE_TX_CART, ack, nack);
    n = fujitrace_packets(t, p, FUJITRACE_MAX);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(p[0].to_host == 1 && p[0].device == 0xFF);
        CHECK(strcmp(p[0].text, "FujiNet \xe2\x86\x92 cartridge FF 4F OPEN (DBC, 8 bytes)") == 0);
        CHECK(p[1].to_host == 0);
        CHECK(strcmp(p[1].text, "Cartridge \xe2\x86\x92 FujiNet FF 06 ACK (DBC, 6 bytes)") == 0);
    }

    /* A cartridge ack arriving while the MSX is mid-frame stays apart. */
    fujitrace_clear(t);
    feed_bytewise(t, FUJITRACE_TX, req, 4);
    fujitrace_feed(t, FUJITRACE_TX_CART, ack, nack);
    feed_bytewise(t, FUJITRACE_TX, req + 4, nreq - 4);
    n = fujitrace_packets(t, p, FUJITRACE_MAX);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(p[0].device == 0xFF && p[1].device == 0x70 && p[1].command == 0xF2);
        CHECK(p[1].nbytes == 6);
    }

    /* A TX byte throws away a partial RX frame. */
    fujitrace_clear(t);
    fujitrace_feed(t, FUJITRACE_RX, rep, 20);
    feed_bytewise(t, FUJITRACE_TX, req, nreq);
    fujitrace_feed(t, FUJITRACE_RX, rep + 20, nrep - 20);
    n = fujitrace_packets(t, p, FUJITRACE_MAX);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(p[0].to_host == 0);
        CHECK(p[1].to_host == 1 && p[1].nbytes < 146);
    }

    /* Short packets have no header. */
    fujitrace_clear(t);
    {
        const uint8_t junk[] = {0xC0, 0x01, 0x02, 0xC0, 0xC0};
        fujitrace_feed(t, FUJITRACE_RX, junk, sizeof(junk));
    }
    n = fujitrace_packets(t, p, FUJITRACE_MAX);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(p[0].device == -1 && p[0].command == -1 && p[0].length == -1);
        CHECK(strcmp(p[0].text, "FujiNet \xe2\x86\x92 MSX 2 bytes") == 0);
    }

    /* The ring keeps the newest FUJITRACE_MAX. */
    fujitrace_clear(t);
    for (i = 0; i < FUJITRACE_MAX + 88; i++) {
        payload[0] = (uint8_t)i;
        payload[1] = (uint8_t)(i >> 8);
        nreq = frame(req, 0x70, 0xF6, payload, 2);
        fujitrace_feed(t, FUJITRACE_TX, req, nreq);
    }
    n = fujitrace_packets(t, p, FUJITRACE_MAX);
    CHECK(n == FUJITRACE_MAX);
    if (n == FUJITRACE_MAX) {
        CHECK(p[0].data[6] == 88 && p[0].data[7] == 0);
        CHECK(p[n - 1].data[6] == (uint8_t)(FUJITRACE_MAX + 87));
    }
    n = fujitrace_packets(t, p, 3); /* the newest three */
    CHECK(n == 3 && p[2].data[6] == (uint8_t)(FUJITRACE_MAX + 87));

    /* The counts survive a clear; the packets do not. */
    fujitrace_clear(t);
    fujitrace_format_state(t, 1, 1, 65505, state, sizeof(state));
    CHECK(strstr(state, "FujiNet    running") != NULL);
    CHECK(strstr(state, "Cartridge  connected to\n") != NULL);
    CHECK(strstr(state, "Trace      0 of 512 packets") != NULL);
    CHECK(strstr(state, "1 of them DBC") != NULL);
    CHECK(strstr(state, "16 bytes, 2 DBC acks") != NULL);

    fujitrace_free(t);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("fujitrace_test: all checks passed\n");
    return 0;
}
