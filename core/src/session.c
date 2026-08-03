/*
 * msxsession -- session lifecycle and the latest-frame store.
 *
 * Unlike the sibling targets' session.c, there is no paced emulator loop
 * here to drive: core/openmsx/msx_host.hh's msxhost_core_start() spawns
 * openMSX's own thread, which paces itself (throttle on) and pushes frames
 * asynchronously through the frame sink whenever it finishes one -- see
 * msxsession.h's note on msxsession_notify_vsync being advisory only. This
 * file is therefore a thin lifecycle wrapper: start/stop openMSX, store the
 * latest frame for the UI thread to pull, and translate key/joystick input.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "compat.h"
#include "msx_host.hh"
#include "session_internal.h"

/****************************************************************************/
/** Small helpers                                                          **/
/****************************************************************************/

void session_set_error(msxsession *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_error, sizeof(s->last_error), fmt, ap);
    va_end(ap);
    fprintf(stderr, "msxsession: %s\n", s->last_error);
}

static int dir_has_file(const char *dir, const char *file)
{
    char path[MSX_PATH_MAX];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    return stat(path, &st) == 0;
}

/* Resolves the openMSX runtime share tree openMSX boots from
 * (OPENMSX_SYSTEM_DATA): init.tcl + scripts/ + machines/ (incl. C-BIOS) +
 * extensions/ (incl. FujiNet.xml + fujinet-config.rom). Search order:
 * caller override, $MSX_OPENMSX_SHARE, the executable's own directory (a
 * packaged install lays "share/" beside the binary), the install datadir
 * baked in at configure time, then the dev build's raw output
 * (tools/openmsx/work/out/share). A fuller paths.c (XDG dirs, ROM
 * materialisation) lands in M4; this is deliberately self-contained until
 * then. */
static int resolve_openmsx_share(msxsession *s, const char *override_dir)
{
    const char *env;
    const char *candidates[4];
    int n = 0;

    if (override_dir && *override_dir) {
        snprintf(s->openmsx_share, sizeof(s->openmsx_share), "%s",
                 override_dir);
        if (dir_has_file(s->openmsx_share, "init.tcl")) return 0;
    }

    env = getenv("MSX_OPENMSX_SHARE");
    if (env && *env) candidates[n++] = env;
#ifdef MSX_INSTALL_DATADIR_SHARE
    candidates[n++] = MSX_INSTALL_DATADIR_SHARE;
#endif
#ifdef MSX_DEV_OPENMSX_SHARE
    candidates[n++] = MSX_DEV_OPENMSX_SHARE;
#endif

    for (int i = 0; i < n; i++) {
        if (dir_has_file(candidates[i], "init.tcl")) {
            snprintf(s->openmsx_share, sizeof(s->openmsx_share), "%s",
                     candidates[i]);
            return 0;
        }
    }
    return -1;
}

/****************************************************************************/
/** Frame store                                                            **/
/****************************************************************************/

/* Called by msx_host.cc's msxhost_notify_frame -> the frame sink, on the
 * openMSX thread, once per completed frame. */
static void on_core_frame(const uint32_t *px, int w, int h, void *user)
{
    msxsession *s = user;
    size_t n;

    if (w != MSXSESSION_FB_WIDTH || h != MSXSESSION_FB_HEIGHT) {
        /* Cannot happen -- msxhost_notify_frame always publishes exactly
         * this size (openMSX's own getLinePtr640_480 scaling) -- but a
         * dropped frame beats scribbling past the buffer if that ever
         * changes upstream. */
        fprintf(stderr, "msxsession: dropping unexpected %dx%d frame "
                        "(want %dx%d)\n",
                w, h, MSXSESSION_FB_WIDTH, MSXSESSION_FB_HEIGHT);
        return;
    }

    n = (size_t)w * (size_t)h;
    pthread_mutex_lock(&s->frame_mtx);
    memcpy(s->frame, px, n * sizeof(uint32_t));
    s->frame_serial++;
    pthread_mutex_unlock(&s->frame_mtx);
}

int msxsession_copy_frame(msxsession *s, uint32_t *dst,
                          uint64_t *serial_inout)
{
    int copied = 0;
    pthread_mutex_lock(&s->frame_mtx);
    if (s->frame_serial != *serial_inout) {
        memcpy(dst, s->frame, MSX_FB_PIXELS * sizeof(uint32_t));
        *serial_inout = s->frame_serial;
        copied = 1;
    }
    pthread_mutex_unlock(&s->frame_mtx);
    return copied;
}

/****************************************************************************/
/** Vsync (advisory only -- see msxsession.h)                              **/
/****************************************************************************/

void msxsession_notify_vsync(msxsession *s, int64_t frame_time_ns)
{
    const int pace_log = getenv("MSX_PACE_LOG") != NULL;
    if (!s) return;
    if (pace_log && s->vs_ns) {
        long iv_us = (long)((frame_time_ns - s->vs_ns) / 1000);
        static long acc_us;
        static int acc_n;
        acc_us += iv_us;
        acc_n++;
        if (acc_us >= 1000000L) {
            fprintf(stderr, "msxsession pace: %d UI ticks over %ld ms "
                            "(advisory only -- openMSX paces itself)\n",
                    acc_n, acc_us / 1000);
            acc_us = 0;
            acc_n = 0;
        }
    }
    s->vs_ns = frame_time_ns;
}

