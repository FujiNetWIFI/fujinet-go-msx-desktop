/*
 * Debugger window (AppKit): disassembly with click-to-toggle breakpoints,
 * editable registers, a memory view, breakpoints, and a VDP tab
 * (nametable/pattern/sprite/palette pictures, decoded register/status/
 * command-engine text, and a physical-VRAM hex editor), over the shared
 * msxdebug/msxvdp engines -- the same content as the GNOME/KDE debugger
 * windows, built from plain AppKit views.
 *
 * Ported from fujinet-go-coco-desktop's DebuggerWindow.m, not copied
 * line-for-line: msxdebug.h's shape differs from cocodebug.h in ways that
 * matter here --
 *
 *   * A real stop callback. msxdebug_set_stop_callback fires from the
 *     engine's own poll thread (see core/debugger/debugger.c), marshaled
 *     to the main queue with dispatch_async -- unlike cocodebug's poll-
 *     only design (XRoar's instruction-hook context makes a cross-thread
 *     callback unsafe there), so this has no NSTimer poll loop at all.
 *   * Registers and memory ARE editable here (msxdebug_set_regs,
 *     msxdebug_write_mem both exist, unlike cocodebug's read-only pair),
 *     matching the GNOME/KDE debugger windows' own Enter-applies-while-
 *     paused convention.
 *   * A real VDP tab (V9918/V9938/V9958 -- nametable/pattern/sprite/
 *     palette pictures, decoded state text, a physical-VRAM hex editor),
 *     where CoCo has none (no VDP-equivalent chip on a CoCo) and ADAM's
 *     own AppKit debugger (which this port otherwise resembles least,
 *     since ADAM's engine is exec-hook based rather than Tcl-RPC based)
 *     only ever handled a single fixed TMS9918A. See core/include/
 *     msxvdp.h's header comment for exactly what is and is not rendered
 *     as pictures (the MSX1-compatible modes only; every mode's register/
 *     status/command-engine text is complete regardless).
 *   * No instruction trace -- msxdebug has none at all (a Tcl round trip
 *     per Z80 instruction cannot keep up with real CPU speed; see
 *     msxdebug.h's own header comment), so unlike a trace-capable engine
 *     there is no Trace tab here, matching the GNOME/KDE windows' own
 *     omission.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#include "msxdebug.h"
#include "msxvdp.h"

#include <string.h>

#define DISASM_LINES 40
#define MEM_ROWS 16
#define VRAM_ROWS 16
#define VRAM_PAGE (VRAM_ROWS * 16)
#define POKE_MAX 64

static DebuggerWindow *g_debugger;

/* Disassembly text view: a click toggles the breakpoint on the clicked
 * line. The address sits at columns 2-5 of each row (refreshDisasm's
 * "%c%c%04X  ..." format -- two flag columns then the hex address),
 * matching the GNOME/KDE windows' own rendering. */
@interface DasmTextView : NSTextView
@property(nonatomic, copy) void (^onToggleAddr)(uint16_t addr);
@end

@implementation DasmTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p];
    NSString *text = self.string;
    if (idx > text.length)
        idx = text.length;
    NSUInteger start = [text lineRangeForRange:NSMakeRange(idx, 0)].location;
    NSString *line = [text substringFromIndex:start];
    if (line.length >= 6) {
        unsigned addr = 0;
        NSScanner *scan =
            [NSScanner scannerWithString:[line substringWithRange:
                                                   NSMakeRange(2, 4)]];
        if ([scan scanHexInt:&addr] && addr <= 0xFFFF && self.onToggleAddr)
            self.onToggleAddr((uint16_t)addr);
    }
    [super mouseDown:event];
}
@end

static NSImage *imageFromRGBA(const uint8_t *rgba, int w, int h)
{
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        NULL, rgba, (size_t)w * (size_t)h * 4, NULL);
    CGImageRef cg = CGImageCreate(
        (size_t)w, (size_t)h, 8, 32, (size_t)w * 4, space,
        kCGImageAlphaLast, provider, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);
    NSImage *img = [[NSImage alloc] initWithCGImage:cg
                                               size:NSMakeSize(w, h)];
    CGImageRelease(cg);
    return img;
}

