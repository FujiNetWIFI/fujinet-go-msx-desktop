/*
 * AppDelegate: builds the menu bar and main window, owns the session
 * lifecycle, and hosts the FujiNet console log window. Mirrors the GNOME/
 * KDE frontends over the same msxsession API, with the native debugger
 * window in debugger/DebuggerWindow.m.
 *
 * Ported from fujinet-go-coco-desktop's AppDelegate.m, not copied
 * line-for-line: MSX has no CoCo-style TV-input/Artifact-Colour/CPU radio
 * menus, and no configurable aspect_mode/smooth_scaling settings at all --
 * openMSX's own FrameSource::getLinePtr640_480() fixes the frame geometry
 * before msxsession ever sees it (see DisplayView.m), so there is no
 * Settings window here, and no WKWebView either: FujiNet Configuration
 * opens the system browser (msxsession_fujinet_webui_url), matching the
 * GNOME frontend's own GtkUriLauncher choice -- neither sibling embeds a
 * web view for this yet. Machine is a fixed 3-entry radio group
 * (MSX/MSX2/MSX2+, from msxsession.h's MSXSESSION_MACHINE_* constants),
 * not CoCo's generic runtime-walked name table.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "DisplayView.h"
#import "debugger/DebuggerWindow.h"

#include <stdlib.h>

static NSArray<UTType *> *typesForExtensions(NSArray<NSString *> *exts)
{
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (NSString *ext in exts) {
        UTType *t = [UTType typeWithFilenameExtension:ext];
        if (t)
            [types addObject:t];
    }
    return types;
}

/* Fixed 3-entry table, matching the GNOME/KDE frontends' own k_machines. */
static const struct { const char *label; const char *id; } kMachines[] = {
    { "MSX",   MSXSESSION_MACHINE_MSX },
    { "MSX2",  MSXSESSION_MACHINE_MSX2 },
    { "MSX2+", MSXSESSION_MACHINE_MSX2P },
};
#define kMachineCount (sizeof(kMachines) / sizeof(kMachines[0]))

static NSInteger machineIndex(const char *machineId)
{
    for (NSUInteger i = 0; i < kMachineCount; i++)
        if (strcmp(kMachines[i].id, machineId) == 0)
            return (NSInteger)i;
    return -1;
}

@implementation AppDelegate {
    msxsession *_session;
    NSWindow *_window;
    DisplayView *_display;
    NSWindow *_logWindow;
    NSTextView *_logView;
    NSTimer *_logTimer;
}

- (instancetype)initWithSession:(msxsession *)session
{
    self = [super init];
    if (self)
        _session = session;
    return self;
}

/* ---- actions ---------------------------------------------------------------- */

- (void)resetMachine:(id)sender
{
    (void)sender;
    msxsession_reset(_session);
}

- (void)selectMachine:(NSMenuItem *)sender
{
    NSInteger idx = sender.tag;
    if (idx < 0 || (NSUInteger)idx >= kMachineCount)
        return;
    msxsession_set_machine(_session, kMachines[idx].id);
    [self buildMenus];
}

- (void)importMedia:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Import Disk, Cassette or Cartridge Image";
    panel.allowedContentTypes = typesForExtensions(@[ @"dsk", @"cas", @"rom" ]);
    if ([panel runModal] != NSModalResponseOK || !panel.URL)
        return;
    char dest[1024];
    if (msxsession_import_media(_session, panel.URL.path.UTF8String, dest,
                                sizeof(dest)) != 0)
        NSLog(@"import: %s", msxsession_last_error(_session));
}

- (void)showFujiNetConfig:(id)sender
{
    (void)sender;
    NSString *url = [NSString
        stringWithUTF8String:msxsession_fujinet_webui_url(_session)];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:url]];
}

- (void)showFujiNetLog:(id)sender
{
    (void)sender;
    if (_logWindow) {
        [_logWindow makeKeyAndOrderFront:nil];
        return;
    }
    NSRect frame = NSMakeRect(0, 0, 820, 560);
    _logWindow = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _logWindow.title = @"FujiNet Console Log";
    _logWindow.releasedWhenClosed = NO;

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:frame];
    scroll.hasVerticalScroller = YES;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _logView = [[NSTextView alloc] initWithFrame:frame];
    _logView.editable = NO;
    _logView.font = [NSFont monospacedSystemFontOfSize:11
                                                weight:NSFontWeightRegular];
    scroll.documentView = _logView;
    _logWindow.contentView = scroll;

    _logTimer = [NSTimer
        scheduledTimerWithTimeInterval:1.0
                               repeats:YES
                                 block:^(NSTimer *timer) {
                                   (void)timer;
                                   [self refreshLog];
                                 }];
    [self refreshLog];
    [_logWindow center];
    [_logWindow makeKeyAndOrderFront:nil];
}

- (void)refreshLog
{
    static char buf[128 * 1024];
    int n = msxsession_fujinet_copy_log(_session, buf, sizeof(buf));
    NSString *text = n > 0 ? [NSString stringWithUTF8String:buf]
                           : @"(no FujiNet output yet)";
    if (text)
        _logView.string = text;
    [_logView scrollToEndOfDocument:nil];
}

- (void)showDebugger:(id)sender
{
    (void)sender;
    [DebuggerWindow showForSession:_session];
}

/* ---- menus -------------------------------------------------------------------- */

- (NSMenuItem *)item:(NSString *)title action:(SEL)sel key:(NSString *)key
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:sel
                                           keyEquivalent:key];
    item.target = self;
    return item;
}

