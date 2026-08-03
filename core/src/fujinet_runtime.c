/*
 * msxsession FujiNet runtime control: dlopen libfujinet.so and drive the
 * fujinet_desktop_* entry points (the desktop build of fujinet-pc-msx's
 * RS232 target plus the in-process entry wrapper,
 * tools/fujinet/support/fujinet_desktop_entry.cpp).
 *
 * Unlike the CoCo port this carries no audio mixing: the RS232 target has
 * no SAM speech synthesizer output to overlay (that is an Atari-hardware
 * feature), and openMSX owns its own SDL2 Mixer audio path independently of
 * this file either way -- see COMPLIANCE.md / TODO.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "compat.h"
#include "dynlib.h"
#include "session_internal.h"

typedef int (*start_runtime_fn)(const char *root, const char *config,
                                const char *sd, const char *data,
                                int listen_port);
typedef void (*stop_runtime_fn)(void);
typedef const char *(*last_error_fn)(void);
typedef int (*copy_log_fn)(char *out, int max_bytes);

/* One runtime per process: the library owns background threads (web admin,
 * network listeners) that live inside its mapping, so it is loaded once and
 * NEVER dlclose'd -- unmapping it while any such thread still runs executes
 * freed code and crashes. A stopped runtime is restarted through the same
 * handle. Same pattern (and reasoning) as the sibling repos' wrappers. */
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static msx_dynlib g_handle;
static start_runtime_fn g_start;
static stop_runtime_fn g_stop;
static last_error_fn g_last_error;
static copy_log_fn g_copy_log;

static int load_library_locked(msxsession *s)
{
    char errbuf[256];
    if (g_handle) return 0;

    g_handle = msx_dynlib_open(s->fujinet_lib);
    if (!g_handle) {
        session_set_error(s, "FujiNet library load failed: %s",
                          msx_dynlib_error(errbuf, sizeof(errbuf)));
        return -1;
    }
    g_start = (start_runtime_fn)msx_dynlib_sym(
        g_handle, "fujinet_desktop_start_runtime");
    g_stop = (stop_runtime_fn)msx_dynlib_sym(
        g_handle, "fujinet_desktop_stop_runtime");
    g_last_error = (last_error_fn)msx_dynlib_sym(
        g_handle, "fujinet_desktop_last_error_message");
    g_copy_log = (copy_log_fn)msx_dynlib_sym(
        g_handle, "fujinet_desktop_copy_recent_log");

    if (!g_start || !g_stop || !g_last_error) {
        session_set_error(s, "%s is missing the desktop runtime contract",
                          s->fujinet_lib);
        /* Leave the handle mapped (never dlclose); just mark it unusable. */
        g_start = NULL;
        return -1;
    }
    return 0;
}

int fujinet_start(msxsession *s)
{
    pthread_mutex_lock(&g_mtx);
    if (s->fujinet_running) {
        pthread_mutex_unlock(&g_mtx);
        return 0;
    }
    if (paths_provision_fujinet(s) != 0) {
        pthread_mutex_unlock(&g_mtx);
        fprintf(stderr, "msxsession: FujiNet runtime unavailable; "
                        "continuing without it\n");
        return -1;
    }
    if (load_library_locked(s) != 0) {
        pthread_mutex_unlock(&g_mtx);
        return -1;
    }
    /* Two of the firmware patches (fnFsSPIFFS's flash base and
     * mgHttpClient's CA bundle -- see tools/fujinet/build-fujinet-
     * desktop.sh) root themselves from this rather than from the CWD. */
    msx_setenv("FUJINET_RUNTIME_ROOT", s->fujinet_root);
    if (!g_start(s->fujinet_root, s->fujinet_config, s->fujinet_sd,
                 s->fujinet_data, MSXSESSION_BOIP_PORT)) {
        const char *err = g_last_error ? g_last_error() : NULL;
        session_set_error(s, "FujiNet runtime failed to start: %s",
                          err && *err ? err : "(unknown)");
        pthread_mutex_unlock(&g_mtx);
        return -1;
    }
    s->fujinet_running = 1;
    pthread_mutex_unlock(&g_mtx);
    return 0;
}

/* Wait until something is actually accepting on the BOIP port.
 *
 * fujinet_start() returning is NOT enough, and this is the trap that makes
 * this target (like CoCo) different from ADAM/Apple II. The BoIPChannel
 * shared component (lib/hardware/BoIPChannel.cpp -- used by both COCO's
 * DriveWire bus and MSX's RS232 bus) comes back from setup as soon as the
 * devices are constructed, logs "BoIPChannel: No WiFi!" and suspends itself
 * for up to 2 seconds before it actually starts listening. Start openMSX in
 * that window and its FujiNet cartridge device connects, gets refused,
 * closes, and (per src/serial/FujiNet.cc) retries once a second -- so it
 * DOES recover on its own, unlike XRoar's stock becker.c which the CoCo
 * port had to patch. But probing here rather than guessing at a sleep still
 * saves several seconds of "press c, nothing happens yet" on first boot.
 *
 * The probe connection is closed immediately; openMSX's own FujiNet device
 * makes the real one. Returns 0 once the listener is up, -1 on timeout --
 * not fatal, since the retry above covers it either way. */
int fujinet_wait_for_boip(msxsession *s, int timeout_ms)
{
    int waited = 0;

    if (!s->fujinet_running)
        return -1;

    for (;;) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_in addr;
            int ok;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(MSXSESSION_BOIP_PORT);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
            msx_closesocket(fd);
            if (ok)
                return 0;
        }
        if (waited >= timeout_ms) {
            fprintf(stderr, "msxsession: the FujiNet BOIP listener did not "
                            "come up within %d ms; openMSX's own FujiNet "
                            "device retries every second, so it will still "
                            "connect once the runtime is up\n", timeout_ms);
            return -1;
        }
        msx_sleep_ms(25);
        waited += 25;
    }
}

void fujinet_stop(msxsession *s)
{
    stop_runtime_fn stop = NULL;

    pthread_mutex_lock(&g_mtx);
    if (s->fujinet_running) {
        stop = g_stop;
        s->fujinet_running = 0;
    }
    pthread_mutex_unlock(&g_mtx);

    if (stop) stop();
}

int fujinet_copy_log(msxsession *s, char *dst, int max)
{
    copy_log_fn fn;
    (void)s;
    if (!dst || max <= 0) return 0;
    dst[0] = '\0';
    pthread_mutex_lock(&g_mtx);
    fn = g_copy_log;
    pthread_mutex_unlock(&g_mtx);
    return fn ? fn(dst, max) : 0;
}
