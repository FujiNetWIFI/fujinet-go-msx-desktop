#!/usr/bin/env python3
"""Idempotent source patches applied to the staged openMSX tree.

Invoked by cmake/StageOpenMSX.cmake, never run directly. Each patch is
anchored to exact text and hard-fails (nonzero exit) when its anchor is
missing, so a pin that has drifted from what these patches expect is a loud
build error, not a silent no-op -- same discipline as
tools/xroar/patch-staged-tree.py in the sibling CoCo repo.

Five patches:

1. src/serial/FujiNet.cc: FUJINET_DEFAULT_PORT 1985 -> 65505. Upstream's
   default (and the Android sibling's 1986) would collide with other
   FujiNet Go targets or a standalone fujinet-pc-msx on the same machine.

2. src/video/PostProcessor.cc: a frame-sink hook in rotateFrames(). Desktop
   frame capture cannot use the Android app's
   -Wl,--wrap,SDL_GL_SwapWindow + glReadPixels mechanism (--wrap is a GNU
   ld/lld feature, absent on macOS's linker). rotateFrames() is openMSX's
   own per-frame CPU-side consumer of the finished FrameSource (the AVI
   recorder already taps it right below the insertion point), so this adds
   a forward declaration and a single call to an extern "C" hook that
   core/openmsx/msx_host.cc implements. All the scaling-to-320x240/640x480
   and XRGB8888 packing happens on our side, using the same
   FrameSource::getLinePtr*() API PostProcessor's own getScaledFrame()
   helper already uses -- nothing here duplicates that logic.

3. src/Reactor.cc: a debug-pump hook at the top of Reactor::run()'s loop
   (M6, the debugger). Confirmed by reading the loop before writing this:
   `eventDistributor->deliverEvents()` runs on *every* iteration whether
   the CPU is executing or blocked (MSXCPUInterface::doBreak() -- which
   the Tcl "debug break" command calls -- increments blockedCounter, and a
   blocked iteration just sleeps 20ms and loops back around instead of
   calling into the CPU). msx_host.cc's command queue (msxhost_
   switch_machine et al, and now the debugger's synchronous Tcl-command
   RPC) was, until this patch, only drained from the frame hook above --
   which stops firing the instant the CPU is broken, since no more frames
   render. Without this hook every debugger control (resume, step, read a
   register) would hang forever the moment a breakpoint was hit. Calling
   the same drain functions here too, unconditionally, guarantees they run
   at least once every ~20ms regardless of run/broken state; the frame
   hook still covers the (much higher-frequency) normal-running case
   unchanged.

4. src/video/VisibleSurface.cc: SDL_WINDOW_HIDDEN added to the window
   creation flags. VisibleSurface is openMSX's own native window -- never
   meant to be shown on any platform here, since every frontend paints its
   own widget from the frame msx_host.cc publishes via the rotateFrames()
   hook above, not from this window. On Linux this window is largely moot
   (msx_host.cc forces SDL's headless EGL "offscreen" driver there, which
   creates no real window at all), but macOS and Windows have no EGL/
   offscreen-driver equivalent, so openMSX's own SDLVideoSystem there
   always creates a REAL window -- confirmed by a real macOS CI run
   surfacing exactly the gap the original per-platform capture plan
   anticipated (see msx_host.cc's own comment) but that was never actually
   implemented: without this patch that real window would be visible.

5. src/events/InputEventGenerator.cc: InputEventGenerator::poll() -- called
   every Reactor::run() loop iteration via EventDistributor::deliverEvents()
   -- made a no-op. This project's own host never relies on SDL's native
   event queue for anything: keyboard/joystick input is injected straight
   into EventDistributor by msx_host.cc's own drain_pending_events()
   (msxhost_inject_key et al.), and the openMSX-native window is either
   hidden (patch 4, macOS/Windows) or does not exist at all (Linux's
   "offscreen" driver), so there are never any real SDL window events
   worth polling for either. poll()'s SDL_PollEvent call was, in this
   project's design, always pure overhead -- except on macOS, where it is
   actively fatal: SDL_PollEvent pumps the platform event queue when it is
   empty, and Cocoa's pump (Cocoa_PumpEventsUntilDate) hard-requires the
   main thread for its entire lifetime, not just at startup -- and
   Reactor::run() (and so this call) executes on this project's own
   dedicated openMSX thread on every platform, confirmed by a real macOS
   CI run crashing with "'nextEventMatchingMask should only be called
   from the Main Thread!'" once the two earlier main-thread bugs (see
   msx_host.cc's own comments) were already fixed and stopped masking
   this one. Skipping the call entirely sidesteps the conflict at its
   root rather than trying to relocate when it runs -- unlike patches 1-4,
   there is no known legitimate purpose for this call in this project's
   architecture, on any platform.

Deliberately NOT ported: the Android sibling's touch-keyboard release-delay
patch to src/input/EventDelay.{cc,hh}. A physical keyboard produces real
down/up pairs with real dwell time; that patch exists only to compensate for
an on-screen keyboard's synthetic back-to-back press+release.
"""
import sys


