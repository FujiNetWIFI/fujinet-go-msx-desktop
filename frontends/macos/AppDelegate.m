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
 * before msxsession ever sees it (see DisplayView.m), so the only settings
 * window is the FujiNet one, and there is no WKWebView either: FujiNet
 * Configuration opens the system browser (msxsession_fujinet_webui_url), matching the
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
    NSWindow *_settingsWindow;
    NSTextField *_externalField;
    BOOL _sessionDirty;
}

- (instancetype)initWithSession:(msxsession *)session
{
    self = [super init];
    if (self)
        _session = session;
    return self;
}

/* ---- session helpers ---------------------------------------------------- */

- (void)restartSession
{
    msxsession_stop(_session);
    msxsession_start_opts opts;
    msxsession_default_opts(_session, &opts);
    if (msxsession_start(_session, &opts) != 0)
        NSLog(@"session restart: %s", msxsession_last_error(_session));
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
    if (msxsession_fujinet_external(_session)) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"FujiNet is external";
        alert.informativeText = @"Use the web UI of the FujiNet instance "
                                @"this app connects to.";
        [alert runModal];
        return;
    }
    NSString *url = [NSString
        stringWithUTF8String:msxsession_fujinet_webui_url(_session)];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:url]];
}

- (void)showFujiNetLog:(id)sender
{
    (void)sender;
    if (msxsession_fujinet_external(_session)) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"FujiNet is external";
        alert.informativeText = @"Its console output is in the process "
                                @"that is running it.";
        [alert runModal];
        return;
    }
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

/* Starts at the bottom and follows it while the reader is there; once they
 * scroll up it stays put, and an unchanged log is not touched. */
- (void)refreshLog
{
    static char buf[128 * 1024];
    int n = msxsession_fujinet_copy_log(_session, buf, sizeof(buf));
    NSString *text = n > 0 ? [NSString stringWithUTF8String:buf]
                           : @"(no FujiNet output yet)";
    NSScrollView *scroll = _logView.enclosingScrollView;
    const BOOL first = _logView.string.length == 0;
    BOOL follow = first;

    if (!text || (!first && [_logView.string isEqualToString:text]))
        return;
    if (!first && scroll)
        follow = NSMaxY(scroll.contentView.documentVisibleRect) >=
                 NSHeight(_logView.frame) - 4;
    const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    _logView.string = text;
    if (follow) {
        [_logView scrollRangeToVisible:NSMakeRange(text.length, 0)];
    } else if (scroll) {
        [scroll.contentView scrollToPoint:at];
        [scroll reflectScrolledClipView:scroll.contentView];
    }
}

- (void)showDebugger:(id)sender
{
    (void)sender;
    [DebuggerWindow toggleForSession:_session];
}

/* ---- preferences -------------------------------------------------------- */

/* The FujiNet section only: use the built-in FujiNet, or an external one at
 * host:port. Values write straight through to the shared settings store, the
 * same keys as the GNOME and KDE frontends; the session restarts when the
 * window closes. */

- (void)externalToggled:(NSButton *)sender
{
    const int on = sender.state == NSControlStateValueOn ? 1 : 0;
    _externalField.enabled = on != 0;
    if (msxsession_get_int(_session, "fujinet_external", 0) == on)
        return;
    msxsession_set_int(_session, "fujinet_external", on);
    _sessionDirty = YES;
}

/* The address field: valid text (or none, meaning 127.0.0.1:65505) is
 * saved; anything else is shown in red and not saved. */
- (void)externalAddrChanged:(id)sender
{
    NSTextField *field = sender;
    NSString *text = [field.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet
                                            .whitespaceAndNewlineCharacterSet];
    char host[128];
    int port;
    if (msxsession_parse_hostport(text.UTF8String, host, sizeof host,
                                  &port) != 0) {
        field.textColor = NSColor.systemRedColor;
        return;
    }
    field.textColor = NSColor.controlTextColor;
    if (strcmp(msxsession_get_str(_session, "fujinet_external_addr", ""),
               text.UTF8String) == 0)
        return;
    msxsession_set_str(_session, "fujinet_external_addr", text.UTF8String);
    _sessionDirty = YES;
}

- (void)showFujiNetPrefs:(id)sender
{
    (void)sender;
    if (_settingsWindow) {
        [_settingsWindow makeKeyAndOrderFront:nil];
        return;
    }

    NSButton *box = [NSButton checkboxWithTitle:@""
                                         target:self
                                         action:@selector(externalToggled:)];
    box.state = msxsession_get_int(_session, "fujinet_external", 0)
                    ? NSControlStateValueOn
                    : NSControlStateValueOff;
    _externalField = [NSTextField textFieldWithString:@(
        msxsession_get_str(_session, "fujinet_external_addr", ""))];
    _externalField.placeholderString = @"127.0.0.1:65505";
    _externalField.preferredMaxLayoutWidth = 240;
    _externalField.enabled = box.state == NSControlStateValueOn;
    _externalField.target = self;
    _externalField.action = @selector(externalAddrChanged:);
    [_externalField.cell setSendsActionOnEndEditing:YES];

    NSTextField *title = [NSTextField labelWithString:@"FujiNet"];
    title.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    NSArray<NSArray<NSView *> *> *rows = @[
        @[ title, [NSTextField labelWithString:@""] ],
        @[ [NSTextField labelWithString:@"Use external FujiNet"], box ],
        @[ [NSTextField labelWithString:@"Address:port"], _externalField ],
    ];
    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 8;
    grid.columnSpacing = 12;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;

    NSTextField *note = [NSTextField
        labelWithString:@"Skips the built-in FujiNet and connects to another "
                        @"instance already running.\nUse its [BOIP] listen "
                        @"port (65505 by default). The session restarts "
                        @"when this window closes."];
    note.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    note.textColor = NSColor.secondaryLabelColor;

    NSStackView *root = [NSStackView stackViewWithViews:@[ grid, note ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.spacing = 12;
    root.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);

    _settingsWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 460, 180)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _settingsWindow.title = @"FujiNet Preferences";
    _settingsWindow.releasedWhenClosed = NO;
    _settingsWindow.delegate = self;
    _settingsWindow.contentView = root;
    [_settingsWindow center];
    [_settingsWindow makeKeyAndOrderFront:nil];
}

/* Options are collected while the window is open and applied in one restart
 * when it closes. */
- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _settingsWindow)
        return;
    [_settingsWindow makeFirstResponder:nil]; /* commit a field edit */
    _settingsWindow = nil;
    _externalField = nil;
    if (!_sessionDirty)
        return;
    _sessionDirty = NO;
    [self restartSession];
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
    [fuji addItem:[self item:@"Preferences…"
                      action:@selector(showFujiNetPrefs:)
                         key:@","]];
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