static const char *const kRegNames[8] = {"AF", "BC", "DE", "HL",
                                         "IX", "IY", "SP", "PC"};

static uint16_t regGet(const msxdebug_z80_regs *r, int i)
{
    switch (i) {
    case 0: return (uint16_t)((r->a << 8) | r->f);
    case 1: return (uint16_t)((r->b << 8) | r->c);
    case 2: return (uint16_t)((r->d << 8) | r->e);
    case 3: return (uint16_t)((r->h << 8) | r->l);
    case 4: return (uint16_t)((r->ixh << 8) | r->ixl);
    case 5: return (uint16_t)((r->iyh << 8) | r->iyl);
    case 6: return (uint16_t)((r->sph << 8) | r->spl);
    default: return (uint16_t)((r->pch << 8) | r->pcl);
    }
}

static void regSet(msxdebug_z80_regs *r, int i, uint16_t v)
{
    switch (i) {
    case 0: r->a = (uint8_t)(v >> 8); r->f = (uint8_t)v; break;
    case 1: r->b = (uint8_t)(v >> 8); r->c = (uint8_t)v; break;
    case 2: r->d = (uint8_t)(v >> 8); r->e = (uint8_t)v; break;
    case 3: r->h = (uint8_t)(v >> 8); r->l = (uint8_t)v; break;
    case 4: r->ixh = (uint8_t)(v >> 8); r->ixl = (uint8_t)v; break;
    case 5: r->iyh = (uint8_t)(v >> 8); r->iyl = (uint8_t)v; break;
    case 6: r->sph = (uint8_t)(v >> 8); r->spl = (uint8_t)v; break;
    default: r->pch = (uint8_t)(v >> 8); r->pcl = (uint8_t)v; break;
    }
}

static msxvdp_chip chipFromMachine(const char *machineId)
{
    if (!machineId) return MSXVDP_CHIP_V9938;
    if (strstr(machineId, "MSX2+")) return MSXVDP_CHIP_V9958;
    if (strstr(machineId, "MSX1")) return MSXVDP_CHIP_V9918;
    return MSXVDP_CHIP_V9938;
}

@interface DebuggerWindow () <NSWindowDelegate>
- (instancetype)initWithSession:(msxsession *)session;
@end

@implementation DebuggerWindow {
    msxsession *_session;
    msxdebug *_dbg;
    msxvdp_chip _chip;
    NSWindow *_window;

    NSButton *_runBtn;
    NSTextField *_status;
    DasmTextView *_disasm;
    NSTextField *_gotoField;
    BOOL _followPc;
    uint16_t _disasmBase;

    NSTextField *_regFields[8];
    NSTextField *_flagsLabel;

    NSTextField *_memAddr;
    NSTextView *_memView;
    uint16_t _memBase;

    NSTextView *_bpView;
    NSTextField *_bpEntry;

    NSImageView *_ntView, *_patView, *_sprView, *_palView;
    NSPopUpButton *_patBank;
    NSTextView *_sprInfo;
    NSTextView *_vdpState;
    NSTextField *_vramEntry;
    NSTextField *_vramStatus;
    NSTextView *_vramView;
    uint32_t _vramBase;
}

+ (void)showForSession:(msxsession *)session
{
    if (!g_debugger) {
        g_debugger = [[DebuggerWindow alloc] initWithSession:session];
    } else {
        [g_debugger refreshAll];
    }
    [g_debugger->_window makeKeyAndOrderFront:nil];
}

static void stopCallback(void *ud, msxdebug_stop_reason reason, uint16_t pc)
{
    DebuggerWindow *self = (__bridge DebuggerWindow *)ud;
    static const char *const reasonNames[] = {"paused", "breakpoint", "step",
                                              "run-to"};
    dispatch_async(dispatch_get_main_queue(), ^{
      uint16_t off = 0;
      const char *sym = msxdebug_symbol_at(self->_dbg, pc, &off);
      NSString *msg;
      if (sym)
          msg = [NSString stringWithFormat:@"Stopped (%s) at %04X  %s+%X",
                                           reasonNames[reason], pc, sym, off];
      else
          msg = [NSString
              stringWithFormat:@"Stopped (%s) at %04X", reasonNames[reason],
                               pc];
      self->_status.stringValue = msg;
      self->_followPc = YES;
      [self refreshAll];
    });
}