def fail(msg: str) -> None:
    print(f"patch-staged-tree.py: {msg}", file=sys.stderr)
    sys.exit(1)


def patch_fujinet_port(stage_dir: str) -> None:
    path = f"{stage_dir}/src/serial/FujiNet.cc"
    old = "#define FUJINET_DEFAULT_PORT     1985"
    new = ("// FujiNet Go MSX (desktop): 65505, this family's dedicated high\n"
           "// port for MSX (ADAM 65214, Apple II 64001, CoCo 64002/65504),\n"
           "// so a standalone fujinet-pc-msx or another FujiNet Go target\n"
           "// running on the same machine never collides with this one.\n"
           "#define FUJINET_DEFAULT_PORT     65505")
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if new.splitlines()[-1] in text:
        return  # already patched
    if old not in text:
        fail(f"{path}: FUJINET_DEFAULT_PORT anchor not found "
             "(openMSX source has drifted from the pinned commit?)")
    text = text.replace(old, new, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"Patched {path}: FUJINET_DEFAULT_PORT 1985 -> 65505")


def patch_frame_hook(stage_dir: str) -> None:
    path = f"{stage_dir}/src/video/PostProcessor.cc"
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    marker = "msxhost_notify_frame"
    if marker in text:
        return  # already patched

    # 1. Forward-declare the hook once, right after the includes. Anchor on
    #    the start of the anonymous/openmsx namespace block that follows
    #    PostProcessor's own #include list -- namespace openmsx { is unique
    #    enough combined with requiring the file not already have our marker.
    decl_anchor = "namespace openmsx {\n"
    decl = (
        "namespace openmsx {\n\n"
        "// FujiNet Go MSX (desktop) frame-sink hook -- see\n"
        "// tools/openmsx/patches/patch-staged-tree.py and\n"
        "// core/openmsx/msx_host.cc. Called once per finished frame from\n"
        "// PostProcessor::rotateFrames(), below. No GL readback: msx_host.cc\n"
        "// packs XRGB8888 straight off the CPU-side FrameSource.\n"
        "class FrameSource;\n"
    )
    if decl_anchor not in text:
        fail(f"{path}: 'namespace openmsx {{' anchor not found for the "
             "frame-hook forward declaration")
    text = text.replace(decl_anchor, decl, 1)

    # extern "C" the hook itself, right after the namespace opens (outside
    # the openmsx namespace would need closing it early, so declare inside
    # with extern "C" linkage -- valid C++, and matches how the FujiNet
    # entry wrapper declares its own extern "C" API from inside a namespace
    # elsewhere in this codebase).
    text = text.replace(
        decl,
        decl + '\nextern "C" void msxhost_notify_frame(const FrameSource* frame);\n',
        1)

    # 2. The call site: right after paintFrame is finalised in
    #    rotateFrames(), before openMSX's own AVI-recorder tap (which is the
    #    proof this is the right place -- same frame, same lifetime).
    call_anchor = (
        "\t// Possibly record this frame\n"
        "\tif (recorder && needRecord()) {\n")
    call = (
        "\t// FujiNet Go MSX (desktop): hand the finished, mode-selected frame\n"
        "\t// to the desktop host. See patch-staged-tree.py.\n"
        "\tmsxhost_notify_frame(paintFrame);\n\n"
        "\t// Possibly record this frame\n"
        "\tif (recorder && needRecord()) {\n")
    if call_anchor not in text:
        fail(f"{path}: rotateFrames() recorder-tap anchor not found "
             "(openMSX source has drifted from the pinned commit?)")
    text = text.replace(call_anchor, call, 1)

    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"Patched {path}: msxhost_notify_frame() hook in rotateFrames()")


