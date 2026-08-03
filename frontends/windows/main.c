/*
 * FujiNet Go MSX -- native Win32 frontend.
 *
 * Mirrors the GNOME/KDE/macOS frontends over the shared msxsession API: a
 * GDI-blitted display letterboxed to 4:3, a present thread that feeds the
 * session's (advisory-only, see msxsession.h) vsync notification from
 * DwmFlush, full keyboard mapping, a menu bar, media import, and the
 * FujiNet configuration (default browser -- no embedded WebView2 in this
 * first pass) and console-log windows.
 *
 * Gamepads and audio are handled inside openMSX's own SDL2 thread; there is
 * nothing for this file to do for them, the same as every other frontend
 * (see msxsession.h's header comment).
 *
 * Ported from fujinet-go-coco-desktop's Windows frontend, not copied
 * line-for-line -- see frontends/windows/debugger/dbg_window.c's file
 * comment for the debugger side of what differs. The main differences
 * here: msxsession_copy_frame reports a FIXED frame size (MSXSESSION_FB_
 * WIDTH x MSXSESSION_FB_HEIGHT, always exactly 640x480 -- openMSX's own
 * FrameSource::getLinePtr640_480() fixes this before msxsession ever sees
 * it), not a dynamic one, so there is no per-frame w/h to track and the
 * destination rect is always a 4:3 letterbox; MSX has no TV-input/Artifact-
 * Colour/CPU equivalents and no aspect-mode/smooth-scaling settings at all
 * (see resource.h), so Machine is the only menu radio group, it is a fixed
 * 3-entry table rather than a runtime-walked name function, and there is no
 * Settings window; machine switches apply live via msxsession_set_machine
 * (openMSX's own Reactor::switchMachine), so no session restart is needed
 * anywhere in this file, unlike CoCo's ROM-import/Settings paths.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "msxsession.h"
#include "debugger/dbg_window.h"
#include "resource.h"

static msxsession *g_session;
static HWND g_hwnd;
static HMENU g_menu;
static HWND g_log_window;
static HWND g_log_edit;

/* Frame hand-off: the present thread copies the latest frame under this
 * lock; WM_PAINT blits it. Fixed size -- see this file's header comment. */
static CRITICAL_SECTION g_fb_lock;
static uint32_t g_frame[MSXSESSION_FB_WIDTH * MSXSESSION_FB_HEIGHT];
static int g_have_frame;
static uint64_t g_serial;

static pthread_t g_present_thread;
static volatile int g_present_run;
static int g_fullscreen;
static WINDOWPLACEMENT g_prev_placement = {sizeof(g_prev_placement)};

/* Fixed 3-entry table, matching the GNOME/KDE/macOS frontends' own
 * k_machines/kMachines. */
static const struct { const char *label; const char *id; } kMachines[] = {
    {"MSX", MSXSESSION_MACHINE_MSX},
    {"MSX2", MSXSESSION_MACHINE_MSX2},
    {"MSX2+", MSXSESSION_MACHINE_MSX2P},
};
#define kMachineCount (sizeof(kMachines) / sizeof(kMachines[0]))

static int machine_index(const char *machine_id)
{
    unsigned i;
    for (i = 0; i < kMachineCount; i++)
        if (strcmp(kMachines[i].id, machine_id) == 0)
            return (int)i;
    return -1;
}

static int64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---- present thread: vsync notification + frame hand-off ------------------ */

static void *present_main(void *arg)
{
    (void)arg;
    while (g_present_run) {
        BOOL composited = FALSE;
        DwmIsCompositionEnabled(&composited);
        if (composited && SUCCEEDED(DwmFlush())) {
            /* DwmFlush returned at the compositor's vsync. */
        } else {
            Sleep(16); /* no DWM composition (RDP, safe mode): wall clock */
        }
        msxsession_notify_vsync(g_session, monotonic_ns());

        EnterCriticalSection(&g_fb_lock);
        {
            int got = msxsession_copy_frame(g_session, g_frame, &g_serial);
            if (got)
                g_have_frame = 1;
            LeaveCriticalSection(&g_fb_lock);
            if (got)
                InvalidateRect(g_hwnd, NULL, FALSE);
        }
    }
    return NULL;
}

