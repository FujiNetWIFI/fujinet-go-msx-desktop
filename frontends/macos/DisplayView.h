/*
 * DisplayView: the emulator video view for the macOS frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#include "msxsession.h"

@interface DisplayView : NSView

- (instancetype)initWithSession:(msxsession *)session;
- (void)start;
- (void)stop;

@end
