/*
 * DisplayView: paints the emulator's latest frame with CoreGraphics,
 * letterboxed to 4:3 (MSX's frame is always exactly MSXSESSION_FB_WIDTH x
 * MSXSESSION_FB_HEIGHT = 640x480, already 4:3 -- openMSX's own
 * FrameSource::getLinePtr640_480() fixes this before msxsession ever sees
 * it, unlike the CoCo/ADAM sibling cores' variable geometry, so unlike
 * their DisplayView.m there is no dynamic _fbW/_fbH, no aspect-mode
 * setting, and no smooth-scaling toggle here -- always nearest-neighbour,
 * matching the GNOME/KDE frontends' own fixed-size textures). A
 * CVDisplayLink provides the vsync ticks that feed the session's
 * (advisory-only for MSX -- see msxsession.h) notify_vsync and schedules
 * main-thread repaints.
 *
 * Ported from fujinet-go-coco-desktop's DisplayView.m, not copied
 * line-for-line: the core's XRGB8888 packing and the CVDisplayLink/
 * CGImage machinery are unchanged (same conclusion the GNOME frontend's
 * own comment draws about byte order), but the frame size is a compile-
 * time constant instead of a per-frame dynamic value, and there is no
 * COCOSESSION_FB_MAX_WIDTH-sized scratch buffer to size for the largest
 * possible mode -- MSXSESSION_FB_WIDTH x MSXSESSION_FB_HEIGHT is the only
 * size that ever exists. Keyboard forwarding (msxsession_key wants an
 * X11/xkb keysym or 0, plus the raw unicode character -- see
 * core/src/input_map.c's msx_key_from_event, which msxsession_key already
 * calls internally, including the F10/F11/F12 reservation check, so this
 * view needs no special-case swallowing of its own) is unchanged from
 * CoCo's table.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DisplayView.h"

#import <CoreVideo/CoreVideo.h>

#include <time.h>

@implementation DisplayView {
    msxsession *_session;
    CVDisplayLinkRef _link;
    uint32_t *_buf; /* MSXSESSION_FB_WIDTH * MSXSESSION_FB_HEIGHT, XRGB8888 */
    uint64_t _serial;
    CGImageRef _image;
}

static int64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static CVReturn link_cb(CVDisplayLinkRef link, const CVTimeStamp *now,
                        const CVTimeStamp *output, CVOptionFlags flagsIn,
                        CVOptionFlags *flagsOut, void *ctx)
{
    DisplayView *view = (__bridge DisplayView *)ctx;
    (void)link; (void)now; (void)output; (void)flagsIn; (void)flagsOut;
    [view onVsync];
    return kCVReturnSuccess;
}

- (instancetype)initWithSession:(msxsession *)session
{
    self = [super initWithFrame:NSMakeRect(0, 0, 800, 600)];
    if (!self)
        return nil;
    _session = session;
    _buf = calloc((size_t)MSXSESSION_FB_WIDTH * MSXSESSION_FB_HEIGHT,
                 sizeof(uint32_t));
    return self;
}

- (void)dealloc
{
    [self stop];
    if (_image)
        CGImageRelease(_image);
    free(_buf);
}

/* CVDisplayLink is deprecated in macOS 15 in favor of NSView.displayLink
 * (macOS 14+), but the retro community runs plenty of older Intel Macs
 * that never see macOS 14 -- so the older API stays, deliberately, same
 * call as the CoCo/ADAM siblings. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
- (void)start
{
    if (_link)
        return;
    CVDisplayLinkCreateWithActiveCGDisplays(&_link);
    CVDisplayLinkSetOutputCallback(_link, link_cb, (__bridge void *)self);
    CVDisplayLinkStart(_link);
}

- (void)stop
{
    if (_link) {
        CVDisplayLinkStop(_link);
        CVDisplayLinkRelease(_link);
        _link = NULL;
    }
}
#pragma clang diagnostic pop

/* Display-link thread: feed the (advisory) vsync notification, then hop to
 * the main thread to pull and paint whatever frame is latest. */
- (void)onVsync
{
    msxsession_notify_vsync(_session, monotonic_ns());
    dispatch_async(dispatch_get_main_queue(), ^{
        [self pullFrame];
    });
}