/* ---- display ---------------------------------------------------------------
 * MSXSESSION_FB_WIDTH x MSXSESSION_FB_HEIGHT is always exactly 4:3, but the
 * window itself need not be, so this still letterboxes. No aspect-mode or
 * smooth-scaling setting exists for this target (see this file's header
 * comment) -- always nearest-neighbour (COLORONCOLOR). */

static RECT dest_rect(int cw, int ch)
{
    const double aspect =
        (double)MSXSESSION_FB_WIDTH / (double)MSXSESSION_FB_HEIGHT;
    double dw, dh;
    RECT r;

    if ((double)cw / ch > aspect) {
        dh = ch;
        dw = ch * aspect;
    } else {
        dw = cw;
        dh = cw / aspect;
    }
    r.left = (LONG)((cw - dw) / 2);
    r.top = (LONG)((ch - dh) / 2);
    r.right = r.left + (LONG)dw;
    r.bottom = r.top + (LONG)dh;
    return r;
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT client;
    BITMAPINFO bmi;
    RECT d;

    GetClientRect(hwnd, &client);
    FillRect(hdc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));

    EnterCriticalSection(&g_fb_lock);
    if (g_have_frame) {
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = MSXSESSION_FB_WIDTH;
        bmi.bmiHeader.biHeight = -MSXSESSION_FB_HEIGHT; /* top-down */
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        d = dest_rect(client.right, client.bottom);
        SetStretchBltMode(hdc, COLORONCOLOR);
        StretchDIBits(hdc, d.left, d.top, d.right - d.left, d.bottom - d.top,
                      0, 0, MSXSESSION_FB_WIDTH, MSXSESSION_FB_HEIGHT,
                      g_frame, &bmi, DIB_RGB_COLORS, SRCCOPY);
    }
    LeaveCriticalSection(&g_fb_lock);

    EndPaint(hwnd, &ps);
}

/* ---- keyboard ----------------------------------------------------------------
 * msxsession_key wants an X11/xkb keysym (or 0, meaning "look at unicode
 * instead" -- see core/src/input_map.c's msx_key_from_event, which every
 * frontend's translation ultimately feeds, and which already declines
 * F10/F11/F12 on its own, so no special-case swallowing is needed below
 * beyond catching those three for this window's own menu/fullscreen/
 * debugger actions). Win32's VK_* codes carry no shift state of their own
 * (VK_A is the same code whether or not shift is held), so rather than
 * reconstruct a shifted symbol with ToUnicode (and have to keep key-down
 * and key-up consistent despite the transient modifier state), non-special
 * keys are resolved straight to their base US-layout character -- 'a', '1',
 * '-', ... -- stable across down/up by construction. */

static unsigned special_keysym(WPARAM vk)
{
    switch (vk) {
    case VK_RETURN:   return 0xFF0D;
    case VK_ESCAPE:   return 0xFF1B;
    case VK_TAB:      return 0xFF09;
    case VK_BACK:     return 0xFF08;
    case VK_DELETE:   return 0xFFFF;
    case VK_INSERT:   return 0xFF63;
    case VK_HOME:     return 0xFF50;
    case VK_END:      return 0xFF57;
    case VK_PRIOR:    return 0xFF55; /* Page Up */
    case VK_NEXT:     return 0xFF56; /* Page Down */
    case VK_UP:       return 0xFF52;
    case VK_DOWN:     return 0xFF54;
    case VK_LEFT:     return 0xFF51;
    case VK_RIGHT:    return 0xFF53;
    case VK_CAPITAL:  return 0xFFE5;
    case VK_LSHIFT:   return 0xFFE1;
    case VK_RSHIFT:   return 0xFFE2;
    case VK_LCONTROL: return 0xFFE3;
    case VK_RCONTROL: return 0xFFE4;
    case VK_LMENU:    return 0xFFE9;
    case VK_RMENU:    return 0xFFEA;
    case VK_LWIN:     return 0xFFEB;
    case VK_RWIN:     return 0xFFEC;
    case VK_NUMPAD0:  return 0xFFB0;
    case VK_DECIMAL:  return 0xFFAE;
    case VK_DIVIDE:   return 0xFFAF;
    case VK_MULTIPLY: return 0xFFAA;
    case VK_SUBTRACT: return 0xFFAD;
    case VK_ADD:      return 0xFFAB;
    default:
        if (vk >= VK_F1 && vk <= VK_F12)
            return 0xFFBE + (unsigned)(vk - VK_F1);
        if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9)
            return 0xFFB1 + (unsigned)(vk - VK_NUMPAD1);
        return 0;
    }
}

