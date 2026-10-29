/*
 * Debugger window (AppKit) over this app's Z80 debugger engine, via
 * core/include/msxdebug.h. Mirrors the SMS sibling's macOS debugger (and the
 * GTK, Qt and Win32 debuggers), tab for tab: Console, CPU & Memory,
 * Disassembly, VDP, Breakpoints, Slots & I/O, FujiNet.
 *
 * Opening the window attaches the engine, which stops the machine; closing
 * it (F12, the close button, the menu) detaches, and the machine runs on.
 * F12 in the machine's window and the View menu toggle it.
 *
 * The engine runs openMSX on its own thread and answers every call over a
 * blocking Tcl round trip, so the window never sees a callback: a 100 ms
 * NSTimer polls msxdebug_generation() and refreshes only the tab on show,
 * and only when the generation changed. While the machine runs, that tab's
 * live values refresh twice a second (every fifth tick). After a step the
 * window waits a moment (~40 ms) for the stop to settle before it reads
 * anything back; a longer run (Frame, Step Out, Run to Cursor) is picked up
 * by the poll when it stops. msxdebug_set_stop_callback is not used: it
 * fires off the UI thread.
 *
 * Keys work anywhere in the window (a local key monitor, so the console's
 * field takes them too): F5 stop/run, F7 step, Shift+F7 frame (to the next
 * VDP interrupt), F8 step over, Shift+F8 step out, F9 toggle the breakpoint
 * on the selected disassembly line, F12 close. The MSX_DEBUGGER_TAB
 * environment variable picks the tab shown first (an index or the start of
 * a tab's name).
 *
 * The VDP tab (nametable/pattern/sprite/palette pictures, decoded
 * register/status/command-engine text, a physical-VRAM hex editor) and the
 * FujiNet tab (every FujiBus packet between the MSX and FujiNet, both ways,
 * with hex) come over unchanged from the ADAM-era window, over msxvdp.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "msxdebug.h"
#include "msxvdp.h"

#define DISASM_WINDOW 48
#define MAX_BPS 64
#define NUM_REGS 17
#define NUM_FLAGS 6
#define MEM_ROWS 32            /* the hex dump: 16 bytes a row */
#define MEM_BYTES (MEM_ROWS * 16)
#define VRAM_ROWS 16
#define VRAM_PAGE (VRAM_ROWS * 16)
#define POKE_MAX 64
#define TRACE_MAX 512          /* FujiNet packets shown */
#define TAB_VDP 3              /* the tabs with their own refresh rules */
#define TAB_FUJINET 6
#define LIVE_TICKS 5           /* 5 x 100 ms: the live refresh while running */

/* The accent: the stopped Run button and the PC's line. */
#define MSX_ACCENT_RGB 0x2E6FD8

static DebuggerWindow *g_debugger;

static NSColor *accentColor(void)
{
    return [NSColor colorWithSRGBRed:((MSX_ACCENT_RGB >> 16) & 0xff) / 255.0
                               green:((MSX_ACCENT_RGB >> 8) & 0xff) / 255.0
                                blue:(MSX_ACCENT_RGB & 0xff) / 255.0
                               alpha:1.0];
}

/* Disassembly text view: a click selects the clicked line (Run to Cursor);
 * a click on its gutter, or a double-click, toggles its breakpoint; the
 * wheel browses by instruction. */
@interface DasmTextView : NSTextView
@property (nonatomic, copy) void (^onClick)(int line, int column, NSInteger clicks);
@property (nonatomic, copy) void (^onScroll)(int rows);
@end

@implementation DasmTextView {
    CGFloat _wheel;     /* the wheel's fraction not yet scrolled */
}

- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p];
    NSString *text = self.string;
    if (idx > text.length) idx = text.length;
    int line = 0;
    NSUInteger lineStart = 0;
    for (NSUInteger i = 0; i < idx; i++)
        if ([text characterAtIndex:i] == '\n') { line++; lineStart = i + 1; }
    if (self.onClick) self.onClick(line, (int)(idx - lineStart), event.clickCount);
}

- (void)scrollWheel:(NSEvent *)event
{
    /* a notch is a few rows; a trackpad's pixels are many */
    _wheel += event.hasPreciseScrollingDeltas ? -event.scrollingDeltaY / 8.0 : -event.scrollingDeltaY * 3.0;
    const int rows = (int)_wheel;
    if (rows == 0) return;
    _wheel -= rows;
    if (self.onScroll) self.onScroll(rows);
}
@end

static NSImage *imageFromRGBA(const uint8_t *rgba, int w, int h)
{
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, rgba, (size_t)w * (size_t)h * 4, NULL);
    CGImageRef cg = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, space, kCGImageAlphaLast,
                                  provider, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);
    NSImage *img = [[NSImage alloc] initWithCGImage:cg size:NSMakeSize(w, h)];
    CGImageRelease(cg);
    return img;
}

static msxvdp_chip chipFromMachine(const char *machineId)
{
    if (!machineId) return MSXVDP_CHIP_V9938;
    if (strstr(machineId, "MSX2+")) return MSXVDP_CHIP_V9958;
    if (strstr(machineId, "MSX1")) return MSXVDP_CHIP_V9918;
    return MSXVDP_CHIP_V9938;
}

@interface DebuggerWindow () <NSTabViewDelegate>
- (instancetype)initWithSession:(msxsession *)session;
- (void)refreshNow;
@end

@implementation DebuggerWindow {
    msxsession *_session;
    msxdebug *_dbg;
    msxvdp_chip _chip;
    NSWindow *_window;
    NSTimer *_tick;
    id _keyMonitor;
    unsigned _seenGen;
    BOOL _wasStopped;
    int _liveTicks;
    NSTabView *_tabs;

    NSButton *_runBtn;
    NSTextField *_status;

    NSTextView *_conOut;
    NSTextField *_conIn;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyPos;

    NSTextField *_reg[NUM_REGS];
    NSButton *_flag[NUM_FLAGS];
    NSTextView *_mem;
    NSTextField *_memGoto, *_memWrite;
    int _memBase;               /* the dump's first address */
    int _memMark;               /* the row Go to marked, or -1 */

    NSButton *_followPc;
    NSTextField *_jump;
    DasmTextView *_disasm;
    uint16_t _disasmAddr;
    uint16_t _lineAddr[DISASM_WINDOW];
    int _lineCount;
    int _selected;              /* the selected line's address, or -1 */

    NSImageView *_ntView, *_patView, *_sprView, *_palView;
    NSPopUpButton *_patBank;
    NSTextView *_sprInfo;
    NSTextView *_vdpState;
    NSTextField *_vramEntry;
    NSTextField *_vramStatus;
    NSTextView *_vramView;
    uint32_t _vramBase;

    NSPopUpButton *_bpType;
    NSTextField *_bpStart, *_bpEnd, *_bpCond, *_bpErr;
    NSStackView *_bpList;
    NSString *_bpSig;

    NSTextView *_io;

    NSTextField *_fujiNote;
    NSTextView *_fujiText, *_fujiState;
    uint64_t _fujiLast;
    int _fujiCount;             /* what the log shows: -1 forces a rebuild */
}

#define NUM_FILE_KINDS 4
static const char *const kFileKinds[NUM_FILE_KINDS] = { "dis", "mem", "vram", "regs" };
static NSString *const kFileTitles[NUM_FILE_KINDS] = {
    @"Disassembly…", @"Memory (64K)…", @"VRAM…", @"Registers…"
};
static NSString *const kFileNames[NUM_FILE_KINDS] = {
    @"disassembly.asm", @"memory.bin", @"vram.bin", @"registers.txt"
};

