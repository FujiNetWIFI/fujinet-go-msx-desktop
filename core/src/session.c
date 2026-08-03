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

    if (paths_init(s, paths) != 0) {
        free(s->frame);
        free(s);
        return NULL;
    }

    pthread_mutex_init(&s->lifecycle_mtx, NULL);
    pthread_mutex_init(&s->frame_mtx, NULL);
    settings_init(s);

    snprintf(s->cur_machine, sizeof(s->cur_machine), "%s",
             msxsession_get_str(s, "machine", MSXSESSION_MACHINE_MSX2));
    return s;
}

void msxsession_free(msxsession *s)
{
    if (!s) return;
    msxsession_stop(s);
    msxsession_settings_flush(s);
    settings_free_all(s);
    pthread_mutex_destroy(&s->lifecycle_mtx);
    pthread_mutex_destroy(&s->frame_mtx);
    free(s->frame);
    free(s);
}

void msxsession_default_opts(msxsession *s, msxsession_start_opts *opts)
{
    memset(opts, 0, sizeof(*opts));
    /* The pointer msxsession_get_str returns stays valid for the session's
     * lifetime (settings.c never frees a value until msxsession_free), so
     * it is safe for the caller to hold onto opts->machine past this call
     * -- which msxsession_start does, copying it into cur_machine. */
    opts->machine = msxsession_get_str(s, "machine", MSXSESSION_MACHINE_MSX2);
    opts->enable_fujinet = msxsession_get_int(s, "enable_fujinet", 1);
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

    /* FUJINET MUST BE UP BEFORE OPENMSX.
     *
     * Same reasoning as the CoCo port, and the opposite of ADAM/Apple II:
     * here FujiNet is the BOIP server and openMSX's FujiNet cartridge
     * device connects *out* to it. Waiting for the listener to actually
     * accept (rather than just returning from fujinet_start) avoids losing
     * the first seconds of the link to BoIPChannel's own suspend-before-
     * listen delay -- see fujinet_runtime.c. A missing or broken runtime is
     * not fatal: the machine still boots, just without FujiNet, and
     * openMSX's own connect loop keeps retrying regardless. */
    if (opts->enable_fujinet && fujinet_start(s) == 0)
        fujinet_wait_for_boip(s, 10000);

    msxhost_set_frame_sink(on_core_frame, s);
    msxhost_select_machine(s->cur_machine);

    s->frame_serial = 0;

    if (!msxhost_core_start()) {
        session_set_error(s, "%s", msxhost_last_error());
        msxhost_set_frame_sink(NULL, NULL);
        fujinet_stop(s);
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
    fujinet_stop(s);
    s->running = 0;
    pthread_mutex_unlock(&s->lifecycle_mtx);
}

void msxsession_set_machine(msxsession *s, const char *machine_id)
{
    if (!s || !machine_id || !*machine_id) return;
    snprintf(s->cur_machine, sizeof(s->cur_machine), "%s", machine_id);
    msxsession_set_str(s, "machine", machine_id);
    if (s->running)
        msxhost_switch_machine(machine_id);
}

const char *msxsession_machine(const msxsession *s)
{
    return s ? s->cur_machine : "";
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

/****************************************************************************/
/** Media import                                                           **/
/****************************************************************************/

static const char *path_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash)) return "";
    return dot + 1;
}

static int ext_is(const char *ext, const char *want)
{
    for (; *ext && *want; ext++, want++)
        if ((*ext | 0x20) != *want) return 0;
    return *ext == '\0' && *want == '\0';
}

int msxsession_import_rom(msxsession *s, const char *src_path,
                          char *dest_out, int dest_sz)
{
    const char *base = strrchr(src_path, '/');
    char dst[MSX_PATH_MAX];

    base = base ? base + 1 : src_path;
    if (!*base) {
        session_set_error(s, "Not a file: %s", src_path);
        return -1;
    }

    mkdir_p(s->roms_dir);
    snprintf(dst, sizeof(dst), "%s/%s", s->roms_dir, base);
    if (copy_file(src_path, dst) != 0) {
        session_set_error(s, "Could not copy %s to %s", src_path, dst);
        return -1;
    }
    if (dest_out && dest_sz > 0)
        snprintf(dest_out, (size_t)dest_sz, "%s", dst);
    return 0;
}

int msxsession_import_media(msxsession *s, const char *src_path,
                            char *dest_out, int dest_sz)
{
    /* Disk and cassette images go to FujiNet's SD directory, to be mounted
     * from the web UI. Cartridge images are openMSX's business, not
     * FujiNet's, so they go to the session's own cartridge directory. */
    static const char *const k_disk_exts[] = { "dsk", "cas", NULL };
    static const char *const k_cart_exts[] = { "rom", NULL };
    const char *ext = path_ext(src_path);
    const char *base = strrchr(src_path, '/');
    const char *dir = NULL;
    char dst[MSX_PATH_MAX];
    int i;

    base = base ? base + 1 : src_path;
    for (i = 0; k_disk_exts[i] && !dir; i++)
        if (ext_is(ext, k_disk_exts[i])) dir = s->fujinet_sd;
    for (i = 0; k_cart_exts[i] && !dir; i++)
        if (ext_is(ext, k_cart_exts[i])) dir = s->carts_dir;

    if (!dir) {
        session_set_error(s, "Unsupported media type \".%s\" (expected a "
                          "disk or cassette image: .dsk .cas, or a "
                          "cartridge: .rom)", ext);
        return -1;
    }
    if (!dir[0]) {
        session_set_error(s, "No FujiNet SD directory (FujiNet runtime "
                          "unavailable)");
        return -1;
    }

    mkdir_p(dir);
    snprintf(dst, sizeof(dst), "%s/%s", dir, base);
    if (copy_file(src_path, dst) != 0) {
        session_set_error(s, "Could not copy %s to %s", src_path, dst);
        return -1;
    }
    if (dest_out && dest_sz > 0)
        snprintf(dest_out, (size_t)dest_sz, "%s", dst);
    return 0;
}

int msxsession_fujinet_running(const msxsession *s)
{
    return s->fujinet_running;
}

int msxsession_fujinet_copy_log(msxsession *s, char *dst, int max)
{
    return fujinet_copy_log(s, dst, max);
}