/* The base (unshifted) US-layout character for a printable key -- see the
 * file comment above. Matches core/src/input_map.c's printable_scancode()
 * switch one for one. */
static uint32_t base_char(WPARAM vk)
{
    if (vk >= 'A' && vk <= 'Z') return (uint32_t)(vk - 'A' + 'a');
    if (vk >= '0' && vk <= '9') return (uint32_t)vk;
    switch (vk) {
    case VK_SPACE:      return ' ';
    case VK_OEM_MINUS:  return '-';
    case VK_OEM_PLUS:   return '=';
    case VK_OEM_4:      return '[';
    case VK_OEM_6:      return ']';
    case VK_OEM_5:      return '\\';
    case VK_OEM_1:      return ';';
    case VK_OEM_7:      return '\'';
    case VK_OEM_3:      return '`';
    case VK_OEM_COMMA:  return ',';
    case VK_OEM_PERIOD: return '.';
    case VK_OEM_2:      return '/';
    default:            return 0;
    }
}

/* WM_(SYS)KEYDOWN/UP only report the generic VK_SHIFT/CONTROL/MENU; the
 * left/right pair is recovered from the scancode (Shift) or the extended-key
 * bit (Control/Alt), the standard Win32 idiom for this. */
static WPARAM resolve_side(WPARAM vk, LPARAM lp)
{
    UINT scancode;
    int extended;

    switch (vk) {
    case VK_SHIFT:
        scancode = (UINT)((lp >> 16) & 0xFF);
        return MapVirtualKeyA(scancode, MAPVK_VSC_TO_VK_EX);
    case VK_CONTROL:
        extended = (lp & 0x01000000) != 0;
        return extended ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
        extended = (lp & 0x01000000) != 0;
        return extended ? VK_RMENU : VK_LMENU;
    default:
        return vk;
    }
}

static void toggle_fullscreen(HWND hwnd);
static void open_debugger(void);

static void on_key(HWND hwnd, WPARAM vk, LPARAM lp, int down)
{
    WPARAM rvk;
    unsigned keysym;
    uint32_t unicode = 0;
    int ctrl, shift;

    if (down) {
        switch (vk) {
        case VK_F10:
            return; /* let the system open the menu */
        case VK_F11:
            toggle_fullscreen(hwnd);
            return;
        case VK_F12:
            open_debugger();
            return;
        default:
            break;
        }
    } else if (vk == VK_F10 || vk == VK_F11 || vk == VK_F12) {
        return;
    }

    rvk = resolve_side(vk, lp);
    keysym = special_keysym(rvk);
    if (!keysym) {
        unicode = base_char(rvk);
        if (!unicode)
            return; /* not a key the emulated keyboard has */
    }
    ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    msxsession_key(g_session, down, keysym, unicode, ctrl, shift);
}

/* ---- menu actions ------------------------------------------------------------ */