/* MSX_DEBUGGER_TAB takes an index or the start of one of these. */
#define NUM_TABS 7
static const char *const kTabNames[NUM_TABS] = {
    "console", "cpu & memory", "disassembly", "vdp", "breakpoints", "slots & i/o", "fujinet"
};

/* ---- helpers --------------------------------------------------------------- */

/* The engine's text as a string: control bytes dropped (a console's CRs and
 * escapes), UTF-8 if it is, otherwise Latin-1. */
static NSString *cstr(const char *s)
{
    if (!s) return @"";
    const size_t n = strlen(s);
    char *b = malloc(n + 1);
    if (!b) return @"";
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 || c == '\n' || c == '\t') b[o++] = (char)c;
    }
    b[o] = '\0';
    NSString *r = [NSString stringWithUTF8String:b];
    if (!r) r = [[NSString alloc] initWithBytes:b length:o encoding:NSISOLatin1StringEncoding];
    free(b);
    return r ?: @"";
}

/* $hex, 0xhex, hexh, #dec, or bare hex. */
static BOOL parseNum(NSString *text, long *out)
{
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    int base = 16;
    if ([t hasPrefix:@"$"]) t = [t substringFromIndex:1];
    else if ([t.lowercaseString hasPrefix:@"0x"]) t = [t substringFromIndex:2];
    else if ([t hasPrefix:@"#"]) { t = [t substringFromIndex:1]; base = 10; }
    else if (t.length > 1 && [t.lowercaseString hasSuffix:@"h"]) t = [t substringToIndex:t.length - 1];
    if (t.length == 0) return NO;
    char *end;
    long v = strtol(t.UTF8String, &end, base);
    if (*end) return NO;
    *out = v;
    return YES;
}

/* A label, or a number. -1 when neither. */
- (int)resolveAddr:(NSString *)text
{
    long v;
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (t.length == 0) return -1;
    int addr = msxdebug_label_address(_dbg, t.UTF8String);
    if (addr < 0 && parseNum(t, &v) && v >= 0 && v <= 0xffff) addr = (int)v;
    return addr;
}

static NSFont *monoFont(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

static NSDictionary *monoAttrs(void)
{
    return @{ NSFontAttributeName: monoFont(), NSForegroundColorAttributeName: NSColor.textColor };
}

/* The accent fill with white on it: the PC's line, a marked row. */
static NSDictionary *accentAttrs(void)
{
    return @{ NSBackgroundColorAttributeName: accentColor(), NSForegroundColorAttributeName: NSColor.whiteColor };
}

static NSTextView *monoView(NSScrollView **scrollOut, BOOL wrap)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = !wrap;
    NSTextView *view = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    view.editable = NO;
    view.richText = NO;
    view.font = monoFont();
    if (wrap) {
        view.autoresizingMask = NSViewWidthSizable;
    } else {
        view.horizontallyResizable = YES;
        view.textContainer.widthTracksTextView = NO;
        view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

/* Whether a view's reader is at its end (a log then follows new lines). */
static BOOL atEnd(NSTextView *view)
{
    NSScrollView *scroll = view.enclosingScrollView;
    if (!scroll) return YES;
    return NSMaxY(scroll.contentView.documentVisibleRect) >= NSHeight(view.frame) - 4;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void setText(NSTextView *view, NSString *text, BOOL follow)
{
    if (![view.string isEqualToString:text]) {
        NSScrollView *scroll = view.enclosingScrollView;
        const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
        view.string = text;
        if (scroll && !follow) {
            [scroll.contentView scrollToPoint:at];
            [scroll reflectScrolledClipView:scroll.contentView];
        }
    }
    if (follow) [view scrollRangeToVisible:NSMakeRange(view.string.length, 0)];
}

/* The same for styled text, always where the reader left it. */
static void setStyledText(NSTextView *view, NSAttributedString *text)
{
    NSScrollView *scroll = view.enclosingScrollView;
    const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    [view.textStorage setAttributedString:text];
    if (scroll) {
        [scroll.contentView scrollToPoint:at];
        [scroll reflectScrolledClipView:scroll.contentView];
    }
}

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    NSButton *b = [NSButton buttonWithTitle:title target:self action:sel];
    [b setRefusesFirstResponder:YES];
    return b;
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

- (NSTextField *)hint:(NSString *)text
{
    NSTextField *l = [self label:text];
    l.textColor = NSColor.secondaryLabelColor;
    return l;
}

- (NSTextField *)field:(NSString *)placeholder width:(CGFloat)width action:(SEL)sel
{
    NSTextField *f = [[NSTextField alloc] init];
    f.placeholderString = placeholder;
    f.font = monoFont();
    f.target = self;
    f.action = sel;
    if (width > 0) [f.widthAnchor constraintEqualToConstant:width].active = YES;
    return f;
}

/* A pull-down button: `title` on its face, `items` underneath (each one's
 * tag is its index). */
- (NSPopUpButton *)pullDown:(NSString *)title items:(NSArray<NSString *> *)items action:(SEL)sel
{
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
    [p addItemWithTitle:title];
    p.lastItem.tag = -1;
    for (NSUInteger i = 0; i < items.count; i++) {
        [p addItemWithTitle:items[i]];
        p.lastItem.tag = (NSInteger)i;
    }
    p.target = self;
    p.action = sel;
    [p setRefusesFirstResponder:YES];
    return p;
}

static NSStackView *hstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    s.spacing = 6;
    return s;
}

static NSStackView *vstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.spacing = 6;
    return s;
}

static void fill(NSStackView *stack, NSView *view)
{
    [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
}

/* ---- lifetime -------------------------------------------------------------- */

+ (void)showForSession:(msxsession *)session
{
    if (!g_debugger) g_debugger = [[DebuggerWindow alloc] initWithSession:session];
    [g_debugger show];
}

+ (void)toggleForSession:(msxsession *)session
{
    if (g_debugger && [g_debugger->_window isVisible]) {
        /* windowWillClose: detaches */
        [g_debugger->_window close];
        return;
    }
    [self showForSession:session];
}

- (void)show
{
    if (_window.isMiniaturized) [_window deminiaturize:nil];
    [_window makeKeyAndOrderFront:nil];
    const unsigned gen = msxdebug_generation(_dbg);
    msxdebug_attach(_dbg);
    [self settleFrom:gen];
    [self refreshNow];
    if ([self currentTab] == 0) [_window makeFirstResponder:_conIn];
}

- (instancetype)initWithSession:(msxsession *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _dbg = msxsession_debugger(session);
    _chip = chipFromMachine(msxsession_machine(session));
    _fujiCount = -1;
    _memMark = -1;
    _selected = -1;
    _history = [NSMutableArray array];
    [self buildWindow];

    __weak DebuggerWindow *weakSelf = self;
    _tick = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t) {
        (void)t;
        [weakSelf onTick];
    }];
    [[NSRunLoop mainRunLoop] addTimer:_tick forMode:NSRunLoopCommonModes];

    /* F5/F7/F8/F9/F12 wherever the focus is in this window, the console's
     * field included */
    _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                        handler:^NSEvent *(NSEvent *event) {
        DebuggerWindow *s = weakSelf;
        if (!s || event.window != s->_window || !s->_window.isVisible) return event;
        return [s handleKey:event] ? nil : event;
    }];
    return self;
}