- (instancetype)initWithSession:(msxsession *)session
{
    self = [super init];
    if (!self)
        return nil;
    _session = session;
    _dbg = msxsession_debugger(session);
    _chip = chipFromMachine(msxsession_machine(session));
    _followPc = YES;
    _memBase = 0;
    _vramBase = 0;
    [self buildWindow];
    msxdebug_set_stop_callback(_dbg, stopCallback, (__bridge void *)self);
    [self refreshAll];
    return self;
}

/* ---- construction helpers ---------------------------------------------------- */

static NSTextView *monoView(NSScrollView **scrollOut)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    NSTextView *view =
        [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 200)];
    view.editable = NO;
    view.richText = NO;
    view.font = [NSFont monospacedSystemFontOfSize:11
                                            weight:NSFontWeightRegular];
    view.autoresizingMask = NSViewWidthSizable;
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    return [NSButton buttonWithTitle:title target:self action:sel];
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1180, 820)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"MSX Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    /* Toolbar row */
    _runBtn = [self button:@"Pause (F5)" action:@selector(togglePause:)];
    NSButton *stepBtn = [self button:@"Step Into (F7)" action:@selector(stepInto:)];
    NSButton *stepOverBtn = [self button:@"Step Over (F8)" action:@selector(stepOver:)];
    NSButton *stepOutBtn = [self button:@"Step Out (⇧F8)" action:@selector(stepOut:)];
    _runBtn.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF5FunctionKey];
    stepBtn.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF7FunctionKey];
    stepOverBtn.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];

    _gotoField = [[NSTextField alloc] init];
    _gotoField.placeholderString = @"Go to addr / symbol";
    _gotoField.target = self;
    _gotoField.action = @selector(gotoDisasm:);

    _status = [self label:@"Running"];
    _status.alignment = NSTextAlignmentRight;

    NSStackView *toolbar = [NSStackView stackViewWithViews:@[
        _runBtn, stepBtn, stepOverBtn, stepOutBtn, _gotoField, _status
    ]];
    toolbar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    toolbar.spacing = 8;

    /* ---- CPU tab: disasm | (registers, breakpoints, memory) --------------- */
    NSScrollView *dasmScroll = [[NSScrollView alloc] init];
    dasmScroll.hasVerticalScroller = YES;
    _disasm = [[DasmTextView alloc] initWithFrame:NSMakeRect(0, 0, 560, 600)];
    _disasm.editable = NO;
    _disasm.richText = NO;
    _disasm.font = [NSFont monospacedSystemFontOfSize:11
                                               weight:NSFontWeightRegular];
    _disasm.autoresizingMask = NSViewWidthSizable;
    __weak DebuggerWindow *weakSelf = self;
    _disasm.onToggleAddr = ^(uint16_t addr) {
        DebuggerWindow *s = weakSelf;
        if (!s) return;
        msxdebug_bp_toggle(s->_dbg, addr);
        [s refreshDisasm];
        [s refreshBps];
    };
    dasmScroll.documentView = _disasm;

    NSGridView *regsGrid = [self buildRegsGrid];
    _flagsLabel = [self label:@""];

    _bpEntry = [[NSTextField alloc] init];
    _bpEntry.placeholderString = @"Add: addr or symbol";
    _bpEntry.target = self;
    _bpEntry.action = @selector(addBreakpoint:);
    NSButton *bpClear = [self button:@"Clear all" action:@selector(clearBreakpoints:)];
    NSStackView *bpRow = [NSStackView stackViewWithViews:@[ _bpEntry, bpClear ]];
    bpRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    NSScrollView *bpScroll;
    _bpView = monoView(&bpScroll);
    [bpScroll.heightAnchor constraintEqualToConstant:90].active = YES;

    _memAddr = [[NSTextField alloc] init];
    _memAddr.placeholderString = @"Memory addr / symbol";
    _memAddr.target = self;
    _memAddr.action = @selector(gotoMem:);
    NSScrollView *memScroll;
    _memView = monoView(&memScroll);

    NSStackView *side = [NSStackView stackViewWithViews:@[
        [self label:@"Registers (Enter applies while paused)"], regsGrid,
        _flagsLabel, [self label:@"Breakpoints"], bpRow, bpScroll,
        [self label:@"Memory"], _memAddr, memScroll
    ]];
    side.orientation = NSUserInterfaceLayoutOrientationVertical;
    side.alignment = NSLayoutAttributeLeading;
    side.spacing = 6;

    NSSplitView *cpuSplit = [[NSSplitView alloc] init];
    cpuSplit.vertical = YES;
    [cpuSplit addArrangedSubview:dasmScroll];
    [cpuSplit addArrangedSubview:side];

    /* ---- VDP tab ------------------------------------------------------------ */
    NSView *vdpPage = [self buildVdpPage];

    NSTabViewItem *cpuTab = [[NSTabViewItem alloc] initWithIdentifier:@"cpu"];
    cpuTab.label = @"CPU";
    cpuTab.view = cpuSplit;
    NSTabViewItem *vdpTab = [[NSTabViewItem alloc] initWithIdentifier:@"vdp"];
    vdpTab.label = @"VDP";
    vdpTab.view = vdpPage;

    NSTabView *tabs = [[NSTabView alloc] init];
    [tabs addTabViewItem:cpuTab];
    [tabs addTabViewItem:vdpTab];

    NSStackView *root = [NSStackView stackViewWithViews:@[ toolbar, tabs ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor
                                        constant:-16].active = YES;
    [tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor
                                     constant:-16].active = YES;
    [tabs.heightAnchor constraintGreaterThanOrEqualToConstant:650].active = YES;

    _window.contentView = root;
    [_window center];

    if (getenv("MSX_DEBUGGER_TAB") &&
        strcmp(getenv("MSX_DEBUGGER_TAB"), "vdp") == 0)
        [tabs selectTabViewItem:vdpTab];
}

