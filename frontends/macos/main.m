/*
 * FujiNet Go MSX -- macOS frontend entry point.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#import "AppDelegate.h"

#include <stdio.h>
#include <string.h>

#include "msxsession.h"

int main(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    @autoreleasepool {
        /* A bundled FujiNet runtime (CI packs libfujinet.dylib into
         * Contents/Frameworks and the pristine runtime tree into
         * Contents/Resources/fujinet) and a bundled openMSX runtime share
         * tree (Contents/Resources/openmsx -- init.tcl, scripts/,
         * machines/ incl. C-BIOS, extensions/ incl. FujiNet.xml) both take
         * priority when present; without either, the session falls back to
         * its own default search (see core/src/paths.c) -- MSX_OPENMSX_
         * SHARE / a dev build's raw script output for the share tree,
         * FujiNet disabled for the runtime.
         */
        msxsession_paths paths;
        memset(&paths, 0, sizeof(paths));
        NSString *lib = [[[NSBundle mainBundle] privateFrameworksPath]
            stringByAppendingPathComponent:@"libfujinet.dylib"];
        NSString *fujiSrc = [[[NSBundle mainBundle] resourcePath]
            stringByAppendingPathComponent:@"fujinet"];
        NSString *openmsxShare = [[[NSBundle mainBundle] resourcePath]
            stringByAppendingPathComponent:@"openmsx"];
        NSFileManager *fm = [NSFileManager defaultManager];
        if ([fm fileExistsAtPath:lib])
            paths.fujinet_lib = lib.UTF8String;
        if ([fm fileExistsAtPath:
                    [fujiSrc stringByAppendingPathComponent:@"fnconfig.ini"]])
            paths.fujinet_runtime_src = fujiSrc.UTF8String;
        if ([fm fileExistsAtPath:
                    [openmsxShare stringByAppendingPathComponent:@"init.tcl"]])
            paths.openmsx_share = openmsxShare.UTF8String;

        msxsession *session = msxsession_new(&paths);
        if (!session) {
            fprintf(stderr, "fatal: could not create the session\n");
            return 1;
        }
        msxsession_start_opts opts;
        msxsession_default_opts(session, &opts);
        if (msxsession_start(session, &opts) != 0)
            fprintf(stderr, "session start: %s\n",
                    msxsession_last_error(session));

        NSApplication *app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate *delegate =
            [[AppDelegate alloc] initWithSession:session];
        app.delegate = delegate;
        [app run];

        msxsession_free(session);
    }
    return 0;
}
