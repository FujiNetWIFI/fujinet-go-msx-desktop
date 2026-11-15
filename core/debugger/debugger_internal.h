/*
 * debugger_internal -- what the prompt (prompt.c), the machine getters
 * (iostate.c) and the memory tools (memtools.c) share with the engine
 * (debugger.c). Not installed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MSX_DEBUGGER_INTERNAL_H
#define MSX_DEBUGGER_INTERNAL_H

#include "msxdebug.h"
#include "msxsession.h"
#include "symbols.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One Tcl command in openMSX, answered synchronously; 1 on success. The
 * result (or the error message) goes into out. */
int msxdebug_rpc(const char *command, char *out, int outsz);
int msxdebug_rpcf(char *out, int outsz, const char *fmt, ...);

/* A console line for openMSX's Tcl, run under the engine's lock; 1 on success. */
int msxdebug_tcl(msxdebug *dbg, const char *command, char *out, int outsz);

/* The session the engine belongs to. */
msxsession *msxdebug_session(msxdebug *dbg);

/* Marks something a window should redraw for (a command ran). */
void msxdebug_touch(msxdebug *dbg);

/* The symbol tables, for completion (the caller must not keep the pointer
 * across a load). */
const sym_tables *msxdebug_symtabs(msxdebug *dbg);

/* The Video tab's text hook, or NULL (see msxdebug_set_video_text). */
const char *(*msxdebug_video_text_hook(void))(const char *section);

#ifdef __cplusplus
}
#endif

#endif /* MSX_DEBUGGER_INTERNAL_H */