- (NSGridView *)buildRegsGrid
{
    NSMutableArray<NSArray<NSView *> *> *rows = [NSMutableArray array];
    for (int row = 0; row < 4; row++) {
        NSMutableArray<NSView *> *cols = [NSMutableArray array];
        for (int col = 0; col < 2; col++) {
            int i = row * 2 + col;
            NSTextField *field = [[NSTextField alloc] init];
            field.tag = i;
            field.target = self;
            field.action = @selector(regFieldActivated:);
            [field.widthAnchor constraintEqualToConstant:60].active = YES;
            _regFields[i] = field;
            [cols addObject:[self label:[NSString stringWithUTF8String:kRegNames[i]]]];
            [cols addObject:field];
        }
        [rows addObject:cols];
    }
    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 4;
    grid.columnSpacing = 6;
    return grid;
}

- (NSView *)buildVdpPage
{
    _ntView = [[NSImageView alloc] init];
    [_ntView.widthAnchor constraintEqualToConstant:512].active = YES;
    [_ntView.heightAnchor constraintEqualToConstant:384].active = YES;

    _patView = [[NSImageView alloc] init];
    [_patView.widthAnchor constraintEqualToConstant:512].active = YES;
    [_patView.heightAnchor constraintEqualToConstant:128].active = YES;
    _patBank = [[NSPopUpButton alloc] init];
    [_patBank addItemsWithTitles:@[ @"Bank 0", @"Bank 1", @"Bank 2" ]];
    _patBank.target = self;
    _patBank.action = @selector(refreshVdp);
    NSStackView *patBox = [NSStackView stackViewWithViews:@[ _patBank, _patView ]];
    patBox.orientation = NSUserInterfaceLayoutOrientationVertical;

    _sprView = [[NSImageView alloc] init];
    [_sprView.widthAnchor constraintEqualToConstant:128].active = YES;
    [_sprView.heightAnchor constraintEqualToConstant:64].active = YES;
    NSScrollView *sprScroll;
    _sprInfo = monoView(&sprScroll);
    [sprScroll.widthAnchor constraintEqualToConstant:300].active = YES;
    [sprScroll.heightAnchor constraintEqualToConstant:200].active = YES;
    NSStackView *sprBox = [NSStackView stackViewWithViews:@[ _sprView, sprScroll ]];
    sprBox.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    sprBox.spacing = 8;

    _palView = [[NSImageView alloc] init];
    [_palView.widthAnchor constraintEqualToConstant:MSXVDP_PAL_W].active = YES;
    [_palView.heightAnchor constraintEqualToConstant:MSXVDP_PAL_H].active = YES;

    NSScrollView *stateScroll;
    _vdpState = monoView(&stateScroll);
    [stateScroll.widthAnchor constraintEqualToConstant:560].active = YES;
    [stateScroll.heightAnchor constraintEqualToConstant:520].active = YES;

    _vramEntry = [[NSTextField alloc] init];
    _vramEntry.placeholderString = @"addr [bytes...] e.g. 1800: 41 42 43";
    _vramEntry.target = self;
    _vramEntry.action = @selector(vramEntryActivated:);
    NSButton *vramPrev = [self button:@"◀" action:@selector(vramPagePrev:)];
    NSButton *vramNext = [self button:@"▶" action:@selector(vramPageNext:)];
    _vramStatus = [self label:@""];
    NSStackView *vramBar = [NSStackView stackViewWithViews:@[
        _vramEntry, vramPrev, vramNext, _vramStatus
    ]];
    vramBar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    NSScrollView *vramScroll;
    _vramView = monoView(&vramScroll);
    [vramScroll.heightAnchor constraintEqualToConstant:260].active = YES;
    NSStackView *vramBox = [NSStackView stackViewWithViews:@[ vramBar, vramScroll ]];
    vramBox.orientation = NSUserInterfaceLayoutOrientationVertical;

    NSGridView *grid = [NSGridView gridViewWithViews:@[
        @[ [self label:@"Nametable"], patBox ],
        @[ _ntView, sprBox ],
        @[ [self label:@"Palette"], [self label:@"Registers, status & command engine"] ],
        @[ _palView, stateScroll ],
        @[ vramBox ],
    ]];
    grid.rowSpacing = 8;
    grid.columnSpacing = 12;

    NSScrollView *outer = [[NSScrollView alloc] init];
    outer.hasVerticalScroller = YES;
    outer.documentView = grid;
    return outer;
}