- (void)dealloc
{
    [_tick invalidate];
    if (_keyMonitor) [NSEvent removeMonitor:_keyMonitor];
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _window) return;
    msxdebug_detach(_dbg);
}

- (NSInteger)currentTab
{
    return [_tabs indexOfTabViewItem:_tabs.selectedTabViewItem];
}

/* The poll: refresh on a stop, a resume or a command (the generation), and
 * the visible tab's live values twice a second while the machine runs. The
 * FujiNet tab also follows the trace twice a second while stopped, as the
 * cartridge's packets arrive on their own. */
- (void)onTick
{
    if (![_window isVisible]) return;
    const unsigned gen = msxdebug_generation(_dbg);
    const BOOL stopped = msxdebug_is_stopped(_dbg) != 0;
    if (gen != _seenGen || stopped != _wasStopped) {
        [self refreshNow];
    } else if (++_liveTicks >= LIVE_TICKS) {
        _liveTicks = 0;
        if (!stopped) {
            [self refreshStatus];
            [self refreshTab];
        } else if ([self currentTab] == TAB_FUJINET) {
            [self refreshFujinet:NO];
        }
    }
}

/* Waits up to ~40 ms for a stop to land after the engine was asked to move:
 * the generation moves on and the machine reads as stopped. */
- (void)settleFrom:(unsigned)gen
{
    for (int i = 0; i < 20; i++) {
        if (msxdebug_generation(_dbg) != gen && msxdebug_is_stopped(_dbg)) break;
        usleep(2000);
    }
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    [self refreshTab];
    if ([self currentTab] == 0) [_window makeFirstResponder:_conIn];
}

/* ---- keys -------------------------------------------------------------------- */

- (BOOL)handleKey:(NSEvent *)event
{
    NSString *chars = event.charactersIgnoringModifiers;
    if (chars.length != 1) return NO;
    const NSEventModifierFlags mods = event.modifierFlags;
    if (mods & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption)) return NO;
    const BOOL shift = (mods & NSEventModifierFlagShift) != 0;
    switch ([chars characterAtIndex:0]) {
    case NSF5FunctionKey: [self toggleRun:nil]; return YES;
    case NSF7FunctionKey: if (shift) [self frame:nil]; else [self step:nil]; return YES;
    case NSF8FunctionKey: if (shift) [self stepOut:nil]; else [self stepOver:nil]; return YES;
    case NSF9FunctionKey: [self toggleSelected:nil]; return YES;
    case NSF12FunctionKey: [_window performClose:nil]; return YES;
    default: return NO;
    }
}

/* ---- construction ------------------------------------------------------------ */

- (NSView *)buildConsole
{
    NSScrollView *outScroll;
    _conOut = monoView(&outScroll, YES);
    _conOut.string = @"Debugger console. Type 'help' for every command; anything else goes to openMSX's own console.\n";
    _conIn = [self field:@"command (help, step, break $0038, bpw $C000, regs, mem $C000 32, ...) — Tab completes, ↑↓ history"
                   width:0 action:@selector(runCommand:)];
    _conIn.delegate = self;

    NSStackView *v = vstack(@[outScroll, _conIn]);
    fill(v, outScroll);
    fill(v, _conIn);
    return v;
}

- (NSView *)buildCpu
{
    static NSString *const names[NUM_REGS] = {
        @"AF", @"BC", @"DE", @"HL", @"IX", @"IY", @"SP", @"PC",
        @"AF'", @"BC'", @"DE'", @"HL'", @"I", @"R", @"IM", @"IFF1", @"IFF2"
    };
    NSMutableArray *row1 = [NSMutableArray array], *row2 = [NSMutableArray array];
    for (int i = 0; i < NUM_REGS; i++) {
        /* the 16-bit pairs, then I and R, then IM and the two flip-flops */
        const CGFloat width = i < 12 ? 52 : (i < 14 ? 36 : 28);
        NSMutableArray *row = i < 8 ? row1 : row2;
        [row addObject:[self label:names[i]]];
        _reg[i] = [self field:@"" width:width action:@selector(applyRegister:)];
        _reg[i].tag = i;
        [row addObject:_reg[i]];
    }
    static NSString *const fnames[NUM_FLAGS] = { @"S", @"Z", @"H", @"PV", @"N", @"C" };
    NSMutableArray *flags = [NSMutableArray arrayWithObject:[self label:@"Flags"]];
    for (int i = 0; i < NUM_FLAGS; i++) {
        _flag[i] = [NSButton checkboxWithTitle:fnames[i] target:self action:@selector(flagClicked:)];
        _flag[i].tag = i;
        [flags addObject:_flag[i]];
    }
    NSTextField *editHint = [self hint:@"Registers and flags can be edited while the machine is stopped"];
    [flags addObject:editHint];

    NSScrollView *memScroll;
    _mem = monoView(&memScroll, NO);
    _memGoto = [self field:@"address or label" width:150 action:@selector(gotoMem:)];
    NSButton *prev = [self button:@"◀" action:@selector(memPrev:)];
    NSButton *next = [self button:@"▶" action:@selector(memNext:)];
    NSStackView *memHead = hstack(@[[self label:@"Memory (as the CPU sees it)"], [self label:@"Go to"], _memGoto,
                                    prev, next]);
    _memWrite = [self field:@"address value" width:170 action:@selector(writeMem:)];
    NSStackView *edit = hstack(@[[self label:@"Write"], _memWrite,
                                 [self hint:@"RAM takes it; ROM ignores it. Several values go to consecutive addresses"]]);

    NSStackView *v = vstack(@[hstack(row1), hstack(row2), hstack(flags), memHead, memScroll, edit]);
    fill(v, memScroll);
    return v;
}

- (NSView *)buildDisasm
{
    _followPc = [NSButton checkboxWithTitle:@"Follow PC" target:self action:@selector(followToggled:)];
    _followPc.state = NSControlStateValueOn;
    _jump = [self field:@"address or label" width:150 action:@selector(jumpTo:)];
    NSStackView *row = hstack(@[_followPc, [self label:@"Jump to"], _jump,
                                [self hint:@"Click a line to select it (Run to Cursor); click its left edge, double-click it "
                                           @"or press F9 for a breakpoint (* on, o off); scroll to browse"]]);

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = NO;
    scroll.hasHorizontalScroller = YES;
    _disasm = [[DasmTextView alloc] initWithFrame:NSMakeRect(0, 0, 600, 600)];
    _disasm.editable = NO;
    _disasm.richText = NO;
    _disasm.selectable = NO;
    _disasm.font = monoFont();
    _disasm.horizontallyResizable = YES;
    _disasm.textContainer.widthTracksTextView = NO;
    _disasm.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _disasm.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    __weak DebuggerWindow *weakSelf = self;
    _disasm.onClick = ^(int line, int column, NSInteger clicks) {
        DebuggerWindow *s = weakSelf;
        if (!s || line < 0 || line >= s->_lineCount) return;
        const BOOL gutter = column < 2;
        s->_selected = s->_lineAddr[line];
        if (gutter ? clicks == 1 : clicks == 2) [s toggleBreakpointAt:s->_lineAddr[line]];
        else [s refreshDisasm];
    };
    _disasm.onScroll = ^(int rows) {
        DebuggerWindow *s = weakSelf;
        if (!s || rows == 0) return;
        s->_followPc.state = NSControlStateValueOff;
        s->_disasmAddr = (uint16_t)msxdebug_row_address(s->_dbg, s->_disasmAddr, rows);
        [s refreshDisasm];
    };
    scroll.documentView = _disasm;

    NSStackView *v = vstack(@[row, scroll]);
    fill(v, row);
    fill(v, scroll);
    return v;
}