- (void)pullFrame
{
    if (!msxsession_copy_frame(_session, _buf, &_serial))
        return;

    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        NULL, _buf, (size_t)MSXSESSION_FB_WIDTH * MSXSESSION_FB_HEIGHT * 4,
        NULL);
    CGImageRef image = CGImageCreate(
        (size_t)MSXSESSION_FB_WIDTH, (size_t)MSXSESSION_FB_HEIGHT, 8, 32,
        (size_t)MSXSESSION_FB_WIDTH * 4, space,
        kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider,
        NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);

    if (_image)
        CGImageRelease(_image);
    _image = image;
    [self setNeedsDisplay:YES];
}

- (NSRect)destRect
{
    const CGFloat w = self.bounds.size.width;
    const CGFloat h = self.bounds.size.height;
    const CGFloat aspect =
        (CGFloat)MSXSESSION_FB_WIDTH / (CGFloat)MSXSESSION_FB_HEIGHT;
    CGFloat dw, dh;

    if (w / h > aspect) {
        dh = h;
        dw = h * aspect;
    } else {
        dw = w;
        dh = w / aspect;
    }
    return NSMakeRect((w - dw) / 2.0, (h - dh) / 2.0, dw, dh);
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [[NSColor blackColor] setFill];
    NSRectFill(self.bounds);
    if (!_image)
        return;

    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    /* Always nearest-neighbour: no smooth-scaling preference exists for
     * this target (see this file's header comment). */
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextDrawImage(ctx, NSRectToCGRect([self destRect]), _image);
}

/* ---- input -------------------------------------------------------------------
 * msxsession_key wants an X11/xkb keysym (or 0, meaning "look at unicode
 * instead") plus the raw unicode character -- the session does the MSX-key
 * translation itself via msx_key_from_event (which also declines F10/F11/
 * F12, reserved for the frontend -- menu/fullscreen/debugger here -- so no
 * special-case swallowing is needed in this view). Table unchanged from
 * the CoCo/ADAM siblings: NSEvent.charactersIgnoringModifiers already
 * gives a layout-aware unshifted character, and these are already X11
 * keysym values (e.g. 0xFF52 = XK_Up). */

- (BOOL)acceptsFirstResponder
{
    return YES;
}

static uint32_t keysym_for_char(unichar c)
{
    switch (c) {
    case NSUpArrowFunctionKey:    return 0xFF52;
    case NSDownArrowFunctionKey:  return 0xFF54;
    case NSLeftArrowFunctionKey:  return 0xFF51;
    case NSRightArrowFunctionKey: return 0xFF53;
    case NSHomeFunctionKey:       return 0xFF50;
    case NSEndFunctionKey:        return 0xFF57;
    case NSPageUpFunctionKey:     return 0xFF55;
    case NSPageDownFunctionKey:   return 0xFF56;
    case NSInsertFunctionKey:     return 0xFF63;
    case NSDeleteFunctionKey:     return 0xFFFF; /* forward delete */
    case 0x7F:                    return 0xFF08; /* backspace */
    case 0x0D: case 0x03:         return 0xFF0D; /* return / enter */
    case 0x09:                    return 0xFF09; /* tab */
    case 0x1B:                    return 0xFF1B; /* escape */
    default:
        if (c >= NSF1FunctionKey && c <= NSF1FunctionKey + 11)
            return 0xFFBE + (uint32_t)(c - NSF1FunctionKey);
        return 0;
    }
}

- (void)forwardKeyEvent:(NSEvent *)event down:(int)down
{
    NSString *chars = event.charactersIgnoringModifiers;
    unichar c = chars.length ? [chars characterAtIndex:0] : 0;
    const int ctrl = (event.modifierFlags & NSEventModifierFlagControl) != 0;
    const int shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
    uint32_t keysym = keysym_for_char(c);
    uint32_t unicode = (c >= 0x20 && c < 0xF700) ? c : 0;

    if (!keysym && !unicode)
        return; /* not a key the emulated keyboard has */
    msxsession_key(_session, down, keysym, unicode, ctrl, shift);
}

- (void)keyDown:(NSEvent *)event
{
    [self forwardKeyEvent:event down:1];
}

- (void)keyUp:(NSEvent *)event
{
    [self forwardKeyEvent:event down:0];
}

@end
