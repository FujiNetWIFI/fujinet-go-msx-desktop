/*
 * fujitrace -- the FujiNet cartridge's socket bytes, both ways, decoded
 * into FujiBus packets (SLIP frames) and kept in a 512-entry ring (internal
 * to the msxdebug engine). Nothing emulator-side in here: the session feeds
 * it from the openMSX hook (msx_host.hh's msxhost_set_fujinet_trace), and
 * core/tests/fujitrace_test.c feeds it made-up bytes.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MSXDEBUG_FUJITRACE_H
#define MSXDEBUG_FUJITRACE_H

#include <stddef.h>
#include <stdint.h>

#include "msxdebug.h"

/* Where the bytes came from (the hook's dir argument). */
#define FUJITRACE_RX      0 /* FujiNet -> MSX, as the socket delivers them */
#define FUJITRACE_TX      1 /* MSX -> FujiNet, the IO_PUTC bytes sent */
#define FUJITRACE_TX_CART 2 /* the cartridge's own DBC acks, whole frames */

#define FUJITRACE_MAX 512

typedef struct fujitrace fujitrace;

fujitrace *fujitrace_new(void);
void fujitrace_free(fujitrace *t);

/* Any thread. Bytes of one source in order; frames end at SLIP END (C0).
 * A TX byte throws away a partial RX frame. */
void fujitrace_feed(fujitrace *t, int dir, const uint8_t *buf, size_t len);

/* The newest max packets, oldest first. A folded entry's text ends " ×N". */
int  fujitrace_packets(fujitrace *t, msx_fuji_packet *out, int max);
void fujitrace_clear(fujitrace *t);

/* The right pane: link and counts as text lines. */
int  fujitrace_format_state(fujitrace *t, int running, int external, int link,
                            const char *where, char *dst, int dstsz);

#endif /* MSXDEBUG_FUJITRACE_H */