- (void)buildMenus
{
    NSString *appName = @"FujiNet Go MSX";
    NSMenu *menubar = [[NSMenu alloc] init];
    NSInteger current = machineIndex(msxsession_machine(_session));

    /* A menu bar built in code gets nothing for free: the standard items
     * every Mac app is expected to have (Services, Hide/Hide Others/Show
     * All, the window list) exist only if they are added here. */
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:[@"About " stringByAppendingString:appName]
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenu *servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
    NSMenuItem *servicesItem = [appMenu addItemWithTitle:@"Services"
                                                  action:NULL
                                           keyEquivalent:@""];
    servicesItem.submenu = servicesMenu;
    NSApp.servicesMenu = servicesMenu;
    [appMenu addItem:[NSMenuItem separatorItem]];

    [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:appName]
                       action:@selector(hide:)
                keyEquivalent:@"h"];
    NSMenuItem *hideOthers =
        [appMenu addItemWithTitle:@"Hide Others"
                           action:@selector(hideOtherApplications:)
                    keyEquivalent:@"h"];
    hideOthers.keyEquivalentModifierMask =
        NSEventModifierFlagOption | NSEventModifierFlagCommand;
    [appMenu addItemWithTitle:@"Show All"
                       action:@selector(unhideAllApplications:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];

    [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:appName]
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    [menubar addItem:appItem];

    /* Machine: the live radio group plus Reset. */
    NSMenuItem *machineItem = [[NSMenuItem alloc] init];
    NSMenu *machineMenu = [[NSMenu alloc] initWithTitle:@"Machine"];
    for (NSUInteger i = 0; i < kMachineCount; i++) {
        NSMenuItem *mi = [machineMenu
            addItemWithTitle:[NSString stringWithUTF8String:kMachines[i].label]
                      action:@selector(selectMachine:)
               keyEquivalent:@""];
        mi.target = self;
        mi.tag = (NSInteger)i;
        mi.state = ((NSInteger)i == current) ? NSControlStateValueOn
                                             : NSControlStateValueOff;
    }
    [machineMenu addItem:[NSMenuItem separatorItem]];
    [machineMenu addItem:[self item:@"Reset"
                             action:@selector(resetMachine:)
                                key:@""]];
    machineItem.submenu = machineMenu;
    [menubar addItem:machineItem];

    NSMenuItem *mediaItem = [[NSMenuItem alloc] init];
    NSMenu *media = [[NSMenu alloc] initWithTitle:@"Media"];
    [media addItem:[self item:@"Import Disk, Cassette or Cartridge…"
                       action:@selector(importMedia:)
                          key:@"i"]];
    mediaItem.submenu = media;
    [menubar addItem:mediaItem];

    NSMenuItem *fujiItem = [[NSMenuItem alloc] init];
    NSMenu *fuji = [[NSMenu alloc] initWithTitle:@"FujiNet"];
    [fuji addItem:[self item:@"Configuration…"
                      action:@selector(showFujiNetConfig:)
                         key:@""]];
    [fuji addItem:[self item:@"Console Log…"
                      action:@selector(showFujiNetLog:)
                         key:@""]];
    fujiItem.submenu = fuji;
    [menubar addItem:fujiItem];

    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem *fs = [view addItemWithTitle:@"Toggle Full Screen"
                                     action:@selector(toggleFullScreen:)
                              keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask =
        NSEventModifierFlagControl | NSEventModifierFlagCommand;
    NSMenuItem *dbg = [self item:@"Debugger"
                          action:@selector(showDebugger:)
                             key:[NSString stringWithFormat:@"%C",
                                           (unichar)NSF12FunctionKey]];
    dbg.keyEquivalentModifierMask = 0;
    [view addItem:dbg];
    viewItem.submenu = view;
    [menubar addItem:viewItem];

    /* Window menu. AppKit appends the list of open windows (the main
     * window, the debugger, the console log) to whichever menu is handed
     * to windowsMenu, and manages their check marks. */
    NSMenuItem *windowItem = [[NSMenuItem alloc] init];
    NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    [windowMenu addItemWithTitle:@"Close"
                          action:@selector(performClose:)
                   keyEquivalent:@"w"];
    [windowMenu addItemWithTitle:@"Minimize"
                          action:@selector(performMiniaturize:)
                   keyEquivalent:@"m"];
    [windowMenu addItemWithTitle:@"Zoom"
                          action:@selector(performZoom:)
                   keyEquivalent:@""];
    [windowMenu addItem:[NSMenuItem separatorItem]];
    [windowMenu addItemWithTitle:@"Bring All to Front"
                          action:@selector(arrangeInFront:)
                   keyEquivalent:@""];
    windowItem.submenu = windowMenu;
    [menubar addItem:windowItem];
    NSApp.windowsMenu = windowMenu;

    NSApp.mainMenu = menubar;
}

/* ---- lifecycle --------------------------------------------------------------- */

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;
    [self buildMenus];

    /* MSX's frame is always exactly 640x480 (see DisplayView.m); the
     * window starts a bit larger so the initial letterboxing has room to
     * breathe, matching the GNOME/KDE frontends' own default sizes. */
    NSRect frame = NSMakeRect(0, 0, 800, 600);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"FujiNet Go MSX";

    _display = [[DisplayView alloc] initWithSession:_session];
    _display.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _display.frame = ((NSView *)_window.contentView).bounds;
    [_window.contentView addSubview:_display];

    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_display];
    [_display start];

    /* Developer affordance shared with the other frontends. */
    if (getenv("MSX_OPEN_DEBUGGER"))
        [DebuggerWindow showForSession:_session];
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [_logTimer invalidate];
    [_display stop];
    msxsession_stop(_session);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:
    (NSApplication *)sender
{
    (void)sender;
    return YES;
}

@end
