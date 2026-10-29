/*
 * external_smoke -- "Use external FujiNet": with the setting on, the session
 * starts no FujiNet runtime of its own and openMSX's FujiNet cartridge dials
 * the configured host:port instead of 127.0.0.1:65505. A stand-in listener
 * plays the external FujiNet; the test waits for the cartridge to connect.
 * Also checks that a bad address refuses to start. Private config and data
 * directories, so the user's settings are never touched.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msxsession.h"
#include "test_sleep.h"

#if defined(_WIN32)
int main(void)
{
    return 77; /* POSIX sockets; Windows has the unit test only */
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "external_smoke: FAIL line %d: %s\n",         \
                    __LINE__, #cond);                                     \
            failures++;                                                   \
        }                                                                 \
    } while (0)

int main(void)
{
    char tmpl[] = "/tmp/msx-external-smoke-XXXXXX";
    char cfg[256], dat[256], addr[64];
    msxsession_paths paths;
    msxsession_start_opts opts;
    msxsession *s;
    struct sockaddr_in sin;
    socklen_t slen = sizeof(sin);
    struct pollfd pfd;
    int lsn, conn = -1, failures = 0, waited;

    if (!mkdtemp(tmpl)) return 1;
    snprintf(cfg, sizeof(cfg), "%s/config", tmpl);
    snprintf(dat, sizeof(dat), "%s/data", tmpl);
    memset(&paths, 0, sizeof(paths));
    paths.config_dir = cfg;
    paths.data_dir = dat;

    lsn = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (lsn < 0 || bind(lsn, (struct sockaddr *)&sin, sizeof(sin)) != 0 ||
        listen(lsn, 1) != 0 ||
        getsockname(lsn, (struct sockaddr *)&sin, &slen) != 0) {
        fprintf(stderr, "external_smoke: cannot listen\n");
        return 1;
    }
    /* A host name, so the getaddrinfo path is the one exercised. */
    snprintf(addr, sizeof(addr), "localhost:%d", ntohs(sin.sin_port));

    s = msxsession_new(&paths);
    if (!s) return 1;

    /* A bad address is refused, not started. */
    msxsession_set_int(s, "fujinet_external", 1);
    msxsession_set_str(s, "fujinet_external_addr", "not an address");
    msxsession_default_opts(s, &opts);
    CHECK(msxsession_start(s, &opts) != 0);
    CHECK(!msxsession_is_running(s));

    msxsession_set_str(s, "fujinet_external_addr", addr);
    msxsession_default_opts(s, &opts);
    CHECK(opts.external_fujinet == 1);
    if (msxsession_start(s, &opts) != 0) {
        fprintf(stderr, "external_smoke: start failed: %s\n",
                msxsession_last_error(s));
        return 1;
    }
    CHECK(msxsession_fujinet_external(s));
    CHECK(!msxsession_fujinet_running(s));
    CHECK(strcmp(msxsession_fujinet_webui_url(s), "") == 0);
    CHECK(strcmp(msxsession_fujinet_endpoint(s), addr) == 0);

    pfd.fd = lsn;
    pfd.events = POLLIN;
    for (waited = 0; waited < 15000; waited += 100) {
        if (poll(&pfd, 1, 100) > 0) {
            conn = accept(lsn, NULL, NULL);
            break;
        }
    }
    CHECK(conn >= 0);
    for (waited = 0; conn >= 0 && waited < 3000; waited += 50) {
        if (msxsession_fujinet_link(s)) break;
        sleep_ms(50);
    }
    CHECK(conn < 0 || msxsession_fujinet_link(s));
    printf("external_smoke: connected=%d link=%d endpoint=%s\n", conn >= 0,
           msxsession_fujinet_link(s), msxsession_fujinet_endpoint(s));

    msxsession_stop(s);
    CHECK(!msxsession_fujinet_external(s));
    msxsession_free(s);
    if (conn >= 0) close(conn);
    close(lsn);
    {
        char cmd[300];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpl);
        if (system(cmd) != 0) { /* leftover temp dir is harmless */ }
    }
    return failures ? 1 : 0;
}
#endif