/* ---- helpers ------------------------------------------------------------------ */

- (BOOL)parseAddr:(NSString *)text into:(uint16_t *)out
{
    NSString *t = [text stringByTrimmingCharactersInSet:
                            [NSCharacterSet whitespaceCharacterSet]];
    if ([t hasPrefix:@"$"])
        t = [t substringFromIndex:1];
    unsigned v = 0;
    NSScanner *scan = [NSScanner scannerWithString:t];
    if ([scan scanHexInt:&v] && scan.atEnd && v <= 0xFFFF) {
        *out = (uint16_t)v;
        return YES;
    }
    uint16_t addr;
    if (msxdebug_symbol_find(_dbg, text.UTF8String, &addr)) {
        *out = addr;
        return YES;
    }
    return NO;
}

/* ---- actions -------------------------------------------------------------------- */

- (void)togglePause:(id)sender
{
    (void)sender;
    if (msxdebug_is_paused(_dbg)) {
        msxdebug_resume(_dbg);
        _status.stringValue = @"Running";
        [self refreshAll];
    } else {
        msxdebug_pause(_dbg);
    }
}

- (void)stepInto:(id)sender { (void)sender; msxdebug_step_into(_dbg); }
- (void)stepOver:(id)sender { (void)sender; msxdebug_step_over(_dbg); }
- (void)stepOut:(id)sender { (void)sender; msxdebug_step_out(_dbg); }

- (void)gotoDisasm:(id)sender
{
    (void)sender;
    uint16_t addr;
    if ([self parseAddr:_gotoField.stringValue into:&addr]) {
        _followPc = NO;
        _disasmBase = addr;
        [self refreshDisasm];
    }
}

- (void)addBreakpoint:(id)sender
{
    (void)sender;
    uint16_t addr;
    if ([self parseAddr:_bpEntry.stringValue into:&addr]) {
        msxdebug_bp_set(_dbg, addr);
        _bpEntry.stringValue = @"";
        [self refreshBps];
        [self refreshDisasm];
    }
}