static int open_file(HWND hwnd, const char *filter, char *out, DWORD outsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    memset(out, 0, outsz);
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = out;
    ofn.nMaxFile = outsz;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static void import_media(HWND hwnd)
{
    char src[MAX_PATH], dest[1024];
    if (!open_file(hwnd,
                   "MSX disk, cassette and cartridge images (*.dsk;*.cas;*.rom)\0"
                   "*.dsk;*.cas;*.rom\0",
                   src, sizeof(src)))
        return;
    if (msxsession_import_media(g_session, src, dest, sizeof(dest)) != 0)
        MessageBoxA(hwnd, msxsession_last_error(g_session), "FujiNet Go MSX",
                    MB_ICONWARNING);
}

static void show_fujinet_config(void)
{
    ShellExecuteA(NULL, "open", msxsession_fujinet_webui_url(g_session), NULL,
                 NULL, SW_SHOWNORMAL);
}

/* ---- console log window -------------------------------------------------------- */

#define LOG_TIMER_ID 1

static LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        MoveWindow(g_log_edit, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    case WM_TIMER: {
        static char buf[128 * 1024];
        int n = msxsession_fujinet_copy_log(g_session, buf, sizeof(buf));
        SetWindowTextA(g_log_edit, n > 0 ? buf : "(no FujiNet output yet)");
        SendMessageA(g_log_edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        SendMessageA(g_log_edit, EM_SCROLLCARET, 0, 0);
        return 0;
    }
    case WM_CLOSE:
        KillTimer(hwnd, LOG_TIMER_ID);
        DestroyWindow(hwnd);
        g_log_window = NULL;
        g_log_edit = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_fujinet_log(HINSTANCE inst)
{
    static int registered;
    if (g_log_window) {
        SetForegroundWindow(g_log_window);
        return;
    }
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = log_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = "MsxLogWindow";
        RegisterClassA(&wc);
        registered = 1;
    }
    g_log_window = CreateWindowA(
        "MsxLogWindow", "FujiNet Console Log", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 820, 560, NULL, NULL, inst, NULL);
    g_log_edit = CreateWindowA(
        "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL,
        0, 0, 0, 0, g_log_window, NULL, inst, NULL);
    SendMessageA(g_log_edit, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT),
                TRUE);
    SetTimer(g_log_window, LOG_TIMER_ID, 1000, NULL);
    ShowWindow(g_log_window, SW_SHOW);
}

static void open_debugger(void)
{
    msx_debugger_show(g_hwnd, g_session);
}

/* ---- fullscreen ---------------------------------------------------------------- */

static void toggle_fullscreen(HWND hwnd)
{
    DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi = {sizeof(mi)};
        if (GetWindowPlacement(hwnd, &g_prev_placement) &&
            GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY),
                           &mi)) {
            SetWindowLongPtr(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetMenu(hwnd, NULL);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = 1;
        }
    } else {
        SetWindowLongPtr(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetMenu(hwnd, g_menu);
        SetWindowPlacement(hwnd, &g_prev_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = 0;
    }
}

/* ---- menu -----------------------------------------------------------------------
 * Machine is a fixed 3-entry radio group (MSX has no TV-input/Artifact-
 * Colour/CPU equivalents -- see this file's header comment), built from
 * kMachines rather than a runtime-walked name table. */

static HMENU build_menu(void)
{
    HMENU bar = CreateMenu();
    HMENU machine = CreatePopupMenu();
    HMENU media = CreatePopupMenu();
    HMENU fujinet = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU help = CreatePopupMenu();
    int current = machine_index(msxsession_machine(g_session));
    unsigned i;

    for (i = 0; i < kMachineCount; i++)
        AppendMenuA(machine, MF_STRING, IDM_MACHINE_BASE + i,
                   kMachines[i].label);
    if (current >= 0)
        CheckMenuRadioItem(machine, IDM_MACHINE_BASE,
                           IDM_MACHINE_BASE + (unsigned)kMachineCount - 1,
                           (UINT)(IDM_MACHINE_BASE + current), MF_BYCOMMAND);
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_RESET, "Reset");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_EXIT, "E&xit");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)machine, "&Machine");

    AppendMenuA(media, MF_STRING, IDM_IMPORT_MEDIA,
               "Import Disk, Cassette or Cartridge...");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)media, "M&edia");

    AppendMenuA(fujinet, MF_STRING, IDM_FUJINET_CONFIG, "Configuration...");
    AppendMenuA(fujinet, MF_STRING, IDM_FUJINET_LOG, "Console Log...");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)fujinet, "&FujiNet");

    AppendMenuA(view, MF_STRING, IDM_FULLSCREEN, "Fullscreen\tF11");
    AppendMenuA(view, MF_STRING, IDM_DEBUGGER, "Debugger\tF12");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)view, "&View");

    AppendMenuA(help, MF_STRING, IDM_ABOUT, "About FujiNet Go MSX");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "&Help");
    return bar;
}

/* Rebuilds and re-attaches the menu bar after a machine switch, so the radio
 * check mark tracks the new selection. Frees the previous HMENU -- Win32
 * does not do that automatically when SetMenu replaces one. */