- (NSView *)buildVdp
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
    _patBank.action = @selector(vdpViewChanged:);
    NSStackView *patBox = vstack(@[ _patBank, _patView ]);

    _sprView = [[NSImageView alloc] init];
    [_sprView.widthAnchor constraintEqualToConstant:128].active = YES;
    [_sprView.heightAnchor constraintEqualToConstant:64].active = YES;
    NSScrollView *sprScroll;
    _sprInfo = monoView(&sprScroll, YES);
    [sprScroll.widthAnchor constraintEqualToConstant:300].active = YES;
    [sprScroll.heightAnchor constraintEqualToConstant:200].active = YES;
    NSStackView *sprBox = hstack(@[ _sprView, sprScroll ]);
    sprBox.spacing = 8;

    _palView = [[NSImageView alloc] init];
    [_palView.widthAnchor constraintEqualToConstant:MSXVDP_PAL_W].active = YES;
    [_palView.heightAnchor constraintEqualToConstant:MSXVDP_PAL_H].active = YES;

    NSScrollView *stateScroll;
    _vdpState = monoView(&stateScroll, YES);
    [stateScroll.widthAnchor constraintEqualToConstant:560].active = YES;
    [stateScroll.heightAnchor constraintEqualToConstant:520].active = YES;

    _vramEntry = [self field:@"addr [bytes...] e.g. 1800: 41 42 43" width:0 action:@selector(vramEntryActivated:)];
    NSButton *vramPrev = [self button:@"◀" action:@selector(vramPagePrev:)];
    NSButton *vramNext = [self button:@"▶" action:@selector(vramPageNext:)];
    _vramStatus = [self label:@""];
    NSStackView *vramBar = hstack(@[ _vramEntry, vramPrev, vramNext, _vramStatus ]);
    NSScrollView *vramScroll;
    _vramView = monoView(&vramScroll, NO);
    [vramScroll.heightAnchor constraintEqualToConstant:260].active = YES;
    NSStackView *vramBox = vstack(@[ vramBar, vramScroll ]);

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

/* One breakpoint-table cell: a fixed-width monospace label, or (width 0) the
 * flexible one that takes what is left. */