- (void)clearBreakpoints:(id)sender
{
    (void)sender;
    msxdebug_bp_clear_all(_dbg);
    [self refreshBps];
    [self refreshDisasm];
}

- (void)gotoMem:(id)sender
{
    (void)sender;
    uint16_t addr;
    if ([self parseAddr:_memAddr.stringValue into:&addr]) {
        _memBase = addr;
        [self refreshMem];
    }
}

- (void)regFieldActivated:(NSTextField *)sender
{
    if (!msxdebug_is_paused(_dbg))
        return;
    unsigned v = 0;
    NSScanner *scan = [NSScanner scannerWithString:sender.stringValue];
    if (![scan scanHexInt:&v] || v > 0xFFFF)
        return;
    msxdebug_z80_regs r;
    msxdebug_get_regs(_dbg, &r);
    regSet(&r, (int)sender.tag, (uint16_t)v);
    msxdebug_set_regs(_dbg, &r);
    [self refreshRegs];
    [self refreshDisasm];
}

- (void)vramEntryActivated:(id)sender
{
    (void)sender;
    uint8_t bytes[POKE_MAX];
    uint32_t addr;
    int n = msxvdp_parse_poke(_vramEntry.stringValue.UTF8String, &addr, bytes,
                              POKE_MAX);
    if (n < 0) {
        _vramStatus.stringValue = @"expected: addr [bytes...], e.g. 1800: 41 42";
        return;
    }
    if (n > 0) {
        msxdebug_vdp_write(_dbg, addr, bytes, n);
        _vramStatus.stringValue =
            [NSString stringWithFormat:@"wrote %d byte%s at $%05X", n,
                                       n == 1 ? "" : "s", addr];
        _vramEntry.stringValue = @"";
    } else {
        _vramStatus.stringValue = [NSString stringWithFormat:@"$%05X", addr];
    }
    _vramBase = addr & ~0xFu;
    [self refreshVdp];
}

- (void)vramPagePrev:(id)sender
{
    (void)sender;
    _vramBase = (uint32_t)((int64_t)_vramBase - VRAM_PAGE);
    [self refreshVdp];
}

- (void)vramPageNext:(id)sender
{
    (void)sender;
    _vramBase = (uint32_t)((int64_t)_vramBase + VRAM_PAGE);
    [self refreshVdp];
}

/* windowShouldClose: keep the window alive but hidden, so F12 (or the
 * Debugger menu item) reopens it instantly with its state intact -- the
 * stop callback stays registered throughout, matching the GNOME/KDE
 * windows' own always-engaged design (unlike CoCo's exec-hook engine,
 * msxdebug has no separate "engaged" toggle to disengage on close: openMSX
 * itself pays no per-instruction cost from a live callback registration,
 * only the small poll thread's ~40ms ticks). */
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    (void)sender;
    [_window orderOut:nil];
    return NO;
}

/* ---- refreshers ------------------------------------------------------------------ */

- (void)refreshAll
{
    BOOL paused = msxdebug_is_paused(_dbg) != 0;
    _runBtn.title = paused ? @"Continue (F5)" : @"Pause (F5)";
    [self refreshDisasm];
    [self refreshRegs];
    [self refreshMem];
    [self refreshBps];
    [self refreshVdp];
}

- (void)refreshDisasm
{
    msxdasm_line lines[DISASM_LINES];
    msxdebug_z80_regs r;
    msxdebug_get_regs(_dbg, &r);
    uint16_t pc = (uint16_t)((r.pch << 8) | r.pcl);
    if (_followPc)
        _disasmBase = pc;

    int n = msxdebug_disassemble(_dbg, _disasmBase, DISASM_LINES, lines);
    NSMutableString *text = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        char bytes[16] = "";
        for (int b = 0; b < lines[i].len; b++)
            snprintf(bytes + b * 3, 4, "%02X ", lines[i].bytes[b]);
        if (lines[i].symbol)
            [text appendFormat:@"%17s%s:\n", "", lines[i].symbol];
        [text appendFormat:@"%c%c%04X  %-12s %s\n",
                           msxdebug_bp_is_set(_dbg, lines[i].addr) ? '*' : ' ',
                           lines[i].addr == pc ? '>' : ' ', lines[i].addr,
                           bytes, lines[i].text];
    }
    _disasm.string = text;
}