/****************************************************************************/
/** Lifecycle                                                              **/
/****************************************************************************/

msxsession *msxsession_new(const msxsession_paths *paths)
{
    msxsession *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    /* ~1.2MB, so heap rather than inline in the struct. */
    s->frame = calloc(MSX_FB_PIXELS, sizeof(uint32_t));
    if (!s->frame) {
        free(s);
        return NULL;
    }

    if (resolve_openmsx_share(s, paths ? paths->openmsx_share : NULL) != 0) {
        session_set_error(s,
            "Could not find the openMSX runtime share tree (init.tcl). "
            "Set MSX_OPENMSX_SHARE, or build tools/openmsx/"
            "build-openmsx-desktop.sh first for a dev checkout.");
        free(s->frame);
        free(s);
        return NULL;
    }

    pthread_mutex_init(&s->lifecycle_mtx, NULL);
    pthread_mutex_init(&s->frame_mtx, NULL);

    snprintf(s->cur_machine, sizeof(s->cur_machine), "%s",
             MSXSESSION_MACHINE_MSX2);
    return s;
}

void msxsession_free(msxsession *s)
{
    if (!s) return;
    msxsession_stop(s);
    pthread_mutex_destroy(&s->lifecycle_mtx);
    pthread_mutex_destroy(&s->frame_mtx);
    free(s->frame);
    free(s);
}

void msxsession_default_opts(msxsession *s, msxsession_start_opts *opts)
{
    (void)s;
    memset(opts, 0, sizeof(*opts));
    opts->machine = MSXSESSION_MACHINE_MSX2;
}

int msxsession_start(msxsession *s, const msxsession_start_opts *opts)
{
    pthread_mutex_lock(&s->lifecycle_mtx);
    if (s->running) {
        pthread_mutex_unlock(&s->lifecycle_mtx);
        return 0;
    }

    s->opts = *opts;
    snprintf(s->cur_machine, sizeof(s->cur_machine), "%s",
             (opts->machine && *opts->machine) ? opts->machine
                                               : MSXSESSION_MACHINE_MSX2);

    /* openMSX resolves its user dir from $HOME/$OPENMSX_HOME and its
     * bundled machines/extensions/C-BIOS from OPENMSX_SYSTEM_DATA. */
    msx_setenv("OPENMSX_SYSTEM_DATA", s->openmsx_share);
    if (s->data_dir[0]) {
        msx_setenv("HOME", s->data_dir);
        {
            char home_openmsx[MSX_PATH_MAX];
            snprintf(home_openmsx, sizeof(home_openmsx), "%s/.openMSX",
                     s->data_dir);
            msx_setenv("OPENMSX_HOME", home_openmsx);
        }
    }

    msxhost_set_frame_sink(on_core_frame, s);
    msxhost_select_machine(s->cur_machine);

    s->frame_serial = 0;

    if (!msxhost_core_start()) {
        session_set_error(s, "%s", msxhost_last_error());
        msxhost_set_frame_sink(NULL, NULL);
        pthread_mutex_unlock(&s->lifecycle_mtx);
        return -1;
    }

    s->running = 1;
    pthread_mutex_unlock(&s->lifecycle_mtx);
    return 0;
}

void msxsession_stop(msxsession *s)
{
    pthread_mutex_lock(&s->lifecycle_mtx);
    if (!s->running) {
        pthread_mutex_unlock(&s->lifecycle_mtx);
        return;
    }
    msxhost_core_stop();
    msxhost_set_frame_sink(NULL, NULL);
    s->running = 0;
    pthread_mutex_unlock(&s->lifecycle_mtx);
}

int msxsession_is_running(const msxsession *s)
{
    return s && s->running;
}

const char *msxsession_last_error(const msxsession *s)
{
    return s->last_error;
}

/****************************************************************************/
/** Input / misc accessors                                                 **/
/****************************************************************************/

void msxsession_key(msxsession *s, int down, uint32_t keysym,
                    uint32_t unicode, int ctrl_down, int shift_down)
{
    unsigned keycode = 0;
    uint16_t mods = 0;

    if (!s->running) return;
    if (!msx_key_from_event(keysym, unicode, ctrl_down, shift_down,
                            &keycode, &mods))
        return;
    msxhost_inject_key(down, keycode, unicode, mods);
}

void msxsession_joystick_button(msxsession *s, int port, int button,
                                int pressed)
{
    if (!s->running) return;
    msxhost_set_joystick_button(port, button, pressed);
}

void msxsession_reset(msxsession *s)
{
    if (s->running) msxhost_core_reset();
}

const char *msxsession_config_path(const msxsession *s) { return s->config_dir; }
const char *msxsession_data_path(const msxsession *s)   { return s->data_dir; }
const char *msxsession_roms_path(const msxsession *s)   { return s->roms_dir; }
const char *msxsession_carts_path(const msxsession *s)  { return s->carts_dir; }
const char *msxsession_sd_path(const msxsession *s)     { return s->fujinet_sd; }

const char *msxsession_fujinet_webui_url(const msxsession *s)
{
    return s->webui_url;
}

int msxsession_fujinet_running(const msxsession *s)
{
    return s->fujinet_running;
}
