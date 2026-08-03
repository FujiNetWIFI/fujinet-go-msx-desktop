/*
 * test_sleep.h -- shared sleep_ms() for the ctest binaries.
 *
 * On macOS, these test binaries have no Cocoa/CF run loop of their own
 * (unlike the real desktop frontends, which always have one via their
 * toolkit's event loop). A plain sleep here can permanently wedge the
 * openMSX thread if it needs to dispatch_sync back to this process's real
 * main thread while a test is "just waiting" -- e.g. msxhost_switch_machine()
 * is fire-and-forget (it only queues Tcl commands; see msx_host.cc), so a
 * live machine switch rebuilds VisibleSurface (a real NSWindow) on the
 * openMSX thread some time after the call returns, with nothing here to
 * service the dispatch it needs (see patch-staged-tree.py's
 * msxRunOnMainThread). Same reasoning as msx_host.cc's own
 * wait_rpc_done()/boot-wait, applied to callers polling the C API from
 * outside the process's synchronous RPC path -- confirmed by a real macOS
 * CI run where machine_switch_test hung exactly here after a live switch.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MSX_TEST_SLEEP_H
#define MSX_TEST_SLEEP_H

#include <time.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h> /* CFRunLoopRunInMode's Boolean arg: true/false are not
                       * keywords in this project's C11 test binaries. */
#endif

static void sleep_ms(int ms)
{
#ifdef __APPLE__
    double remaining = (double)ms / 1000.0;
    while (remaining > 0.0) {
        double slice = remaining < 0.01 ? remaining : 0.01;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, slice, true);
        remaining -= slice;
    }
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

#endif /* MSX_TEST_SLEEP_H */