def patch_debug_pump_hook(stage_dir: str) -> None:
    path = f"{stage_dir}/src/Reactor.cc"
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    marker = "msxhost_debug_pump"
    if marker in text:
        return  # already patched

    decl_anchor = "namespace openmsx {\n"
    decl = (
        "namespace openmsx {\n\n"
        "// FujiNet Go MSX (desktop) debugger command-queue pump -- see\n"
        "// tools/openmsx/patches/patch-staged-tree.py and\n"
        "// core/openmsx/msx_host.cc. Called at the top of every\n"
        "// Reactor::run() loop iteration, whether the CPU is executing or\n"
        "// blocked (a debugger break), so msx_host.cc's cross-thread\n"
        "// command queue keeps draining even while broken -- the frame\n"
        "// hook in PostProcessor.cc alone cannot, since no more frames\n"
        "// render once the CPU stops.\n"
        'extern "C" void msxhost_debug_pump(void);\n')
    if decl_anchor not in text:
        fail(f"{path}: 'namespace openmsx {{' anchor not found for the "
             "debug-pump forward declaration")
    text = text.replace(decl_anchor, decl, 1)

    call_anchor = (
        "void Reactor::run()\n"
        "{\n"
        "\twhile (running) {\n"
        "\t\teventDistributor->deliverEvents();\n")
    call = (
        "void Reactor::run()\n"
        "{\n"
        "\twhile (running) {\n"
        "\t\t// FujiNet Go MSX (desktop): see patch-staged-tree.py.\n"
        "\t\tmsxhost_debug_pump();\n"
        "\t\teventDistributor->deliverEvents();\n")
    if call_anchor not in text:
        fail(f"{path}: Reactor::run() loop anchor not found "
             "(openMSX source has drifted from the pinned commit?)")
    text = text.replace(call_anchor, call, 1)

    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"Patched {path}: msxhost_debug_pump() hook in Reactor::run()")


def patch_hidden_window(stage_dir: str) -> None:
    path = f"{stage_dir}/src/video/VisibleSurface.cc"
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    old = "\tint flags = SDL_WINDOW_OPENGL;\n"
    new = (
        "\t// FujiNet Go MSX (desktop): never show openMSX's own native\n"
        "\t// window -- see patch-staged-tree.py.\n"
        "\tint flags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN;\n")
    if new.splitlines()[-1] in text:
        return  # already patched
    if old not in text:
        fail(f"{path}: VisibleSurface window-flags anchor not found "
             "(openMSX source has drifted from the pinned commit?)")
    text = text.replace(old, new, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"Patched {path}: SDL_WINDOW_HIDDEN added to window creation flags")


def patch_disable_input_poll(stage_dir: str) -> None:
    path = f"{stage_dir}/src/events/InputEventGenerator.cc"
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    marker = "FujiNet Go MSX (desktop): no-op"
    if marker in text:
        return  # already patched
    old = "void InputEventGenerator::poll()\n{\n"
    new = (
        "void InputEventGenerator::poll()\n"
        "{\n"
        f"\t// {marker} -- see patch-staged-tree.py.\n"
        "\t// This project's own host never relies on SDL's native event\n"
        "\t// queue (input is injected straight into EventDistributor by\n"
        "\t// msx_host.cc; the openMSX-native window is hidden or does not\n"
        "\t// exist at all), and on macOS the SDL_PollEvent call below is\n"
        "\t// actively fatal from this thread (Cocoa's event pump hard-\n"
        "\t// requires the main thread; Reactor::run(), which reaches this\n"
        "\t// via EventDistributor::deliverEvents(), always runs on this\n"
        "\t// project's own dedicated openMSX thread instead).\n"
        "\treturn;\n")
    if old not in text:
        fail(f"{path}: InputEventGenerator::poll() anchor not found "
             "(openMSX source has drifted from the pinned commit?)")
    text = text.replace(old, new, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"Patched {path}: InputEventGenerator::poll() made a no-op")


def main() -> None:
    if len(sys.argv) != 2:
        fail("usage: patch-staged-tree.py <staged-openmsx-dir>")
    stage_dir = sys.argv[1]
    patch_fujinet_port(stage_dir)
    patch_frame_hook(stage_dir)
    patch_debug_pump_hook(stage_dir)
    patch_hidden_window(stage_dir)
    patch_disable_input_poll(stage_dir)


if __name__ == "__main__":
    main()
