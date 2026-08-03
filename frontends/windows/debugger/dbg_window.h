/*
 * Debugger window for the Windows frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSX_WIN_DBG_WINDOW_H
#define MSX_WIN_DBG_WINDOW_H

#include <windows.h>

#include "msxsession.h"

/* Shows (creating on first use) the debugger window for the session. */
void msx_debugger_show(HWND parent, msxsession *session);

/* Called from the frontend's message pump before TranslateMessage so the
 * debugger's function-key accelerators work while a child control (an edit
 * field, say) has the focus. Returns 1 when the message was consumed. */
int msx_debugger_pretranslate(MSG *msg);

#endif /* MSX_WIN_DBG_WINDOW_H */