static NSTextField *bpCell(NSString *text, CGFloat width, BOOL bold)
{
    NSTextField *l = [NSTextField labelWithString:text];
    l.font = bold ? [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightSemibold] : monoFont();
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    if (width > 0) {
        [l.widthAnchor constraintEqualToConstant:width].active = YES;
    } else {
        [l setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    }
    return l;
}

- (NSView *)buildBreaks
{
    /* the table's rows scroll: a session can collect more breakpoints than
     * the tab is tall. The stack is the scroll view's document, pinned to
     * the top and to the clip view's width so rows keep the tab's width. */
    NSStackView *header = hstack(@[bpCell(@"On  #", 70, YES), bpCell(@"Type", 60, YES), bpCell(@"Range", 190, YES),
                                   bpCell(@"Condition", 0, YES), bpCell(@"Hits", 60, YES), bpCell(@"", 70, YES)]);
    header.distribution = NSStackViewDistributionFill;
    _bpList = vstack(@[]);
    _bpList.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *bpScroll = [[NSScrollView alloc] init];
    bpScroll.hasVerticalScroller = YES;
    bpScroll.drawsBackground = NO;
    bpScroll.documentView = _bpList;
    NSClipView *clip = bpScroll.contentView;
    [_bpList.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [_bpList.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [_bpList.widthAnchor constraintEqualToAnchor:clip.widthAnchor].active = YES;

    _bpType = [[NSPopUpButton alloc] init];
    [_bpType addItemsWithTitles:@[@"exec", @"read", @"write", @"in", @"out"]];
    _bpStart = [self field:@"$4010, a label, or port $98" width:190 action:@selector(addBreakpoint:)];
    _bpEnd = [self field:@"end (optional)" width:120 action:@selector(addBreakpoint:)];
    _bpCond = [self field:@"condition (optional): [reg A] == 5" width:0 action:@selector(addBreakpoint:)];
    NSButton *add = [self button:@"Add" action:@selector(addBreakpoint:)];
    NSButton *clear = [self button:@"Clear All" action:@selector(clearBreaks:)];
    NSStackView *row = hstack(@[_bpType, [self label:@"Start"], _bpStart, [self label:@"End"], _bpEnd,
                                [self label:@"Condition"], _bpCond, add, clear]);
    row.distribution = NSStackViewDistributionFill;
    _bpErr = [self label:@""];
    _bpErr.textColor = NSColor.systemRedColor;
    NSTextField *hint = [self hint:@"Untick to disable. A condition is a Tcl expression, e.g. [reg A] == 5. "
                                   @"A port is 0-255 for in/out; the end defaults to the start."];

    NSStackView *v = vstack(@[header, bpScroll, hint, row, _bpErr]);
    fill(v, header);
    fill(v, bpScroll);
    fill(v, row);
    [bpScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return v;
}

- (NSView *)buildIo
{
    NSScrollView *scroll;
    _io = monoView(&scroll, NO);
    return scroll;
}

/* The packets left (a note and Clear over them), the cartridge's link and
 * counts right. */
- (NSView *)buildFujinet
{
    _fujiNote = [self label:@""];
    _fujiNote.textColor = NSColor.secondaryLabelColor;
    _fujiNote.lineBreakMode = NSLineBreakByTruncatingTail;
    [_fujiNote setContentHuggingPriority:NSLayoutPriorityDefaultLow
                          forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_fujiNote setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = hstack(@[_fujiNote, [self button:@"Clear" action:@selector(fujiClear:)]]);

    NSScrollView *fujiScroll, *stateScroll;
    _fujiText = monoView(&fujiScroll, NO);
    _fujiState = monoView(&stateScroll, NO);
    NSTextField *title = [self label:@"FujiBus packets (SLIP-decoded)"];
    title.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSStackView *left = vstack(@[title, row, fujiScroll]);
    left.distribution = NSStackViewDistributionFill;   /* the log takes the column's height */
    fill(left, row);
    fill(left, fujiScroll);
    NSTextField *stateTitle = [self label:@"FujiNet cartridge"];
    stateTitle.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSStackView *right = vstack(@[stateTitle, stateScroll]);
    right.distribution = NSStackViewDistributionFill;
    fill(right, stateScroll);

    NSStackView *h = hstack(@[left, right]);
    h.alignment = NSLayoutAttributeTop;
    h.spacing = 12;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:520].active = YES;
    [right.widthAnchor constraintEqualToConstant:320].active = YES;
    [left.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    [right.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    return h;
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1150, 800)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    /* Toolbar row. The keys are handled by the key monitor, not as the
     * buttons' key equivalents, so they work wherever the focus is. */
    _runBtn = [self button:@"Stop (F5)" action:@selector(toggleRun:)];
    NSButton *step = [self button:@"Step (F7)" action:@selector(step:)];
    NSButton *over = [self button:@"Step Over (F8)" action:@selector(stepOver:)];
    NSButton *outBtn = [self button:@"Step Out (⇧F8)" action:@selector(stepOut:)];
    NSButton *toCursor = [self button:@"Run to Cursor" action:@selector(runToCursor:)];
    NSButton *frame = [self button:@"Frame (⇧F7)" action:@selector(frame:)];
    NSPopUpButton *symbols = [self pullDown:@"Symbols" items:@[@"Load Symbols…"] action:@selector(loadSymbols:)];
    NSMutableArray *saveTitles = [NSMutableArray array];
    for (int i = 0; i < NUM_FILE_KINDS; i++) [saveTitles addObject:kFileTitles[i]];
    NSPopUpButton *save = [self pullDown:@"Save…" items:saveTitles action:@selector(saveFile:)];
    _status = [self label:@"Running"];
    _status.alignment = NSTextAlignmentRight;
    _status.textColor = NSColor.secondaryLabelColor;
    _status.lineBreakMode = NSLineBreakByTruncatingTail;
    NSStackView *toolbar = hstack(@[_runBtn, step, over, outBtn, toCursor, frame, symbols, save, _status]);
    [_status setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];

    _tabs = [[NSTabView alloc] init];
    NSArray *pages = @[ @[@"Console", [self buildConsole]], @[@"CPU & Memory", [self buildCpu]],
                        @[@"Disassembly", [self buildDisasm]], @[@"VDP", [self buildVdp]],
                        @[@"Breakpoints", [self buildBreaks]], @[@"Slots & I/O", [self buildIo]],
                        @[@"FujiNet", [self buildFujinet]] ];
    for (NSArray *page in pages) {
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:page[0]];
        item.label = page[0];
        NSView *content = page[1];
        NSView *holder = [[NSView alloc] init];
        [holder addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
        [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
        [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
        item.view = holder;
        [_tabs addTabViewItem:item];
    }
    const char *tab = getenv("MSX_DEBUGGER_TAB");
    if (tab && *tab) {
        int t = -1;
        if (tab[0] >= '0' && tab[0] <= '9') {
            t = atoi(tab);
        } else {
            for (int i = 0; i < NUM_TABS && t < 0; i++)
                if (!strncasecmp(tab, kTabNames[i], strlen(tab))) t = i;
        }
        if (t >= 0 && t < (int)_tabs.numberOfTabViewItems) [_tabs selectTabViewItemAtIndex:t];
    }
    _tabs.delegate = self;

    NSStackView *root = vstack(@[toolbar, _tabs]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    _window.contentView = root;
    [_window center];
}

/* ---- actions ------------------------------------------------------------------ */

/* A line for the status label (and the console's log, when asked). */
- (void)message:(NSString *)text console:(BOOL)console
{
    _status.stringValue = text;
    if (console) [self appendConsole:[text stringByAppendingString:@"\n"]];
}

- (void)toggleRun:(id)sender
{
    (void)sender;
    const unsigned gen = msxdebug_generation(_dbg);
    if (msxdebug_is_stopped(_dbg)) msxdebug_resume(_dbg); else msxdebug_stop(_dbg);
    [self settleFrom:gen];
    [self refreshNow];
}

/* An action that moves the machine: the stop settles (or the poll catches
 * it, for a long run), then everything shown is read back. */
- (void)moved:(unsigned)gen
{
    [self settleFrom:gen];
    [self refreshNow];
}

- (void)step:(id)sender { (void)sender; const unsigned g = msxdebug_generation(_dbg); msxdebug_step(_dbg); [self moved:g]; }
- (void)stepOver:(id)sender { (void)sender; const unsigned g = msxdebug_generation(_dbg); msxdebug_step_over(_dbg); [self moved:g]; }
- (void)stepOut:(id)sender { (void)sender; const unsigned g = msxdebug_generation(_dbg); msxdebug_step_out(_dbg); [self moved:g]; }
- (void)frame:(id)sender { (void)sender; const unsigned g = msxdebug_generation(_dbg); msxdebug_frame(_dbg); [self moved:g]; }

- (void)runToCursor:(id)sender
{
    (void)sender;
    if (_selected < 0) {
        [self message:@"Select a line in the Disassembly to run to" console:NO];
        return;
    }
    const unsigned g = msxdebug_generation(_dbg);
    msxdebug_run_to(_dbg, (uint16_t)_selected);
    [self moved:g];
}

- (void)toggleBreakpointAt:(uint16_t)addr
{
    msxdebug_breakpoint_toggle(_dbg, addr);
    [self refreshDisasm];
}

/* F9 */
- (void)toggleSelected:(id)sender
{
    (void)sender;
    if (_selected < 0) {
        [self message:@"Select a line in the Disassembly to toggle its breakpoint" console:NO];
        return;
    }
    [self toggleBreakpointAt:(uint16_t)_selected];
    if ([self currentTab] == 4) [self refreshBps];
}

- (void)fujiClear:(id)sender
{
    (void)sender;
    msxdebug_fujinet_trace_clear(_dbg);
    [self refreshFujinet:YES];
}

- (void)appendConsole:(NSString *)text
{
    const BOOL follow = atEnd(_conOut);     /* a reader scrolled back keeps their place */
    [_conOut.textStorage appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:monoAttrs()]];
    if (follow) [_conOut scrollRangeToVisible:NSMakeRange(_conOut.string.length, 0)];
}

- (void)runCommand:(id)sender
{
    (void)sender;
    static char out[65536];
    NSString *cmd = [_conIn.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (cmd.length == 0) return;
    if (![_history.lastObject isEqualToString:cmd]) [_history addObject:cmd];
    _historyPos = (NSInteger)_history.count;
    [self appendConsole:[NSString stringWithFormat:@"> %@\n", cmd]];
    out[0] = '\0';
    const unsigned gen = msxdebug_generation(_dbg);
    msxdebug_command(_dbg, cmd.UTF8String, out, sizeof out);
    NSString *text = cstr(out);
    if (text.length) [self appendConsole:[text hasSuffix:@"\n"] ? text : [text stringByAppendingString:@"\n"]];
    _conIn.stringValue = @"";
    [self settleFrom:gen];
    [self refreshNow];
}

/* In the console, Tab completes instead of moving the focus, and Up/Down
 * walk the history. */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel
{
    (void)textView;
    if (control != _conIn) return NO;
    if (sel == @selector(moveUp:) || sel == @selector(moveDown:)) {
        const NSInteger n = (NSInteger)_history.count;
        if (n == 0) return YES;
        _historyPos += sel == @selector(moveUp:) ? -1 : 1;
        if (_historyPos < 0) _historyPos = 0;
        if (_historyPos > n) _historyPos = n;
        _conIn.stringValue = _historyPos < n ? _history[(NSUInteger)_historyPos] : @"";
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
        return YES;
    }
    if (sel != @selector(insertTab:)) return NO;
    /* the engine takes the whole line and completes its last word */
    NSString *text = _conIn.stringValue;
    const NSRange sp = [text rangeOfString:@" " options:NSBackwardsSearch];
    char comps[4096];
    comps[0] = '\0';
    const int n = msxdebug_completions(_dbg, text.UTF8String, comps, sizeof comps);
    NSString *list = cstr(comps);
    if (n == 1) {
        NSString *c = [list componentsSeparatedByString:@"\n"][0];
        NSString *head = sp.location == NSNotFound ? @"" : [text substringToIndex:sp.location + 1];
        _conIn.stringValue = [NSString stringWithFormat:@"%@%@ ", head, c];
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
    } else if (n > 1) {
        if (![list hasSuffix:@"\n"]) list = [list stringByAppendingString:@"\n"];
        [self appendConsole:list];
    }
    return YES;
}

- (void)loadSymbols:(NSPopUpButton *)sender
{
    (void)sender;
    char msg[512];
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.title = @"Load Symbols (\"HHHH NAME\" per line)";
    if ([p runModal] != NSModalResponseOK) return;
    msg[0] = '\0';
    msxdebug_load_symbols(_dbg, [[p URL] fileSystemRepresentation], msg, sizeof msg);
    [self message:cstr(msg) console:YES];
    [self refreshNow];
}

- (void)saveFile:(NSPopUpButton *)sender
{
    const NSInteger kind = sender.selectedItem.tag;
    if (kind < 0 || kind >= NUM_FILE_KINDS) return;
    NSSavePanel *p = [NSSavePanel savePanel];
    p.title = [NSString stringWithFormat:@"Save %@", kFileTitles[kind]];
    [p setNameFieldStringValue:kFileNames[kind]];
    if ([p runModal] != NSModalResponseOK) return;
    char msg[512];
    msg[0] = '\0';
    msxdebug_save(_dbg, kFileKinds[kind], [[p URL] fileSystemRepresentation], msg, sizeof msg);
    [self message:cstr(msg) console:YES];
}

- (void)applyRegister:(NSTextField *)sender
{
    static const int regIds[NUM_REGS] = {
        MSX_REG_AF, MSX_REG_BC, MSX_REG_DE, MSX_REG_HL, MSX_REG_IX, MSX_REG_IY, MSX_REG_SP, MSX_REG_PC,
        MSX_REG_AF2, MSX_REG_BC2, MSX_REG_DE2, MSX_REG_HL2, MSX_REG_I, MSX_REG_R, MSX_REG_IM,
        MSX_REG_IFF1, MSX_REG_IFF2
    };
    long v;
    [_window makeFirstResponder:nil];       /* leave the field, so the refresh may rewrite it */
    if (!msxdebug_is_stopped(_dbg)) {
        [self message:@"Stop the machine (F5) to edit registers" console:NO];
    } else if (sender.tag >= 0 && sender.tag < NUM_REGS && parseNum(sender.stringValue, &v)) {
        msxdebug_cpu_set(_dbg, regIds[sender.tag], (int)v);
    }
    [self refreshCpu];
    if ([self currentTab] == 2) [self refreshDisasm];
}

- (void)flagClicked:(NSButton *)sender
{
    static const int flagIds[NUM_FLAGS] = { MSX_FLAG_S, MSX_FLAG_Z, MSX_FLAG_H, MSX_FLAG_PV, MSX_FLAG_N, MSX_FLAG_C };
    if (msxdebug_is_stopped(_dbg))
        msxdebug_cpu_set(_dbg, flagIds[sender.tag], sender.state == NSControlStateValueOn);
    else
        [self message:@"Stop the machine (F5) to edit flags" console:NO];
    [self refreshCpu];
}

/* Brings an address's row into the dump (a couple of rows down from the
 * top) and marks it. */
- (void)showMemAddr:(int)addr
{
    _memBase = (addr & ~0xF) - 2 * 16;
    _memMark = addr;
    [self refreshMem];
}

- (void)gotoMem:(id)sender
{
    (void)sender;
    const int a = [self resolveAddr:_memGoto.stringValue];
    if (a < 0) {
        [self message:[NSString stringWithFormat:@"Not an address or label: %@", _memGoto.stringValue] console:NO];
        return;
    }
    [self showMemAddr:a];
}

- (void)memPrev:(id)sender { (void)sender; _memBase -= MEM_BYTES; [self refreshMem]; }
- (void)memNext:(id)sender { (void)sender; _memBase += MEM_BYTES; [self refreshMem]; }

/* "address value [value...]": the values land at consecutive addresses. */
- (void)writeMem:(id)sender
{
    (void)sender;
    NSArray<NSString *> *words = [_memWrite.stringValue componentsSeparatedByCharactersInSet:
        [NSCharacterSet whitespaceCharacterSet]];
    NSMutableArray<NSString *> *tok = [NSMutableArray array];
    for (NSString *w in words) if (w.length) [tok addObject:w];
    const int a = tok.count > 0 ? [self resolveAddr:tok[0]] : -1;
    if (tok.count < 2 || a < 0) {
        [self message:@"Write: address value [value...]" console:NO];
        return;
    }
    for (NSUInteger i = 1; i < tok.count; i++) {
        long v;
        if (!parseNum(tok[i], &v) || v < 0 || v > 0xFF || a + (int)i - 1 > 0xFFFF) {
            [self message:[NSString stringWithFormat:@"Not a byte: %@", tok[i]] console:NO];
            return;
        }
        msxdebug_write(_dbg, (uint16_t)(a + (int)i - 1), (uint8_t)v);
    }
    _memWrite.stringValue = @"";
    if (a < _memBase || a >= _memBase + MEM_BYTES) [self showMemAddr:a];
    else [self refreshMem];
}

- (void)followToggled:(id)sender
{
    (void)sender;
    [self refreshDisasm];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    const int addr = [self resolveAddr:_jump.stringValue];
    if (addr < 0) {
        [self message:[NSString stringWithFormat:@"Not an address or label: %@", _jump.stringValue] console:NO];
        return;
    }
    _followPc.state = NSControlStateValueOff;
    _disasmAddr = (uint16_t)addr;
    _selected = addr;
    [self refreshDisasm];
}

- (void)addBreakpoint:(id)sender
{
    (void)sender;
    static const int types[5] = { MSXDEBUG_BP_EXEC, MSXDEBUG_BP_READ, MSXDEBUG_BP_WRITE, MSXDEBUG_BP_IN,
                                  MSXDEBUG_BP_OUT };
    const NSInteger sel = _bpType.indexOfSelectedItem;
    const int type = types[sel >= 0 && sel < 5 ? sel : 0];
    const BOOL port = (type & (MSXDEBUG_BP_IN | MSXDEBUG_BP_OUT)) != 0;
    NSString *what = port ? @"port" : @"address";
    NSString *startText = [_bpStart.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    NSString *endText = [_bpEnd.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (startText.length == 0) {
        _bpErr.stringValue = [NSString stringWithFormat:@"Give a %@", what];
        return;
    }
    const int start = [self resolveAddr:startText];
    const int end = endText.length ? [self resolveAddr:endText] : start;
    if (start < 0 || end < 0 || end < start || (port && end > 0xFF)) {
        _bpErr.stringValue = [NSString stringWithFormat:@"Bad %@ range: %@%@%@", what, startText,
                              endText.length ? @" - " : @"", endText];
        return;
    }
    char err[256];
    err[0] = '\0';
    const int bpId = msxdebug_breakpoint_add(_dbg, type, (uint16_t)start, (uint16_t)end,
                                             _bpCond.stringValue.UTF8String, err, sizeof err);
    if (bpId < 0) {
        NSString *e = cstr(err);
        _bpErr.stringValue = e.length ? e : @"The breakpoint was refused";
        return;
    }
    _bpErr.stringValue = @"";
    _bpStart.stringValue = @"";
    _bpEnd.stringValue = @"";
    _bpCond.stringValue = @"";
    [self refreshNow];
}

- (void)bpEnabled:(NSButton *)sender
{
    msxdebug_breakpoint_enable(_dbg, (int)sender.tag, sender.state == NSControlStateValueOn);
    [self refreshNow];
}

- (void)bpRemove:(NSButton *)sender
{
    msxdebug_breakpoint_remove(_dbg, (int)sender.tag);
    [self refreshNow];
}

- (void)clearBreaks:(id)sender
{
    (void)sender;
    msxdebug_breakpoint_clear(_dbg);
    [self refreshNow];
}

- (void)vdpViewChanged:(id)sender
{
    (void)sender;
    [self refreshVdp];
}

- (void)vramEntryActivated:(id)sender
{
    (void)sender;
    uint8_t bytes[POKE_MAX];
    uint32_t addr;
    int n = msxvdp_parse_poke(_vramEntry.stringValue.UTF8String, &addr, bytes, POKE_MAX);
    if (n < 0) {
        _vramStatus.stringValue = @"expected: addr [bytes...], e.g. 1800: 41 42";
        return;
    }
    if (n > 0) {
        msxdebug_vdp_write(_dbg, addr, bytes, n);
        _vramStatus.stringValue = [NSString stringWithFormat:@"wrote %d byte%s at $%05X", n, n == 1 ? "" : "s", addr];
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

/* ---- refreshers ------------------------------------------------------------------ */

/* Everything the window shows, read back now: the status and the visible
 * tab. The poll compares against what was seen here. */
- (void)refreshNow
{
    _seenGen = msxdebug_generation(_dbg);
    _wasStopped = msxdebug_is_stopped(_dbg) != 0;
    _liveTicks = 0;
    [self refreshStatus];
    [self refreshTab];
}

/* Only the tab on show: the others refresh when they are selected. */
- (void)refreshTab
{
    switch ([self currentTab]) {
    case 1: [self refreshCpu]; [self refreshMem]; break;
    case 2: [self refreshDisasm]; break;
    case TAB_VDP: [self refreshVdp]; break;
    case 4: [self refreshBps]; break;
    case 5: [self refreshIo]; break;
    case TAB_FUJINET: [self refreshFujinet:NO]; break;
    default: break;
    }
}

- (void)refreshStatus
{
    char reason[160];
    int addr;
    const BOOL stopped = msxdebug_is_stopped(_dbg) != 0;
    reason[0] = '\0';
    msxdebug_stop_reason(_dbg, reason, sizeof reason, &addr);
    NSString *why = cstr(reason);
    _status.stringValue = stopped ? [NSString stringWithFormat:@"Stopped%@%@", why.length ? @": " : @"", why]
                                  : @"Running";
    _runBtn.title = stopped ? @"Run (F5)" : @"Stop (F5)";
    _runBtn.bezelColor = stopped ? accentColor() : nil;
}

- (void)refreshCpu
{
    msxdebug_cpu c;
    msxdebug_cpu_get(_dbg, &c);
    const int vals[NUM_REGS] = {
        c.af, c.bc, c.de, c.hl, c.ix, c.iy, c.sp, c.pc, c.af2, c.bc2, c.de2, c.hl2, c.i, c.r, c.im, c.iff1, c.iff2
    };
    for (int i = 0; i < NUM_REGS; i++)
        if (!_reg[i].currentEditor)         /* never over a field being edited */
            _reg[i].stringValue = [NSString stringWithFormat:(i < 12 ? @"%04X" : (i < 14 ? @"%02X" : @"%d")), vals[i]];
    const int flags[NUM_FLAGS] = { c.sf, c.zf, c.hf, c.pf, c.nf, c.cf };
    for (int i = 0; i < NUM_FLAGS; i++) _flag[i].state = flags[i] ? NSControlStateValueOn : NSControlStateValueOff;
}

- (void)refreshMem
{
    uint8_t data[MEM_BYTES];
    if (_memBase > 0x10000 - MEM_BYTES) _memBase = 0x10000 - MEM_BYTES;
    if (_memBase < 0) _memBase = 0;
    memset(data, 0, sizeof data);
    msxdebug_read(_dbg, (uint16_t)_memBase, data, MEM_BYTES);
    NSMutableString *text = [NSMutableString stringWithString:@"      0  1  2  3  4  5  6  7   8  9  A  B  C  D  E  F\n"];
    NSRange mark = NSMakeRange(NSNotFound, 0);
    for (int row = 0; row < MEM_ROWS; row++) {
        const int base = _memBase + row * 16;
        const NSUInteger start = text.length;
        [text appendFormat:@"%04X  ", base];
        for (int col = 0; col < 16; col++)
            [text appendFormat:(col == 8 ? @" %02X " : @"%02X "), data[row * 16 + col]];
        [text appendString:@" "];
        for (int col = 0; col < 16; col++) {
            const uint8_t c = data[row * 16 + col];
            [text appendFormat:@"%c", (c >= 0x20 && c < 0x7F) ? (char)c : '.'];
        }
        if (_memMark >= base && _memMark < base + 16) mark = NSMakeRange(start, text.length - start);
        [text appendString:@"\n"];
    }
    NSMutableAttributedString *as = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    if (mark.location != NSNotFound) [as addAttributes:accentAttrs() range:mark];
    setStyledText(_mem, as);
}

- (void)refreshDisasm
{
    static msxdebug_line lines[DISASM_WINDOW];
    int pcLine = -1;
    if (_followPc.state == NSControlStateValueOn) {
        msxdebug_cpu c;
        msxdebug_cpu_get(_dbg, &c);
        /* the PC a third of the way down */
        _disasmAddr = (uint16_t)msxdebug_row_address(_dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }
    int n = msxdebug_disassemble(_dbg, _disasmAddr, lines, DISASM_WINDOW, &pcLine);
    if (n < 0) n = 0;
    if (n > DISASM_WINDOW) n = DISASM_WINDOW;
    _lineCount = n;

    NSMutableString *text = [NSMutableString string];
    NSRange ranges[DISASM_WINDOW];
    for (int i = 0; i < n; i++) {
        _lineAddr[i] = lines[i].address;
        const char gutter = !lines[i].has_breakpoint ? ' ' : (lines[i].breakpoint_enabled ? '*' : 'o');
        NSString *line = [NSString stringWithFormat:@"%c%c %04X  %-11s  %-16s %-24s%s%s", gutter,
                          lines[i].is_pc ? '>' : ' ', lines[i].address, lines[i].bytes, lines[i].label,
                          lines[i].disasm, lines[i].comment[0] ? " ; " : "", lines[i].comment];
        ranges[i] = NSMakeRange(text.length, line.length);
        [text appendString:line];
        [text appendString:@"\n"];
    }
    if (n == 0) [text appendString:@"(no disassembly: the debugger is not attached)\n"];

    NSMutableAttributedString *as = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    for (int i = 0; i < n; i++) {
        /* the gutter: a breakpoint red, a disabled one grey */
        if (lines[i].has_breakpoint)
            [as addAttribute:NSForegroundColorAttributeName
                       value:lines[i].breakpoint_enabled ? NSColor.systemRedColor : NSColor.systemGrayColor
                       range:NSMakeRange(ranges[i].location, 1)];
        /* the selected line underlined on a selection fill */
        if ((int)lines[i].address == _selected)
            [as addAttributes:@{ NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle),
                                 NSBackgroundColorAttributeName: NSColor.selectedTextBackgroundColor }
                        range:ranges[i]];
    }
    /* the PC's line in the accent colour, last so its colours win */
    for (int i = 0; i < n; i++) {
        if (!lines[i].is_pc) continue;
        [as addAttributes:accentAttrs() range:ranges[i]];
        if ((int)lines[i].address == _selected)
            [as addAttribute:NSUnderlineStyleAttributeName value:@(NSUnderlineStyleSingle) range:ranges[i]];
    }
    [_disasm.textStorage setAttributedString:as];
    NSScrollView *scroll = _disasm.enclosingScrollView;
    [scroll.contentView scrollToPoint:NSMakePoint(scroll.contentView.bounds.origin.x, 0)];
    [scroll reflectScrolledClipView:scroll.contentView];
}

- (void)refreshVdp
{
    static msxvdp_snapshot snap;
    static uint8_t nt[256 * 192 * 4], pat[256 * 64 * 4], spr[128 * 64 * 4], pal[MSXVDP_PAL_W * MSXVDP_PAL_H * 4];
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

    NSMutableString *sprText = [NSMutableString stringWithString:@"##  Y    X  PAT CLR EC\n"];
    for (int i = 0; i < 32; i++)
        [sprText appendFormat:@"%02d %3d  %3d  %02X  %2d  %d\n", i, info[i].y, info[i].x, info[i].pattern,
                              info[i].color, info[i].early_clock];
    setText(_sprInfo, sprText, NO);

    text[0] = '\0';
    msxvdp_format_state(&snap, text, (int)sizeof(text));
    setText(_vdpState, cstr(text), NO);

    text[0] = '\0';
    msxvdp_format_hex(&snap, _vramBase, VRAM_ROWS, text, (int)sizeof(text));
    setText(_vramView, cstr(text), NO);
}

- (void)refreshBps
{
    static msxdebug_breakpoint bps[MAX_BPS];
    int n = msxdebug_breakpoint_list(_dbg, bps, MAX_BPS);
    if (n < 0) n = 0;
    if (n > MAX_BPS) n = MAX_BPS;

    /* the rows are rebuilt only when something in them changed (a refresh
     * must not eat a click) */
    NSMutableString *sig = [NSMutableString string];
    for (int i = 0; i < n; i++)
        [sig appendFormat:@"%d %d %d %d %d %u %s\n", bps[i].id, bps[i].type, bps[i].start, bps[i].end,
                          bps[i].enabled, bps[i].hits, bps[i].condition];
    if ([sig isEqualToString:_bpSig]) return;
    _bpSig = sig;

    for (NSView *v in [_bpList.arrangedSubviews copy]) [_bpList removeView:v];
    if (n == 0) [_bpList addArrangedSubview:[self label:@"No breakpoints"]];
    for (int i = 0; i < n; i++) {
        static const struct { int bit; const char *name; } kinds[5] = {
            { MSXDEBUG_BP_EXEC, "exec" }, { MSXDEBUG_BP_READ, "read" }, { MSXDEBUG_BP_WRITE, "write" },
            { MSXDEBUG_BP_IN, "in" }, { MSXDEBUG_BP_OUT, "out" },
        };
        NSMutableString *type = [NSMutableString string];
        for (int k = 0; k < 5; k++)
            if (bps[i].type & kinds[k].bit) {
                if (type.length) [type appendString:@"/"];
                [type appendFormat:@"%s", kinds[k].name];
            }
        const BOOL port = (bps[i].type & (MSXDEBUG_BP_IN | MSXDEBUG_BP_OUT)) != 0;
        NSMutableString *range = [NSMutableString stringWithFormat:(port ? @"port $%02X" : @"$%04X"), bps[i].start];
        if (bps[i].end != bps[i].start) [range appendFormat:(port ? @"-$%02X" : @"-$%04X"), bps[i].end];
        char label[64];
        label[0] = '\0';
        if (!port && msxdebug_address_label(_dbg, bps[i].start, label, sizeof label) > 0 && label[0])
            [range appendFormat:@" %s", label];

        NSButton *box = [NSButton checkboxWithTitle:[NSString stringWithFormat:@"#%d", bps[i].id]
                                             target:self action:@selector(bpEnabled:)];
        box.state = bps[i].enabled ? NSControlStateValueOn : NSControlStateValueOff;
        box.tag = bps[i].id;
        box.font = monoFont();
        [box.widthAnchor constraintEqualToConstant:70].active = YES;
        NSButton *remove = [self button:@"Remove" action:@selector(bpRemove:)];
        remove.tag = bps[i].id;
        [remove.widthAnchor constraintEqualToConstant:70].active = YES;
        NSStackView *row = hstack(@[box, bpCell(type, 60, NO), bpCell(range, 190, NO),
                                    bpCell(cstr(bps[i].condition), 0, NO),
                                    bpCell([NSString stringWithFormat:@"%u", bps[i].hits], 60, NO), remove]);
        row.distribution = NSStackViewDistributionFill;
        [_bpList addArrangedSubview:row];
        [row.widthAnchor constraintEqualToAnchor:_bpList.widthAnchor].active = YES;
    }
}

- (void)refreshIo
{
    static char text[8192];
    text[0] = '\0';
    msxdebug_io_state(_dbg, text, (int)sizeof(text));
    setText(_io, cstr(text), NO);
}

/* The log is rebuilt only when it changed: a new packet, or a repeat folded
 * into the newest. Built as UTF-8 first: %s in an NSString format is not
 * UTF-8. */
- (void)refreshFujinet:(BOOL)force
{
    static msx_fuji_packet p[TRACE_MAX];
    static char t[TRACE_MAX * 336 + 128]; /* an entry is at most ~330 */
    static char state[2048];
    int n = msxdebug_fujinet_trace(_dbg, p, TRACE_MAX);
    size_t len = 0;
    if (n < 0) n = 0;

    if (msxsession_fujinet_external(_session))
        _fujiNote.stringValue = [NSString
            stringWithFormat:@"External FujiNet %s, cartridge %s.",
                             msxsession_fujinet_endpoint(_session),
                             msxsession_fujinet_link(_session)
                                 ? "connected" : "not connected"];
    else if (!msxsession_fujinet_running(_session))
        _fujiNote.stringValue = @"FujiNet is off (disabled, or its runtime did not start).";
    else
        _fujiNote.stringValue = msxsession_fujinet_link(_session)
                                    ? @"FujiNet is on, cartridge connected."
                                    : @"FujiNet is on, cartridge not connected.";
    state[0] = '\0';
    msxdebug_fujinet_state(_dbg, state, (int)sizeof(state));
    setText(_fujiState, cstr(state), NO);

    if (!force && n == _fujiCount && (n == 0 || p[n - 1].serial == _fujiLast))
        return;
    const BOOL follow = force || _fujiCount < 0 || atEnd(_fujiText);
    _fujiCount = n;
    _fujiLast = n > 0 ? p[n - 1].serial : 0;
    t[0] = '\0';
    for (int i = 0; i < n; i++) {
        len += (size_t)snprintf(t + len, sizeof(t) - len, "%s%s", i ? "\n" : "", p[i].text);
        if (p[i].repeat > 1)
            len += (size_t)snprintf(t + len, sizeof(t) - len, " \xc3\x97%u", p[i].repeat);
        len += (size_t)snprintf(t + len, sizeof(t) - len, "\n     ");
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof(p[i].data); k++)
            len += (size_t)snprintf(t + len, sizeof(t) - len, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof(p[i].data))
            len += (size_t)snprintf(t + len, sizeof(t) - len, "\xe2\x80\xa6");
    }
    if (n == 0)
        snprintf(t, sizeof(t), "(no FujiBus packets since the trace was cleared)");
    setText(_fujiText, cstr(t), follow);
}

@end
