/*
 * msxsession internals shared between the session and (from M3/M4/M6
 * onward) the FujiNet, settings, paths and debugger translation units.
 * Nothing here is API; frontends must only include msxsession.h / msxdebug.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MSXSESSION_INTERNAL_H
#define MSXSESSION_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "msxsession.h"

#define MSX_PATH_MAX 1024

#define MSX_FB_PIXELS \
    (MSXSESSION_FB_WIDTH * MSXSESSION_FB_HEIGHT)

struct msxsession {
    /* ---- resolved paths ---- */
    char config_dir[MSX_PATH_MAX];
    char data_dir[MSX_PATH_MAX];
    char openmsx_share[MSX_PATH_MAX];
    char roms_dir[MSX_PATH_MAX];   /* imported turboR/real-machine ROMs (M4) */
    char carts_dir[MSX_PATH_MAX];  /* imported cartridge images (M4) */
    char fujinet_lib[MSX_PATH_MAX];   /* "" = unavailable/disabled (M3) */
    char fujinet_src[MSX_PATH_MAX];   /* provisioning source override */
    char fujinet_root[MSX_PATH_MAX];
    char fujinet_config[MSX_PATH_MAX];
    char fujinet_sd[MSX_PATH_MAX];
    char fujinet_data[MSX_PATH_MAX];
    char webui_url[64];

    /* ---- lifecycle ---- */
    pthread_mutex_t lifecycle_mtx;
    volatile int running;
    msxsession_start_opts opts;
    char cur_machine[128];

    /* ---- latest-frame store (openMSX thread -> UI thread) ----
     * Fixed size -- see msxsession.h's note on why MSX has no
     * variable-geometry frame handling, unlike the sibling targets. */
    pthread_mutex_t frame_mtx;
    uint32_t *frame; /* MSX_FB_PIXELS XRGB8888, heap: ~1.2MB */
    uint64_t frame_serial;

    /* ---- vsync (advisory only; see msxsession_notify_vsync) ---- */
    int64_t vs_ns;
    long vs_iv_us;

    /* ---- FujiNet runtime (fujinet_runtime.c, M3) ---- */
    int fujinet_loaded;
    int fujinet_running;

    /* ---- debugger (M6) ---- */
    msxdebug *debugger;

    char last_error[512];
};

/* session.c */
void session_set_error(msxsession *s, const char *fmt, ...);

/* settings.c (M4) */
void settings_init(msxsession *s);
void settings_free_all(msxsession *s);

/* paths.c (M4) */
int  paths_init(msxsession *s, const msxsession_paths *p);
int  paths_provision_fujinet(msxsession *s); /* fills fujinet_* or "" */

/* fujinet_runtime.c (M3) */
int  fujinet_start(msxsession *s);
int  fujinet_wait_for_boip(msxsession *s, int timeout_ms);
void fujinet_stop(msxsession *s);
int  fujinet_copy_log(msxsession *s, char *dst, int max);

/* debugger.c (M6) */
msxdebug *msxdebug_create(msxsession *s);
void msxdebug_destroy(msxdebug *d);

#endif /* MSXSESSION_INTERNAL_H */
