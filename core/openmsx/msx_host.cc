/*
 * msx_host -- see msx_host.hh.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "msx_host.hh"

#include "compat.h"

#include "CommandController.hh"
#include "CommandLineParser.hh"
#include "Display.hh"
#include "Event.hh"
#include "EventDistributor.hh"
#include "FrameSource.hh"
#include "GlobalCommandController.hh"
#include "MSXException.hh"
#include "Reactor.hh"
#include "RenderSettings.hh"
#include "Thread.hh"

#include <SDL.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

// --- frame sink --------------------------------------------------------
MsxFrameSink g_frame_sink = nullptr;
void* g_frame_user = nullptr;

// Fixed regardless of the MSX's actual video mode -- see msx_host.hh.
constexpr int kFrameW = 640;
constexpr int kFrameH = 480;

std::string g_machine = "C-BIOS_MSX2";

// --- boot handshake ------------------------------------------------------
// msxhost_core_start() blocks the caller until the openMSX thread has
// either finished booting or failed, so a bad machine id or a renderer
// failure is reported synchronously (unlike the Android host, which fires
// the thread and returns immediately -- there is no separate "is it up yet"
// poll to expose in a headless boot_smoke test or a session_start() that
// wants an honest return value, matching the family convention).
std::mutex g_boot_mutex;
std::condition_variable g_boot_cv;
int g_boot_status = 0; // 0 = pending, 1 = up, -1 = failed
std::string g_last_error;

std::thread g_omsx_thread;
std::atomic<bool> g_omsx_running{false};   // true between powerOn and run() returning
std::atomic<bool> g_omsx_thread_done{true};
openmsx::Reactor* g_reactor = nullptr;     // valid only on the openMSX thread

// Input injection is cross-thread: msxhost_inject_key /
// msxhost_set_joystick_button run on the caller's thread; they enqueue
// openMSX Event objects (POD SDL_Event wrappers, safe to build off-thread).
// drain_pending_events() forwards them on the openMSX thread (from the
// frame hook) into the *global* EventDistributor -> EventDelay -> MSX
// keyboard/joystick -- the same path desktop openMSX uses for real SDL
// input. Injecting straight into the per-motherboard MSXEventDistributor
// (bypassing EventDelay) is what made the Android app's held keys get read
// twice across screen transitions; see msx_host.hh's threading note.
std::mutex g_event_mutex;
std::vector<openmsx::Event> g_event_queue;

// Tcl commands (machine switching -- see msxhost_switch_machine), queued the
// same way and drained on the openMSX thread from the frame hook.
std::mutex g_command_mutex;
std::vector<std::string> g_command_queue;

void set_error(const std::string& msg) {
    std::fprintf(stderr, "msx_host: %s\n", msg.c_str());
    g_last_error = msg;
}

void report_boot_result(int status) {
    std::lock_guard<std::mutex> lk(g_boot_mutex);
    g_boot_status = status;
    g_boot_cv.notify_all();
}

void drain_pending_events() {
    if (!g_reactor) return;
    std::vector<openmsx::Event> pending;
    {
        std::lock_guard<std::mutex> lk(g_event_mutex);
        if (!g_event_queue.empty()) pending.swap(g_event_queue);
    }
    auto& dist = g_reactor->getEventDistributor();
    for (auto& e : pending) {
        dist.distributeEvent(std::move(e));
    }
}

void drain_pending_commands() {
    if (!g_reactor) return;
    std::vector<std::string> pending;
    {
        std::lock_guard<std::mutex> lk(g_command_mutex);
        if (g_command_queue.empty()) return;
        pending.swap(g_command_queue);
    }
    auto& cc = g_reactor->getCommandController();
    for (auto& cmd : pending) {
        try {
            cc.executeCommand(cmd);
        } catch (openmsx::MSXException& e) {
            std::fprintf(stderr, "msx_host: command \"%s\" failed: %s\n",
                        cmd.c_str(), e.getMessage().c_str());
        } catch (...) {
            std::fprintf(stderr, "msx_host: command \"%s\" failed\n", cmd.c_str());
        }
    }
}

void openmsx_thread_main() {
    using namespace openmsx;
    msx_thread_setname("msx-openmsx");

    // openMSX renders via GLES2/GL into SDL's "offscreen" headless EGL
    // pbuffer -- SDLGL-PP is the only real renderer RendererFactory offers
    // (DummyRenderer produces no frames), and nothing here ever draws to a
    // visible window: every frontend paints its own widget from the frame
    // this host publishes via msxhost_notify_frame(), called from the
    // patched PostProcessor::rotateFrames() below.
    SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "offscreen", SDL_HINT_OVERRIDE);
    // openMSX's own main.cc normally has SDL_main call SDL_SetMainReady; we
    // boot the Reactor directly, so SDL_InitSubSystem would otherwise refuse
    // to run ("Application didn't initialize properly").
    SDL_SetMainReady();

    // Heap-allocate the Reactor so a boot exception doesn't run its
    // destructor on a half-constructed object (crashes in ~Subject<Setting>)
    // and mask the real error. It is destructed only after a *clean* run()
    // exit (cleanRun below), which is what makes stop()/restart work.
    Reactor* reactor = nullptr;
    bool cleanRun = false;
    try {
        Thread::setMainThread();
        reactor = new Reactor();
        g_reactor = reactor;

        std::vector<std::string> argv = {
            "openmsx", "-machine", g_machine, "-ext", "FujiNet",
        };
        std::vector<char*> cargv;
        cargv.reserve(argv.size());
        for (auto& s : argv) cargv.push_back(s.data());

        CommandLineParser parser(*reactor);
        parser.parse(std::span<char*>(cargv.data(), cargv.size()));
        if (parser.getParseStatus() == CommandLineParser::Status::EXIT) {
            set_error("openMSX command line parser exited before boot "
                      "(bad machine id \"" + g_machine + "\"?)");
            report_boot_result(-1);
            g_reactor = nullptr;
            delete reactor;
            g_omsx_thread_done.store(true);
            return;
        }
        reactor->runStartupScripts(parser);

        // Initialise the renderer (mirrors openMSX's own main.cc). A
        // *failed* renderer init must NOT be followed by powerOn():
        // VDP::reset() dereferences a null renderer and crashes.
        try {
            auto& display = reactor->getDisplay();
            auto& renderSetting = display.getRenderSettings().getRendererSetting();
            if (renderSetting.getEnum() == RenderSettings::RendererID::UNINITIALIZED) {
                renderSetting.setValue(renderSetting.getDefaultValue());
                reactor->getEventDistributor().deliverEvents();
            }
        } catch (FatalError& e) {
            set_error(std::string("renderer init failed: ") + e.getMessage());
            report_boot_result(-1);
            g_reactor = nullptr;
            delete reactor;
            g_omsx_thread_done.store(true);
            return;
        }

        reactor->powerOn();
        g_omsx_running.store(true);

        // Bind the two MSX joystick ports to the synthetic "joy1"/"joy2" the
        // host injects (msxhost_set_joystick_button). See msx_host.hh's
        // MSXHOST_JOY_* enum. Each direction's value is a Tcl *list of
        // bindings*; a single binding ("joy1 -axis1") has a space, so it must
        // be its own list element -- hence the double braces {{...}}.
        try {
            auto& cc = reactor->getCommandController();
            // Pin to real-time: the FujiNet device holds the MSX in a
            // "loading" state while it streams, so with the default
            // fullspeedwhenloading=on openMSX would run the CPU flat-out,
            // racing emulated time ahead of wall-clock.
            cc.executeCommand("set throttle on");
            cc.executeCommand("set fullspeedwhenloading off");
            cc.executeCommand(
                "set msxjoystick1_config {UP {{joy1 -axis1}} DOWN {{joy1 +axis1}} "
                "LEFT {{joy1 -axis0}} RIGHT {{joy1 +axis0}} A {{joy1 button0}} B {{joy1 button1}}}");
            cc.executeCommand(
                "set msxjoystick2_config {UP {{joy2 -axis1}} DOWN {{joy2 +axis1}} "
                "LEFT {{joy2 -axis0}} RIGHT {{joy2 +axis0}} A {{joy2 button0}} B {{joy2 button1}}}");
        } catch (...) {
            // Never fatal to the boot.
        }

        report_boot_result(1);
        reactor->run();   // blocks until a QuitEvent (msxhost_core_stop)
        cleanRun = true;  // run() returned normally -> safe to destruct
    } catch (FatalError& e) {
        set_error(std::string("openMSX FatalError: ") + e.getMessage());
        report_boot_result(-1);
    } catch (MSXException& e) {
        set_error(std::string("openMSX boot failed: ") + e.getMessage());
        report_boot_result(-1);
    } catch (std::exception& e) {
        set_error(std::string("openMSX boot failed: ") + e.what());
        report_boot_result(-1);
    }
    g_omsx_running.store(false);
    g_reactor = nullptr; // no more inject/drain access past this point
    {   // drop any input queued but never dispatched, so it can't leak into a restart
        std::lock_guard<std::mutex> lk(g_event_mutex);
        g_event_queue.clear();
    }
    if (cleanRun && reactor) {
        delete reactor; // tears down SDL video for a fresh restart
    }
    g_omsx_thread_done.store(true);
}

}  // namespace

// Forward-declared here rather than pulled in via a header shared with the
// C session core: msx_thread_setname_shim keeps this translation unit
// independent of core/src/compat.h's C linkage (pthread_setname_np's
// signature differs by platform; compat.h already solves that once for the
// whole project).
extern "C" void msx_thread_setname_shim_impl(const char* name);
namespace { inline void msx_thread_setname_shim() { msx_thread_setname_shim_impl("msx-openmsx"); } }

// Called from the patched openmsx::PostProcessor::rotateFrames() (see
// tools/openmsx/patches/patch-staged-tree.py) once per finished frame, on
// the openMSX thread. Packs the CPU-side FrameSource into the fixed
// 640x480 XRGB8888 buffer the frame sink expects -- no GL readback.
extern "C" void msxhost_notify_frame(const openmsx::FrameSource* frame) {
    drain_pending_events();
    drain_pending_commands();
    if (!g_frame_sink || !frame) return;

    static thread_local std::vector<uint32_t> capture(
        static_cast<size_t>(kFrameW) * kFrameH);

    for (int y = 0; y < kFrameH; ++y) {
        openmsx::FrameSource::Pixel line_buf[kFrameW];
        auto line = frame->getLinePtr640_480(
            static_cast<unsigned>(y),
            std::span<openmsx::FrameSource::Pixel, kFrameW>{line_buf, kFrameW});
        uint32_t* dst = capture.data() + static_cast<size_t>(y) * kFrameW;
        for (int x = 0; x < kFrameW; ++x) {
            // openMSX's internal Pixel is fixed R=0xFF, G=0xFF00, B=0xFF0000
            // (PixelOperations.hh), i.e. memory order R,G,B,A on a
            // little-endian host -- repack to this family's XRGB8888
            // (0x00RRGGBB) convention.
            const uint32_t p = line[x];
            const uint32_t r = p & 0xFFu, g = (p >> 8) & 0xFFu, b = (p >> 16) & 0xFFu;
            dst[x] = (r << 16) | (g << 8) | b;
        }
    }
    g_frame_sink(capture.data(), kFrameW, kFrameH, g_frame_user);
}

// --- C API ----------------------------------------------------------------
extern "C" {

void msxhost_set_frame_sink(MsxFrameSink sink, void* user) {
    g_frame_sink = sink;
    g_frame_user = user;
}

void msxhost_select_machine(const char* machine_id) {
    if (machine_id && *machine_id) g_machine = machine_id;
}

int msxhost_core_start(void) {
    g_boot_status = 0;
    g_last_error.clear();
    g_omsx_thread_done.store(false);
    g_omsx_thread = std::thread(openmsx_thread_main);

    std::unique_lock<std::mutex> lk(g_boot_mutex);
    // Bounded so a wedged boot cannot hang the caller forever; 15s matches
    // the family's own start-handshake timeouts (e.g. cocosession_start).
    g_boot_cv.wait_for(lk, std::chrono::seconds(15),
                       [] { return g_boot_status != 0; });
    if (g_boot_status != 1) {
        if (g_last_error.empty()) set_error("openMSX did not come up in time");
        if (g_omsx_thread.joinable()) {
            // Give the thread a moment to finish unwinding, then detach
            // rather than block indefinitely -- a wedged boot must not hang
            // a caller that already waited 15s.
            lk.unlock();
            for (int i = 0; i < 100 && !g_omsx_thread_done.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (g_omsx_thread_done.load()) g_omsx_thread.join();
            else g_omsx_thread.detach();
        }
        return 0;
    }
    return 1;
}

void msxhost_core_stop(void) {
    if (g_reactor && g_omsx_running.load()) {
        g_reactor->getEventDistributor().distributeEvent(openmsx::QuitEvent());
    }
    if (g_omsx_thread.joinable()) {
        for (int i = 0; i < 300 && !g_omsx_thread_done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (g_omsx_thread_done.load()) g_omsx_thread.join();
        else g_omsx_thread.detach(); // wedged core; do not hang the caller
    }
    g_omsx_running.store(false);
}

void msxhost_core_reset(void) {
    if (!g_reactor || !g_omsx_running.load()) return;
    try {
        g_reactor->getCommandController().executeCommand("reset");
    } catch (...) {
    }
}

int msxhost_is_running(void) { return g_omsx_running.load() ? 1 : 0; }
const char* msxhost_last_error(void) { return g_last_error.c_str(); }

void msxhost_get_geometry(int* width, int* height) {
    if (width) *width = kFrameW;
    if (height) *height = kFrameH;
}

void msxhost_inject_key(int down, unsigned keycode, uint32_t character, uint16_t mods) {
    if (!g_omsx_running.load()) return;
    SDL_Event ev;
    ev.key = SDL_KeyboardEvent{};
    ev.key.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.timestamp = SDL_GetTicks();
    ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    ev.key.keysym.sym = static_cast<SDL_Keycode>(keycode);
    // A non-UNKNOWN scancode is mandatory: openMSX's
    // Keyboard::processQueuedEvent drops any key event whose scancode ==
    // SDL_SCANCODE_UNKNOWN (a Japanese-Kanji workaround) before it ever
    // reaches the matrix.
    ev.key.keysym.scancode = SDL_GetScancodeFromKey(static_cast<SDL_Keycode>(keycode));
    ev.key.keysym.mod = static_cast<Uint16>(mods);
    ev.key.keysym.unused = character; // unicode codepoint (0 for non-text keys)
    std::lock_guard<std::mutex> lk(g_event_mutex);
    if (down) g_event_queue.emplace_back(openmsx::KeyDownEvent(ev));
    else      g_event_queue.emplace_back(openmsx::KeyUpEvent(ev));
}

namespace {
void enqueue_joy_axis(int port, uint8_t axis, int16_t value) {
    SDL_Event ev;
    ev.jaxis = SDL_JoyAxisEvent{};
    ev.jaxis.type = SDL_JOYAXISMOTION;
    ev.jaxis.timestamp = SDL_GetTicks();
    ev.jaxis.which = static_cast<SDL_JoystickID>(port);
    ev.jaxis.axis = axis;
    ev.jaxis.value = value;
    std::lock_guard<std::mutex> lk(g_event_mutex);
    g_event_queue.emplace_back(openmsx::JoystickAxisMotionEvent(ev));
}

void enqueue_joy_button(int port, uint8_t button, bool pressed) {
    SDL_Event ev;
    ev.jbutton = SDL_JoyButtonEvent{};
    ev.jbutton.type = pressed ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
    ev.jbutton.timestamp = SDL_GetTicks();
    ev.jbutton.which = static_cast<SDL_JoystickID>(port);
    ev.jbutton.button = button;
    ev.jbutton.state = pressed ? SDL_PRESSED : SDL_RELEASED;
    std::lock_guard<std::mutex> lk(g_event_mutex);
    if (pressed) g_event_queue.emplace_back(openmsx::JoystickButtonDownEvent(ev));
    else         g_event_queue.emplace_back(openmsx::JoystickButtonUpEvent(ev));
}
}  // namespace

void msxhost_set_joystick_button(int port, int id, int pressed) {
    if (!g_omsx_running.load() || port < 0 || port >= 2) return;
    // The MSX joystick is digital; directions map to full-throw axis motion
    // (comfortably clearing the binding's dead zone), fire buttons to
    // buttons.
    constexpr int16_t kFull = 32767;
    switch (id) {
        case MSXHOST_JOY_LEFT:   enqueue_joy_axis(port, 0, pressed ? -kFull : 0); break;
        case MSXHOST_JOY_RIGHT:  enqueue_joy_axis(port, 0, pressed ?  kFull : 0); break;
        case MSXHOST_JOY_UP:     enqueue_joy_axis(port, 1, pressed ? -kFull : 0); break;
        case MSXHOST_JOY_DOWN:   enqueue_joy_axis(port, 1, pressed ?  kFull : 0); break;
        case MSXHOST_JOY_TRIG_A: enqueue_joy_button(port, 0, pressed != 0); break;
        case MSXHOST_JOY_TRIG_B: enqueue_joy_button(port, 1, pressed != 0); break;
        default: break;
    }
}

void msxhost_switch_machine(const char* machine_id) {
    if (!machine_id || !*machine_id) return;
    // Machine ids are openMSX machine config names (alphanumeric, '-', '_',
    // '+') -- reject anything containing Tcl-special characters rather than
    // quote them, since a malformed profile id has no legitimate reason to
    // need them.
    for (const char* p = machine_id; *p; ++p) {
        if (*p == ' ' || *p == '"' || *p == '{' || *p == '}' ||
            *p == '[' || *p == ']' || *p == ';' || *p == '\n') {
            std::fprintf(stderr,
                        "msx_host: rejecting machine id with unsafe "
                        "characters: %s\n", machine_id);
            return;
        }
    }
    std::lock_guard<std::mutex> lk(g_command_mutex);
    g_command_queue.emplace_back(std::string("machine ") + machine_id);
    // openMSX's own machine switch rebuilds the whole MSXMotherBoard, which
    // does not carry over the previous board's extensions -- re-insert
    // FujiNet the same way the initial boot does.
    g_command_queue.emplace_back("ext FujiNet");
}

}  // extern "C"