static void rebuild_menu(HWND hwnd)
{
    HMENU old = g_menu;
    g_menu = build_menu();
    SetMenu(hwnd, g_menu);
    DrawMenuBar(hwnd);
    if (old)
        DestroyMenu(old);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1; /* paint() clears; avoid flicker */
    case WM_SIZE:
        /* A resize only invalidates the newly exposed strips, and BeginPaint
         * clips the repaint to them -- the rest of the client area would keep
         * the image blitted at the old size (visible as a ghost after
         * maximize or F11). Ask for the whole client area instead. */
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        on_key(hwnd, wp, lp, 1);
        /* F10 falls through to DefWindowProc so the system opens the menu;
         * everything else is fully handled above. */
        if (wp == VK_F10)
            break;
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        on_key(hwnd, wp, lp, 0);
        if (wp == VK_F10)
            break;
        return 0;
    case WM_COMMAND: {
        unsigned id = LOWORD(wp);
        if (id >= IDM_MACHINE_BASE && id < IDM_MACHINE_BASE + IDM_RADIO_SPAN &&
            id - IDM_MACHINE_BASE < kMachineCount) {
            msxsession_set_machine(g_session,
                                   kMachines[id - IDM_MACHINE_BASE].id);
            rebuild_menu(hwnd);
        } else switch (id) {
        case IDM_RESET:          msxsession_reset(g_session); break;
        case IDM_IMPORT_MEDIA:   import_media(hwnd); break;
        case IDM_FUJINET_CONFIG: show_fujinet_config(); break;
        case IDM_FUJINET_LOG:
            show_fujinet_log((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE));
            break;
        case IDM_FULLSCREEN: toggle_fullscreen(hwnd); break;
        case IDM_DEBUGGER:   open_debugger(); break;
        case IDM_ABOUT:
            MessageBoxA(hwnd,
                        "FujiNet Go MSX\n"
                        "Self-contained MSX with built-in FujiNet.\n"
                        "Copyright (C) 2026 Thomas Cherryhomes\n"
                        "GPL-3.0-or-later",
                        "About FujiNet Go MSX", MB_ICONINFORMATION);
            break;
        case IDM_EXIT: DestroyWindow(hwnd); break;
        default: break;
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSEXA wc;
    MSG msg;
    msxsession_start_opts opts;
    (void)prev;
    (void)cmd;

    SetProcessDPIAware();
    InitializeCriticalSection(&g_fb_lock);

    g_session = msxsession_new(NULL);
    if (!g_session) {
        MessageBoxA(NULL, "Could not create the session.", "FujiNet Go MSX",
                    MB_ICONERROR);
        return 1;
    }
    msxsession_default_opts(g_session, &opts);
    if (msxsession_start(g_session, &opts) != 0)
        MessageBoxA(NULL, msxsession_last_error(g_session), "FujiNet Go MSX",
                    MB_ICONWARNING);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    /* Redraw the whole client area on any size change; the display is
     * stretched to fit, so a partial repaint leaves stale pixels. */
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "MsxMainWindow";
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassExA(&wc);

    g_menu = build_menu();
    g_hwnd = CreateWindowExA(0, "MsxMainWindow", "FujiNet Go MSX",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             800, 640, NULL, g_menu, inst, NULL);
    if (!g_hwnd)
        return 1;
    ShowWindow(g_hwnd, show);

    g_present_run = 1;
    pthread_create(&g_present_thread, NULL, present_main, NULL);

    /* Developer affordance: open the debugger alongside the main window,
     * which is what you want when the app dies before you can reach a
     * menu. Matches the other frontends' MSX_OPEN_DEBUGGER convention. */
    if (getenv("MSX_OPEN_DEBUGGER"))
        open_debugger();

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        /* The debugger's F5/F7/F8/Shift+F8 accelerators get first refusal
         * while its window (or one of its fields) has the focus. */
        if (msx_debugger_pretranslate(&msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    g_present_run = 0;
    pthread_join(g_present_thread, NULL);
    msxsession_free(g_session);
    DeleteCriticalSection(&g_fb_lock);
    return (int)msg.wParam;
}