- (void)refreshRegs
{
    msxdebug_z80_regs r;
    msxdebug_get_regs(_dbg, &r);
    for (int i = 0; i < 8; i++)
        _regFields[i].stringValue =
            [NSString stringWithFormat:@"%04X", regGet(&r, i)];
    _flagsLabel.stringValue = [NSString
        stringWithFormat:@"F: %c%c%c%c%c%c  IFF1:%d IFF2:%d IM:%d",
                         (r.f & 0x80) ? 'S' : '-', (r.f & 0x40) ? 'Z' : '-',
                         (r.f & 0x10) ? 'H' : '-', (r.f & 0x04) ? 'P' : '-',
                         (r.f & 0x02) ? 'N' : '-', (r.f & 0x01) ? 'C' : '-',
                         r.iff & 1, (r.iff >> 1) & 1, r.im];
}

- (void)refreshMem
{
    uint8_t data[MEM_ROWS * 16];
    msxdebug_read_mem(_dbg, _memBase, data, sizeof(data));
    NSMutableString *text = [NSMutableString string];
    for (int row = 0; row < MEM_ROWS; row++) {
        [text appendFormat:@"%04X  ", (unsigned)(_memBase + row * 16)];
        for (int col = 0; col < 16; col++)
            [text appendFormat:@"%02X ", data[row * 16 + col]];
        [text appendString:@" "];
        for (int col = 0; col < 16; col++) {
            uint8_t c = data[row * 16 + col];
            [text appendFormat:@"%c", (c >= 0x20 && c <= 0x7E) ? (char)c : '.'];
        }
        [text appendString:@"\n"];
    }
    _memView.string = text;
}

- (void)refreshBps
{
    uint16_t bps[128];
    int n = msxdebug_bp_list(_dbg, bps, 128);
    NSMutableString *text = [NSMutableString string];
    if (n == 0)
        [text appendString:@"(no breakpoints)\n"];
    for (int i = 0; i < n; i++) {
        uint16_t off = 0;
        const char *sym = msxdebug_symbol_at(_dbg, bps[i], &off);
        if (sym && off == 0)
            [text appendFormat:@"%04X  %s\n", bps[i], sym];
        else if (sym)
            [text appendFormat:@"%04X  %s+%X\n", bps[i], sym, off];
        else
            [text appendFormat:@"%04X\n", bps[i]];
    }
    _bpView.string = text;
}

- (void)refreshVdp
{
    static msxvdp_snapshot snap;
    static uint8_t nt[256 * 192 * 4], pat[256 * 64 * 4], spr[128 * 64 * 4],
        pal[MSXVDP_PAL_W * MSXVDP_PAL_H * 4];
    static char text[4096];
    msxvdp_sprite info[32];

    msxdebug_vdp_snapshot(_dbg, _chip, &snap);

    msxvdp_render_nametable(&snap, nt);
    _ntView.image = imageFromRGBA(nt, 256, 192);

    msxvdp_render_patterns(&snap, (int)_patBank.indexOfSelectedItem, pat);
    _patView.image = imageFromRGBA(pat, 256, 64);

    msxvdp_render_sprites(&snap, spr, info);
    _sprView.image = imageFromRGBA(spr, 128, 64);

    msxvdp_render_palette(&snap, pal);
    _palView.image = imageFromRGBA(pal, MSXVDP_PAL_W, MSXVDP_PAL_H);

    NSMutableString *sprText =
        [NSMutableString stringWithString:@"##  Y    X  PAT CLR EC\n"];
    for (int i = 0; i < 32; i++)
        [sprText appendFormat:@"%02d %3d  %3d  %02X  %2d  %d\n", i, info[i].y,
                              info[i].x, info[i].pattern, info[i].color,
                              info[i].early_clock];
    _sprInfo.string = sprText;

    msxvdp_format_state(&snap, text, (int)sizeof(text));
    _vdpState.string = [NSString stringWithUTF8String:text];

    msxvdp_format_hex(&snap, _vramBase, VRAM_ROWS, text, (int)sizeof(text));
    _vramView.string = [NSString stringWithUTF8String:text];
}

@end
