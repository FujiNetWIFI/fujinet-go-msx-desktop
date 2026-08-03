#!/usr/bin/env python3
"""Idempotent source patches applied to the staged openMSX tree.

Invoked by cmake/StageOpenMSX.cmake, never run directly. Each patch is
anchored to exact text and hard-fails (nonzero exit) when its anchor is
missing, so a pin that has drifted from what these patches expect is a loud
build error, not a silent no-op -- same discipline as
tools/xroar/patch-staged-tree.py in the sibling CoCo repo.

Thirteen patches:

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

6. src/video/VisibleSurface.cc (macOS only): every NSWindow-mutating call
   -- construction (createSurface() + SDL_GL_CreateContext()),
   updateWindowTitle(), resize() and setFullScreen() -- dispatched onto the
   main thread through one shared helper, msxRunOnMainThread(). This class
   runs entirely on this project's own dedicated openMSX thread like
   everything else in Reactor, and unlike patches 4-5 (hiding the window,
   skipping SDL's event poll) there is no way to avoid touching AppKit
   here at all -- a window has to actually be created for openMSX to
   render into, even a hidden one, and its title changes on every machine
   switch. Found incrementally across two real macOS CI runs, each
   surfacing the next AppKit main-thread requirement once the previous fix
   stopped masking it: first "renderer init failed: Could not create
   window: NSWindow should only be instantiated on the main thread!"
   (construction), then -- with construction already fixed -- "NSWindow
   geometry should only be modified on the main thread!" from
   updateWindowTitle(), triggered by a live machine switch that changes
   the window title without ever recreating the window. resize() (called
   on every video-mode change, so this runs constantly during ordinary
   MSX operation, not just once) and setFullScreen() (never actually
   reachable from this project's own code, since no fullscreen setting is
   ever exposed or toggled) are wrapped the same way rather than waiting
   for a third and fourth real failure to prove each one necessary.

   msxRunOnMainThread checks pthread_main_np() before dispatching:
   createSurface() itself calls updateWindowTitle() as part of
   construction, and by the time that inner call happens the constructor
   has already dispatched onto the main thread -- a nested dispatch_sync
   from a block already running on the very queue it targets is a
   textbook deadlock, not a re-entrant no-op. A C++ exception thrown
   inside a dispatch_sync block runs on a genuinely different thread's
   stack even though the caller is synchronously blocked, so it cannot
   simply propagate -- std::exception_ptr captures it inside the block and
   rethrows it back on the caller's thread once dispatch_sync returns,
   preserving msx_host.cc's own catch (FatalError&) handling around the
   constructor call unchanged. Since SDL_GL_CreateContext implicitly makes
   the new context current on whichever thread creates it (the main
   thread, during construction), it is explicitly re-bound with
   SDL_GL_MakeCurrent() afterward -- every OpenGL call in the rest of the
   constructor, and every frame openMSX renders afterward, needs the
   context current on the openMSX thread instead.

   core/openmsx/msx_host.cc's own msxhost_core_start() is the other half
   of the construction case: without it, the main thread would be parked
   in a plain condition_variable wait during boot, which never services
   the main dispatch queue, and that dispatch_sync would block forever.

7. build/platform-darwin.mk (macOS only): -fblocks added to TARGET_FLAGS.
   Patch 6 above uses Clang's Blocks extension (the `^{ ... }` syntax
   dispatch_sync needs), which Apple Clang enables by default for Darwin
   targets in every language mode -- but this project has no Mac to
   confirm that default actually holds for this exact build's compiler
   invocation, and the cost of being wrong is a wasted CI round-trip for
   something a single explicit flag avoids entirely. Cheap insurance, not
   a response to an observed failure.

8. src/video/VisibleSurface.cc (macOS only): the destructor's window
   teardown dispatched onto the main thread too, the same way patch 6
   dispatches its construction. Every real macOS CI run of this project so
   far, once its own build/link errors were fixed, has ended with several
   tests crashing right after their own test logic visibly succeeded
   (boot_smoke printing real non-black rendered frame data;
   machine_switch_test printing a real "MSX2 booted" line) -- consistent
   with a crash during session teardown, which happens on every test's
   clean exit regardless of what the test itself checks. The destructor
   reads the window's position (getWindowPosition(), itself SDL_
   GetWindowFlags + SDL_GetWindowPosition) and calls SDL_GL_DeleteContext,
   then -- after the explicit destructor body finishes -- the `window`
   member (an SDLWindowPtr, i.e. a unique_ptr-alike with an SDL_
   DestroyWindow deleter) is torn down implicitly as part of ordinary
   member destruction, back on the openMSX thread the destructor itself
   runs on, same as everything else in this class. Explicitly
   window.reset()'d inside the same msxRunOnMainThread block as the rest
   of this teardown, so the implicit end-of-scope destruction that would
   otherwise happen off the main thread becomes a no-op on an
   already-null pointer. setWindowPosition() (the standalone setter,
   distinct from the destructor's own read of the getter) is wrapped the
   same way, for the same "never actually reachable from this project's
   own code, but cheap to cover" reasoning as setFullScreen() in patch 6 --
   this project never moves or otherwise manages the hidden window's
   position.

9. build/3rdparty.mk: pkg-config's bundled internal glib (2013-era code)
   declares a struct member and later accesses a union member both
   literally named `bool` (glib/goption.c) -- legal C when it was written,
   since bool was just an ordinary identifier unless a translation unit
   opted in via <stdbool.h>. A real Windows CI run failed here with "two
   or more data types in declaration specifiers" and "expected identifier
   before 'bool'" at exactly those two spots: the MinGW-w64 GCC this
   project's windows.yml pulls in defaults to -std=gnu23, where
   bool/true/false became real keywords, so `gboolean bool;` is parsed as
   two competing type-specifiers rather than a declaration.

   The fix renames the identifier directly with a `sed -i` step injected
   into the Makefile recipe, right after the source is extracted and
   before ./configure runs, rather than trying to influence the compiler's
   dialect via CFLAGS. Two earlier versions of this patch tried exactly
   that (first unconditionally, then gated to $(OPENMSX_TARGET_OS) =
   mingw-w64 via Make's $(if $(filter ...)) after the unconditional
   version broke Apple Clang's build of this same module with unrelated
   -Wint-conversion errors in glib/gatomic.c) -- both were abandoned once
   reading the real CI log showed pkg-config's own top-level ./configure
   does not forward CFLAGS to this bundled glib's own recursive
   ./configure at all (autoconf's own "configure: running .../glib/
   configure ... 'CC=x86_64-w64-mingw32-gcc'" announcement explicitly
   shows CC being forwarded and CFLAGS conspicuously absent), so neither
   CFLAGS-based attempt could ever have worked regardless of gating.
   Renaming the identifier sidesteps the whole question of which flags
   reach this compile: it needs no platform gate at all, since the rename
   is semantically identical on every compiler/dialect and nothing else
   in this bundled module depends on the field's exact name.

   The -Wint-conversion/gatomic.c failure that motivated gating the second
   attempt turned out to be a red herring for THIS patch specifically, and
   not a deterministic regression at all: re-running the already-green
   macOS build from an earlier, completely unrelated commit (no -std=
   change of any kind) reproduced the identical gatomic.c failure on one
   attempt, then SUCCEEDED cleanly on an immediate retry of the exact same
   unmodified commit -- ruling out both "caused by this patch" and "a
   permanent environment/toolchain shift", and pointing instead at
   nondeterministic flakiness across GitHub's macOS runner fleet (plausibly
   heterogeneous Xcode/Clang images across individual runner instances at
   the moment, though that specific mechanism isn't confirmed further).
   Since it reproduces on code this patch never touches, it's tracked and
   worked separately from this patch; see TODO for current status.

Deliberately NOT ported: the Android sibling's touch-keyboard release-delay
patch to src/input/EventDelay.{cc,hh}. A physical keyboard produces real
down/up pairs with real dwell time; that patch exists only to compensate for
an on-screen keyboard's synthetic back-to-back press+release.

10. build/3rdparty.mk: PKG_CONFIG_PATH exported alongside the existing
    PKG_CONFIG export, pointing at this same 3rdparty chain's own
    $(INSTALL_DIR)/lib/pkgconfig. A real Windows CI run got past all four
    pkg-config/glib bugs above (patch 9) and progressed into SDL2_ttf's
    own ./configure, which failed there with "*** Unable to find
    FreeType2 library" despite freetype2.pc being right there in that
    exact install/lib/pkgconfig directory (confirmed in the same log:
    freetype's own install step puts it there a few thousand lines
    earlier) -- because nothing ever told the freshly cross-built
    pkg-config binary to look in it. "checking for sdl2 >= 2.0.10... no"
    (immediately above, same log) shows this isn't freetype-specific:
    pkg-config-based detection was silently failing for every 3rdparty
    package this whole time, just papered over for SDL2 because SDL2 also
    installs a working sdl2-config fallback script that autoconf falls
    back to -- freetype apparently doesn't install an equivalent
    freetype-config for this freetype version/configuration, so SDL2_ttf
    had no fallback left and the configure hard-failed.

    Exported unconditionally (not gated to any platform): this is a
    strict addition to pkg-config's search path, so it can only let
    pkg-config find MORE .pc files it previously couldn't -- it cannot
    break a lookup that was already succeeding some other way (a
    fallback script, or a check that didn't need pkg-config at all), on
    any platform. macOS's own 3rdparty chain builds successfully without
    this export today, most likely because its freetype build happens to
    produce a working freetype-config there too -- but there's no
    downside to fixing the actual gap (a missing PKG_CONFIG_PATH,
    normally implied/inherited from a real pkg-config installation but
    absent here since this cross-built pkg-config binary has no system
    install to inherit it from) globally rather than leaving it to keep
    silently degrading to fragile *-config-script fallbacks everywhere
    it happens to still work.

11. build/libraries.py: _get_pkg_config()'s cross-pkg-config filename match
    widened to also accept a .exe suffix. A real Windows CI run got past
    every bug above -- the 3rdparty chain now finishes completely, and
    both "checking for sdl2 >= 2.0.10... yes" and "checking for freetype2
    >= 7.0.1... yes" confirm patch 10 above actually works -- and
    progressed into an entirely different phase of openMSX's own build:
    the main (non-3rdparty) build's own library-probing step
    (build/probe.py), which crashed with "RuntimeError: No cross-pkg-config
    found in 3rdparty build" raised by _get_pkg_config() in this file.

    That function lists $(3rdparty install dir)/../tools/bin (where
    patch 9's own analysis already established pkg-config gets installed)
    and looks for a filename ending in '-pkg-config' -- but the real
    installed file on Windows is 'x86_64-w64-mingw32-pkg-config.exe', not
    the extension-less name this check assumes. Confirmed directly from
    pkg-config's own upstream build files (the same pinned
    pkg-config-0.29.2.tar.gz patch 9 already downloads to verify its own
    fix): Makefile.am defines `host_tool = $(host)-pkg-config$(EXEEXT)`,
    and $(EXEEXT) is '.exe' for a mingw32 target -- not inferred, read
    directly from the real source. This didn't break patch 9's own fix
    (PKG_CONFIG_PATH lets shell commands invoke the binary by its
    extension-less name -- Windows' CreateProcess/PATHEXT resolves that to
    the real .exe automatically) because that's ordinary process
    execution, which auto-appends .exe when the bare name doesn't
    literally exist; this function instead does a plain directory listing
    and textual filename comparison, which has no such fallback.

    Widened to `name.endswith('-pkg-config') or name.endswith(
    '-pkg-config.exe')` rather than stripping a trailing extension before
    comparing, so the returned path is still the exact real filename on
    disk either way (no behavior change on Linux/macOS, where the
    extension-less form is already correct).

12. build/libraries.py: FreeType.getVersion() uses --modversion instead of
    --ftversion when its own getConfigScript() fell back to pkg-config.
    A real Windows CI run's probe.log (only visible after this same
    session added a diagnostic that prints it -- see build-openmsx-
    desktop.sh) showed FREETYPE: Found header / Found lib (patches 9-11
    above all worked) immediately followed by:
    `Error executing ".../pkg-config.exe freetype2 --ftversion"` /
    `Unknown option --ftversion`. --ftversion is a real freetype-config
    flag, but this bundled freetype (like patch 9's docstring already
    established, quoting this exact file's own comment) "no longer
    installs the freetype-config script by default", so
    getConfigScript() always falls back to invoking pkg-config directly
    here -- and pkg-config's own version flag is --modversion, not
    --ftversion. The base Library.getVersion() already handles exactly
    this distinction (`elif 'pkg-config' in configScript: return
    '`%s --modversion`' ...`); FreeType's own override just never
    replicated that same check when it added its own getConfigScript()
    fallback. Cosmetic only (a missing version string, not a detection
    failure -- header/lib were already found), but a real, verifiable
    bug all the same.

13. build/msysutils.py: a Python-2-only print STATEMENT
    ("print sys.argv[1]", no parentheses) fixed to a Python 3 print CALL
    ("print(sys.argv[1])"). This is a real, pre-existing bug in openMSX's
    own build tooling that patch 12's OSTYPE=msys export (build-openmsx-
    desktop.sh) immediately exposed: setting OSTYPE=msys makes
    msysActive() return true, which -- correctly, as intended -- makes
    build/executils.py route *-config script invocations through `sh -c`
    (patch 12's whole point). But msysActive() being true ALSO triggers
    an entirely different, previously-never-exercised code path: this
    module's own top-level `if msysActive(): msysMounts =
    _determineMounts()`, which shells out to the SAME python3 interpreter
    with a hardcoded, never-updated-for-Python-3 command string. A real
    Windows CI run confirmed exactly this: "Error determining MSYS root:
    ... SyntaxError: Missing parentheses in call to 'print'. Did you mean
    print(...)?", immediately after patch 12 first went in -- this whole
    module has evidently never actually run under a real MSYS2 GCC 16
    Python 3 environment before, in this project or upstream, since
    nothing here previously set OSTYPE/MSYSCON at all. `print(x)` is
    valid, identical syntax under both Python 2 and 3 for a single
    argument, so this fix carries no risk of breaking whatever Python 2
    compatibility this file's vintage was originally written for.

    Second bug in the same never-exercised function, found in the same
    review rather than via a second CI round-trip: _determineMounts()'s
    `msysRoot = stdoutdata.strip()` keeps stdoutdata as bytes (Python 3's
    subprocess.communicate() default, no text=True given), then
    concatenates it with str literals twice further down (`msysRoot +
    '/etc/fstab'`, `msysRoot + '/'`) -- TypeError: can't concat str to
    bytes, confirmed by a real Windows CI run immediately after the print
    fix alone went in. Decoding once, where the bytes value is first
    captured (`stdoutdata.decode('utf-8').strip()`), fixes both
    downstream sites in one place. build/executils.py's own
    captureStdout() already does this correctly (`stdoutdata.decode(
    'utf-8')`) for its own subprocess output -- this function alone
    missed it, the same "never run under Python 3 before" story as the
    print statement.
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


def patch_macos_main_thread_window(stage_dir: str) -> None:
    path = f"{stage_dir}/src/video/VisibleSurface.cc"
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    marker = "FujiNet Go MSX (desktop): msxRunOnMainThread"
    if marker in text:
        return  # already patched

    # ---- 1. includes + the shared helper -----------------------------------
    # A real macOS CI run confirmed window *creation* needing the main
    # thread was only the first of several: a later run (after that fix
    # landed) crashed identically but from updateWindowTitle() instead
    # ("NSWindow geometry should only be modified on the main thread!",
    # during a live machine switch, which changes the window title but
    # never recreates the window) -- confirming AppKit gates essentially
    # every NSWindow mutation this way, not just construction. One shared
    # helper, used at every call site that touches the window's visible
    # state, avoids finding the rest of these one real CI failure at a
    # time. It checks pthread_main_np() first rather than dispatch_sync
    # unconditionally: createSurface() itself calls updateWindowTitle()
    # (line ~266, original source) as part of construction, and by the
    # time that inner call happens this project's own constructor patch
    # below has *already* dispatched onto the main thread -- a nested
    # dispatch_sync from a block already running on the very queue it
    # targets is a textbook deadlock, not a re-entrant no-op.
    include_anchor = "#include <ranges>\n\nnamespace openmsx {\n"
    include = (
        "#include <ranges>\n"
        "#ifdef __APPLE__\n"
        "#include <dispatch/dispatch.h>\n"
        "#include <exception>\n"
        "#include <pthread.h>\n"
        "#endif\n\n"
        "namespace openmsx {\n"
        "\n"
        "#ifdef __APPLE__\n"
        "namespace {\n"
        f"// {marker}(fn): runs fn() synchronously on the main thread (or\n"
        "// inline, if already there) and propagates any C++ exception it\n"
        "// throws back to the caller -- see patch-staged-tree.py. AppKit\n"
        "// requires NSWindow creation and every later mutation of its\n"
        "// visible state (title, size, fullscreen, ...) to happen on the\n"
        "// main thread; VisibleSurface's constructor and every method\n"
        "// below that touches the window run on this project's own\n"
        "// dedicated openMSX thread instead (msx_host.cc), never the main\n"
        "// one. A C++ exception thrown inside a dispatch_sync block runs\n"
        "// on a genuinely different thread's stack even though the\n"
        "// caller is synchronously blocked, so it cannot simply\n"
        "// propagate -- std::exception_ptr captures it inside the block\n"
        "// and rethrows it back on the caller's thread once dispatch_sync\n"
        "// returns.\n"
        "template<typename F>\n"
        "void msxRunOnMainThread(F&& fn)\n"
        "{\n"
        "\tif (pthread_main_np()) {\n"
        "\t\tfn();\n"
        "\t\treturn;\n"
        "\t}\n"
        "\t__block std::exception_ptr pendingException;\n"
        "\tdispatch_sync(dispatch_get_main_queue(), ^{\n"
        "\t\ttry {\n"
        "\t\t\tfn();\n"
        "\t\t} catch (...) {\n"
        "\t\t\tpendingException = std::current_exception();\n"
        "\t\t}\n"
        "\t});\n"
        "\tif (pendingException) {\n"
        "\t\tstd::rethrow_exception(pendingException);\n"
        "\t}\n"
        "}\n"
        "} // namespace\n"
        "#endif\n")
    if include_anchor not in text:
        fail(f"{path}: '#include <ranges>' / 'namespace openmsx {{' anchor "
             "not found for the msxRunOnMainThread helper")
    text = text.replace(include_anchor, include, 1)

    # ---- 2. constructor: window + GL-context creation -----------------------
    old_ctor = (
        "\tcreateSurface(size, flags);\n"
        "\tWindowEvent::setMainWindowId(SDL_GetWindowID(window.get()));\n"
        "\n"
        "\tglContext = SDL_GL_CreateContext(window.get());\n"
        "\tif (!glContext) {\n"
        "\t\tthrow InitException(\n"
        '\t\t\t"Failed to create " VERSION_STRING " context: ", SDL_GetError());\n'
        "\t}\n")
    new_ctor = (
        "#ifdef __APPLE__\n"
        "\t// See the msxRunOnMainThread comment above.\n"
        "\tmsxRunOnMainThread([&] {\n"
        "#endif\n"
        "\t\tcreateSurface(size, flags);\n"
        "\t\tWindowEvent::setMainWindowId(SDL_GetWindowID(window.get()));\n"
        "\n"
        "\t\tglContext = SDL_GL_CreateContext(window.get());\n"
        "\t\tif (!glContext) {\n"
        "\t\t\tthrow InitException(\n"
        '\t\t\t\t"Failed to create " VERSION_STRING " context: ", SDL_GetError());\n'
        "\t\t}\n"
        "#ifdef __APPLE__\n"
        "\t});\n"
        "\t// SDL_GL_CreateContext implicitly makes the new context current\n"
        "\t// on whichever thread creates it (the main thread, above) --\n"
        "\t// every OpenGL call below this point (and every frame openMSX\n"
        "\t// renders afterward) needs the context current on THIS thread\n"
        "\t// instead.\n"
        "\tif (SDL_GL_MakeCurrent(window.get(), glContext) != 0) {\n"
        "\t\tthrow InitException(\n"
        '\t\t\t"Failed to make GL context current: ", SDL_GetError());\n'
        "\t}\n"
        "#endif\n")
    if old_ctor not in text:
        fail(f"{path}: VisibleSurface window/GL-context creation anchor "
             "not found (openMSX source has drifted from the pinned commit?)")
    text = text.replace(old_ctor, new_ctor, 1)

    # ---- 3. updateWindowTitle(): a live machine switch calls this without ---
    # ---- ever recreating the window, confirmed fatal by a real CI run ------
    old_title = (
        "void VisibleSurface::updateWindowTitle()\n"
        "{\n"
        "\tassert(window);\n"
        "\tSDL_SetWindowTitle(window.get(), getDisplay().getWindowTitle().c_str());\n"
        "}\n")
    new_title = (
        "void VisibleSurface::updateWindowTitle()\n"
        "{\n"
        "\tassert(window);\n"
        "#ifdef __APPLE__\n"
        "\tmsxRunOnMainThread([&] {\n"
        "#endif\n"
        "\t\tSDL_SetWindowTitle(window.get(), getDisplay().getWindowTitle().c_str());\n"
        "#ifdef __APPLE__\n"
        "\t});\n"
        "#endif\n"
        "}\n")
    if old_title not in text:
        fail(f"{path}: VisibleSurface::updateWindowTitle() anchor not found "
             "(openMSX source has drifted from the pinned commit?)")
    text = text.replace(old_title, new_title, 1)

    # ---- 4. resize(): the window is resized on every video-mode change, so --
    # ---- this runs constantly during ordinary MSX operation, not just once -
    old_resize = (
        "void VisibleSurface::resize()\n"
        "{\n"
        "\tauto size = display.getWindowSize();\n"
        "\tSDL_SetWindowSize(window.get(), size.x, size.y);\n"
        "\n"
        "\tbool fullScreen = display.getRenderSettings().getFullScreen();\n"
        "\tsetViewPort(size, fullScreen);\n"
        "}\n")
    new_resize = (
        "void VisibleSurface::resize()\n"
        "{\n"
        "\tauto size = display.getWindowSize();\n"
        "#ifdef __APPLE__\n"
        "\tmsxRunOnMainThread([&] {\n"
        "#endif\n"
        "\t\tSDL_SetWindowSize(window.get(), size.x, size.y);\n"
        "#ifdef __APPLE__\n"
        "\t});\n"
        "#endif\n"
        "\n"
        "\tbool fullScreen = display.getRenderSettings().getFullScreen();\n"
        "\tsetViewPort(size, fullScreen);\n"
        "}\n")
    if old_resize not in text:
        fail(f"{path}: VisibleSurface::resize() anchor not found "
             "(openMSX source has drifted from the pinned commit?)")
    text = text.replace(old_resize, new_resize, 1)

    # ---- 5. setFullScreen(): never actually reachable from this project's ---
    # ---- own code (no fullscreen setting is ever exposed or toggled), but --
    # ---- wrapped anyway for the same reason as the rest of this patch ------
    old_fs = (
        "bool VisibleSurface::setFullScreen(bool fullscreen)\n"
        "{\n"
        "\tauto flags = SDL_GetWindowFlags(window.get());\n"
        "\t// Note: SDL_WINDOW_FULLSCREEN_DESKTOP also has the SDL_WINDOW_FULLSCREEN\n"
        "\t//       bit set.\n"
        "\tif (bool currentState = (flags & SDL_WINDOW_FULLSCREEN) != 0;\n"
        "\t    currentState == fullscreen) {\n"
        "\t\t// already wanted stated\n"
        "\t\treturn true;\n"
        "\t}\n"
        "\n"
        "\tif (SDL_SetWindowFullscreen(window.get(),\n"
        "\t\t\tfullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0) {\n"
        "\t\treturn false; // error, try re-creating the window\n"
        "\t}\n"
        "\tfullScreenUpdated(fullscreen);\n"
        "\treturn true; // success\n"
        "}\n")
    new_fs = (
        "bool VisibleSurface::setFullScreen(bool fullscreen)\n"
        "{\n"
        "\tbool alreadySet = false;\n"
        "\tbool setOk = true;\n"
        "#ifdef __APPLE__\n"
        "\tmsxRunOnMainThread([&] {\n"
        "#endif\n"
        "\t\tauto flags = SDL_GetWindowFlags(window.get());\n"
        "\t\t// Note: SDL_WINDOW_FULLSCREEN_DESKTOP also has the SDL_WINDOW_FULLSCREEN\n"
        "\t\t//       bit set.\n"
        "\t\tif (bool currentState = (flags & SDL_WINDOW_FULLSCREEN) != 0;\n"
        "\t\t    currentState == fullscreen) {\n"
        "\t\t\talreadySet = true; // already wanted state\n"
        "\t\t} else if (SDL_SetWindowFullscreen(window.get(),\n"
        "\t\t\t\tfullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0) {\n"
        "\t\t\tsetOk = false; // error, try re-creating the window\n"
        "\t\t}\n"
        "#ifdef __APPLE__\n"
        "\t});\n"
        "#endif\n"
        "\tif (alreadySet) return true;\n"
        "\tif (!setOk) return false;\n"
        "\tfullScreenUpdated(fullscreen);\n"
        "\treturn true; // success\n"
        "}\n")
    if old_fs not in text:
        fail(f"{path}: VisibleSurface::setFullScreen() anchor not found "
             "(openMSX source has drifted from the pinned commit?)")
    text = text.replace(old_fs, new_fs, 1)

    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"Patched {path}: window creation, title, resize and fullscreen "
          "dispatched to the main thread on macOS")


def patch_macos_window_teardown(stage_dir: str) -> None:
    path = f"{stage_dir}/src/video/VisibleSurface.cc"
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    marker = "FujiNet Go MSX (desktop): destroy the window here"
    if marker in text:
        return  # already patched

    old_dtor = (
        "\tgl::context.reset();\n"
        "\tSDL_GL_DeleteContext(glContext);\n"
        "\n"
        "\t// store last known position for when we recreate it\n"
        "\t// the window gets recreated when changing renderers, for instance.\n"
        "\t// Do not store if we're full screen, the location is the top-left\n"
        "\tif (auto pos = getWindowPosition()) {\n"
        "\t\tdisplay.storeWindowPosition(*pos);\n"
        "\t}\n"
        "\n"
        "\tfor (auto type : {EventType::IMGUI_ACTIVE,\n")
    new_dtor = (
        "\tgl::context.reset();\n"
        "#ifdef __APPLE__\n"
        "\tmsxRunOnMainThread([&] {\n"
        "#endif\n"
        "\t\tSDL_GL_DeleteContext(glContext);\n"
        "\n"
        "\t\t// store last known position for when we recreate it\n"
        "\t\t// the window gets recreated when changing renderers, for instance.\n"
        "\t\t// Do not store if we're full screen, the location is the top-left\n"
        "\t\tif (auto pos = getWindowPosition()) {\n"
        "\t\t\tdisplay.storeWindowPosition(*pos);\n"
        "\t\t}\n"
        "#ifdef __APPLE__\n"
        f"\t\t// {marker} -- see patch-staged-tree.py. Without this, `window`\n"
        "\t\t// (an SDLWindowPtr -- a unique_ptr-alike with an SDL_DestroyWindow\n"
        "\t\t// deleter) is torn down implicitly as an ordinary member, once this\n"
        "\t\t// whole function returns -- back on the openMSX thread this\n"
        "\t\t// destructor itself runs on, same as everything else in this class.\n"
        "\t\twindow.reset();\n"
        "#endif\n"
        "#ifdef __APPLE__\n"
        "\t});\n"
        "#endif\n"
        "\n"
        "\tfor (auto type : {EventType::IMGUI_ACTIVE,\n")
    if old_dtor not in text:
        fail(f"{path}: VisibleSurface::~VisibleSurface() teardown anchor "
             "not found (openMSX source has drifted from the pinned commit?)")
    text = text.replace(old_dtor, new_dtor, 1)

    old_setpos = (
        "void VisibleSurface::setWindowPosition(gl::ivec2 pos)\n"
        "{\n"
        "\tif (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_FULLSCREEN) return;\n"
        "\tSDL_SetWindowPosition(window.get(), pos.x, pos.y);\n"
        "}\n")
    new_setpos = (
        "void VisibleSurface::setWindowPosition(gl::ivec2 pos)\n"
        "{\n"
        "#ifdef __APPLE__\n"
        "\tmsxRunOnMainThread([&] {\n"
        "#endif\n"
        "\t\tif (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_FULLSCREEN) return;\n"
        "\t\tSDL_SetWindowPosition(window.get(), pos.x, pos.y);\n"
        "#ifdef __APPLE__\n"
        "\t});\n"
        "#endif\n"
        "}\n")
    if old_setpos not in text:
        fail(f"{path}: VisibleSurface::setWindowPosition() anchor not found "
             "(openMSX source has drifted from the pinned commit?)")
    text = text.replace(old_setpos, new_setpos, 1)

    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"Patched {path}: window teardown dispatched to the main thread "
          "on macOS")


def patch_darwin_blocks_flag(stage_dir: str) -> None:
    path = f"{stage_dir}/build/platform-darwin.mk"
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    marker = "TARGET_FLAGS+=-fblocks"
    if marker in text:
        return  # already patched
    old = (
        "# Enable automatic reference counting in Objective-C\n"
        "ifneq ($(3RDPARTY_FLAG),true)\n"
        "TARGET_FLAGS+=-fobjc-arc\n"
        "endif\n")
    new = (
        "# Enable automatic reference counting in Objective-C\n"
        "ifneq ($(3RDPARTY_FLAG),true)\n"
        "TARGET_FLAGS+=-fobjc-arc\n"
        "endif\n"
        "\n"
        "# FujiNet Go MSX (desktop): see patch-staged-tree.py. Blocks are the\n"
        "# dispatch_sync syntax the VisibleSurface.cc patch above needs;\n"
        f"# {marker} makes that explicit rather than relying on Apple Clang's\n"
        "# own Darwin-target default.\n"
        f"{marker}\n")
    if old not in text:
        fail(f"{path}: TARGET_FLAGS/-fobjc-arc anchor not found "
             "(openMSX source has drifted from the pinned commit?)")
    text = text.replace(old, new, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"Patched {path}: -fblocks added to TARGET_FLAGS")


def patch_pkgconfig_glib_bool_identifier(stage_dir: str) -> None:
    path = f"{stage_dir}/build/3rdparty.mk"
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    marker = "FujiNet Go MSX (desktop): see patch-staged-tree.py -- this"
    if marker in text:
        return  # already patched
    old = (
        "# Configure pkg-config.\n"
        "$(BUILD_DIR)/$(PACKAGE_PKG_CONFIG)/Makefile: \\\n"
        "  $(SOURCE_DIR)/$(PACKAGE_PKG_CONFIG)/.extracted\n"
        "\tmkdir -p $(@D)\n"
        "\tcd $(@D) && $(PWD)/$(<D)/configure \\\n")
    new = (
        "# Configure pkg-config.\n"
        "$(BUILD_DIR)/$(PACKAGE_PKG_CONFIG)/Makefile: \\\n"
        "  $(SOURCE_DIR)/$(PACKAGE_PKG_CONFIG)/.extracted\n"
        "\tmkdir -p $(@D)\n"
        f"\t# {marker}\n"
        "\t# bundled pkg-config's internal glib (2013-era) declares a\n"
        "\t# struct member and later accesses a union member both literally\n"
        "\t# named `bool` (glib/goption.c) -- legal C when it was written,\n"
        "\t# since bool was just an ordinary identifier unless a translation\n"
        "\t# unit opted in via <stdbool.h>. A real Windows CI run failed here\n"
        "\t# (\"two or more data types in declaration specifiers\" /\n"
        "\t# \"expected identifier before 'bool'\"): the MinGW-w64 GCC\n"
        "\t# windows.yml pulls in defaults to -std=gnu23, where bool/true/\n"
        "\t# false became real keywords. A CFLAGS-based fix (an earlier\n"
        "\t# version of this patch) does not work: pkg-config's own configure\n"
        "\t# does not forward CFLAGS to this bundled glib's own recursive\n"
        "\t# ./configure -- confirmed by reading real CI's own \"configure:\n"
        "\t# running ... glib/configure ... 'CC=...'\" announcement, where CC\n"
        "\t# is forwarded but CFLAGS is conspicuously not. Renaming the\n"
        "\t# identifier directly is a one-line, portable fix that sidesteps\n"
        "\t# the question of which flags reach this compile entirely --\n"
        "\t# unconditional (no platform gate needed): the rename is\n"
        "\t# semantically identical on every compiler/dialect, and this\n"
        "\t# module has no other code depending on the field's exact name.\n"
        "\tsed -i.bak \\\n"
        "\t\t-e 's/gboolean bool;/gboolean bool_val;/' \\\n"
        "\t\t-e 's/change->prev\\.bool;/change->prev.bool_val;/' \\\n"
        "\t\t$(PWD)/$(<D)/glib/glib/goption.c\n"
        "\t# Same file, unrelated bug: this bundled glib's own G_GSIZE_FORMAT\n"
        "\t# (glib/glib/gslice.c's three MemChecker fprintf() calls) resolves\n"
        "\t# to a plain \"u\" on this target -- wrong for a 64-bit size_t on\n"
        "\t# 64-bit Windows (LLP64: size_t is 8 bytes, plain unsigned int is\n"
        "\t# 4) -- rather than to something matching size_t's real width.\n"
        "\t# Confirmed by a real Windows CI run: \"format '%u' expects\n"
        "\t# argument of type 'unsigned int', but argument ... has type\n"
        "\t# 'size_t' {aka 'long long unsigned int'}\" at all three call\n"
        "\t# sites. The arguments printed here (pointer, size, real_size) are\n"
        "\t# only ever used for a diagnostic message on an already-corrupt-\n"
        "\t# heap error path, so the standard, portable %zu -- guaranteed\n"
        "\t# correct for size_t specifically, unlike this old macro -- is a\n"
        "\t# direct textual substitute: G_GSIZE_FORMAT itself expands to a\n"
        "\t# quoted string spliced in by C's adjacent-string-literal\n"
        "\t# concatenation (e.g. \"%\" G_GSIZE_FORMAT \"\\n\"), so replacing the\n"
        "\t# bare macro token with the literal \"zu\" keeps that concatenation\n"
        "\t# valid. All 4 occurrences in this file are this exact pattern\n"
        "\t# (confirmed against the real pinned tarball -- no other use of\n"
        "\t# the macro exists in gslice.c), so a whole-file replace is safe.\n"
        "\tsed -i.bak -e 's/G_GSIZE_FORMAT/\"zu\"/g' \\\n"
        "\t\t$(PWD)/$(<D)/glib/glib/gslice.c\n"
        "\t# Third distinct bug in this bundled glib, in glib/gstdio.c:\n"
        "\t# g_stat()'s buf is GStatBuf* -- plain `struct stat` here, since\n"
        "\t# this file's only special-case (a #if defined(_MSC_VER) &&\n"
        "\t# !defined(_WIN64) block a few lines above) doesn't apply when\n"
        "\t# compiling with GCC -- but _wstat expands (via mingw-w64's own\n"
        "\t# _mingw_stat64.h, unconditionally when _USE_32BIT_TIME_T is not\n"
        "\t# defined, which this project never defines) to _wstat64i32,\n"
        "\t# which wants `struct _stat64i32 *` specifically -- a distinct\n"
        "\t# struct tag from plain `struct stat` as far as the type system\n"
        "\t# is concerned, even though mingw-w64's own header comments and\n"
        "\t# field-by-field inspection (both confirmed against the real\n"
        "\t# x86_64-w64-mingw32-gcc 16.1.0 headers this project's own\n"
        "\t# windows.yml cross-compiles this repo's own code with) show the\n"
        "\t# two structs are field-for-field identical under this project's\n"
        "\t# exact configuration (neither _USE_32BIT_TIME_T nor\n"
        "\t# _FILE_OFFSET_BITS is ever defined anywhere in this build), so\n"
        "\t# the cast this sed inserts is provably safe, not just silencing\n"
        "\t# a warning. Confirmed by cross-compiling both the broken and\n"
        "\t# fixed call with the real toolchain: without the cast,\n"
        "\t# \"-Wincompatible-pointer-types\" reproduces character-for-\n"
        "\t# character identical to real CI's own error; with it, zero\n"
        "\t# warnings.\n"
        "\tsed -i.bak \\\n"
        "\t\t-e 's/_wstat (wfilename, buf);/_wstat (wfilename, (struct _stat64i32 *) buf);/' \\\n"
        "\t\t$(PWD)/$(<D)/glib/glib/gstdio.c\n"
        "\t# Fourth distinct bug, in glib/gthread-win32.c: two of this\n"
        "\t# file's four InterlockedCompareExchangePointer() call sites pass\n"
        "\t# a specifically-typed pointer's address (GPrivateDestructor *\n"
        "\t# volatile * and GThreadXpCONDITION_VARIABLE * volatile *) where\n"
        "\t# mingw-w64's own headers declare the first parameter as plain\n"
        "\t# `void * volatile *` -- the other two call sites in this same\n"
        "\t# file (mutex->p, key->p) already pass an already-gpointer-typed\n"
        "\t# field's address and were never reported as errors by real CI,\n"
        "\t# so only these two need the same explicit (void **) cast\n"
        "\t# pattern already used twice above for a different type-tag\n"
        "\t# mismatch. Confirmed against the real x86_64-w64-mingw32-gcc\n"
        "\t# 16.1.0 headers (same version windows.yml uses): without the\n"
        "\t# cast, a minimal reproduction using the exact declared types\n"
        "\t# reproduces real CI's \"expected 'void * volatile*' but argument\n"
        "\t# is of type 'GPrivateDestructor * volatile*'\" character for\n"
        "\t# character; with it, zero warnings under\n"
        "\t# -Werror=incompatible-pointer-types.\n"
        "\tsed -i.bak \\\n"
        "\t\t-e 's/InterlockedCompareExchangePointer (&g_private_destructors,/"
        "InterlockedCompareExchangePointer ((void **) \\&g_private_destructors,/' \\\n"
        "\t\t-e 's/InterlockedCompareExchangePointer (cond, result, NULL)/"
        "InterlockedCompareExchangePointer ((void **) cond, result, NULL)/' \\\n"
        "\t\t$(PWD)/$(<D)/glib/glib/gthread-win32.c\n"
        "\tcd $(@D) && $(PWD)/$(<D)/configure \\\n")
    if old not in text:
        fail(f"{path}: pkg-config configure-recipe anchor not found "
             "(openMSX source has drifted from the pinned commit?)")
    text = text.replace(old, new, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"Patched {path}: fixed pkg-config/glib's `bool` identifier, "
          "G_GSIZE_FORMAT, _wstat64i32 struct-tag mismatch and "
          "InterlockedCompareExchangePointer type mismatches before "
          "configure runs")


def patch_pkgconfig_path_export(stage_dir: str) -> None:
    path = f"{stage_dir}/build/3rdparty.mk"
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    marker = "export PKG_CONFIG_PATH:="
    if marker in text:
        return  # already patched
    old = "export PKG_CONFIG:=$(PWD)/$(TOOLS_DIR)/bin/$(TARGET_TRIPLE)-pkg-config\n"
    new = (
        "export PKG_CONFIG:=$(PWD)/$(TOOLS_DIR)/bin/$(TARGET_TRIPLE)-pkg-config\n"
        "# FujiNet Go MSX (desktop): see patch-staged-tree.py -- without this,\n"
        "# the freshly cross-built pkg-config above has no PKG_CONFIG_PATH at\n"
        "# all, so it never finds any .pc file this same 3rdparty chain\n"
        "# installs for its own later packages to depend on (confirmed by a\n"
        "# real Windows CI run: SDL2_ttf's ./configure failed to find\n"
        "# freetype2.pc despite it being right there in this exact\n"
        "# directory). A strict addition -- can only let pkg-config find\n"
        "# .pc files it previously couldn't -- so it is safe unconditionally.\n"
        f"{marker}$(PWD)/$(INSTALL_DIR)/lib/pkgconfig\n"
    )
    if old not in text:
        fail(f"{path}: PKG_CONFIG export anchor not found "
             "(openMSX source has drifted from the pinned commit?)")
    text = text.replace(old, new, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"Patched {path}: exported PKG_CONFIG_PATH for the 3rdparty chain")


def patch_cross_pkgconfig_exe_suffix(stage_dir: str) -> None:
    path = f"{stage_dir}/build/libraries.py"
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    marker = "-pkg-config.exe"
    if marker in text:
        return  # already patched
    old = "\t\t\tif name.endswith('-pkg-config'):\n"
    new = (
        "\t\t\t# FujiNet Go MSX (desktop): see patch-staged-tree.py -- the\n"
        "\t\t\t# real installed filename on Windows is 'x86_64-w64-mingw32-\n"
        "\t\t\t# pkg-config.exe' (confirmed directly from pkg-config's own\n"
        "\t\t\t# Makefile.am: `host_tool = $(host)-pkg-config$(EXEEXT)`),\n"
        "\t\t\t# not the extension-less name this check originally assumed\n"
        "\t\t\t# -- a real Windows CI run raised the RuntimeError below\n"
        "\t\t\t# despite the file genuinely being right there.\n"
        f"\t\t\tif name.endswith('-pkg-config') or name.endswith('{marker}'):\n"
    )
    if old not in text:
        fail(f"{path}: _get_pkg_config() filename-match anchor not found "
             "(openMSX source has drifted from the pinned commit?)")
    text = text.replace(old, new, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"Patched {path}: _get_pkg_config() now matches a .exe-suffixed "
          "cross-pkg-config too")


def patch_freetype_pkgconfig_version_flag(stage_dir: str) -> None:
    path = f"{stage_dir}/build/libraries.py"
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    marker = "FujiNet Go MSX (desktop): see patch-staged-tree.py -- --ftversion"
    if marker in text:
        return  # already patched
    old = (
        "\t@classmethod\n"
        "\tdef getVersion(cls, platform, linkStatic, distroRoot):\n"
        "\t\tconfigScript = cls.getConfigScript(platform, linkStatic, distroRoot)\n"
        "\t\treturn '`%s --ftversion`' % configScript\n")
    new = (
        "\t@classmethod\n"
        "\tdef getVersion(cls, platform, linkStatic, distroRoot):\n"
        "\t\tconfigScript = cls.getConfigScript(platform, linkStatic, distroRoot)\n"
        f"\t\t# {marker} is a real\n"
        "\t\t# freetype-config flag, but this class's own getConfigScript()\n"
        "\t\t# above falls back to invoking pkg-config directly whenever the\n"
        "\t\t# real freetype-config script isn't installed (true for this\n"
        "\t\t# bundled freetype -- see that method's own comment), and\n"
        "\t\t# pkg-config's version flag is --modversion, not --ftversion --\n"
        "\t\t# the same distinction the base Library.getVersion() already\n"
        "\t\t# handles, just never replicated here. A real Windows CI run's\n"
        "\t\t# probe.log confirmed the mismatch: 'Unknown option --ftversion'\n"
        "\t\t# from pkg-config, despite FreeType's header and lib both\n"
        "\t\t# already being found correctly.\n"
        "\t\tif 'pkg-config' in configScript:\n"
        "\t\t\treturn '`%s --modversion`' % configScript\n"
        "\t\treturn '`%s --ftversion`' % configScript\n")
    if old not in text:
        fail(f"{path}: FreeType.getVersion() anchor not found "
             "(openMSX source has drifted from the pinned commit?)")
    text = text.replace(old, new, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"Patched {path}: FreeType.getVersion() now uses --modversion on "
          "the pkg-config fallback path")


def patch_msysutils_python3_fixes(stage_dir: str) -> None:
    path = f"{stage_dir}/build/msysutils.py"
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    marker = "print(sys.argv[1])"
    if marker in text:
        return  # already patched

    # Fix 1: a Python-2-only print STATEMENT embedded in a command string
    # this function shells out to python3 with. See this file's own
    # docstring for the real Windows CI evidence.
    old_print = "print sys.argv[1]"
    if old_print not in text:
        fail(f"{path}: Python-2 print-statement anchor not found "
             "(openMSX source has drifted from the pinned commit?)")
    text = text.replace(old_print, marker, 1)

    # Fix 2: Popen(..., stdout=PIPE).communicate() returns bytes under
    # Python 3 (no text=True given), but msysRoot immediately gets
    # concatenated with str literals twice below (fstab = msysRoot +
    # '/etc/fstab', then mounts['/'] = msysRoot + '/') -- a
    # TypeError: can't concat str to bytes real Windows CI hit
    # immediately after fix 1 above unblocked this function far enough to
    # reach it. Decoding once here, where the bytes value is first
    # captured, fixes both downstream concatenations in one place rather
    # than patching each site separately.
    old_decode = "\tmsysRoot = stdoutdata.strip()\n"
    new_decode = "\tmsysRoot = stdoutdata.decode('utf-8').strip()\n"
    if old_decode not in text:
        fail(f"{path}: msysRoot bytes/str anchor not found "
             "(openMSX source has drifted from the pinned commit?)")
    text = text.replace(old_decode, new_decode, 1)

    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"Patched {path}: two Python 2/3 bugs fixed (print statement, "
          "bytes/str concatenation)")


def main() -> None:
    if len(sys.argv) != 2:
        fail("usage: patch-staged-tree.py <staged-openmsx-dir>")
    stage_dir = sys.argv[1]
    patch_fujinet_port(stage_dir)
    patch_frame_hook(stage_dir)
    patch_debug_pump_hook(stage_dir)
    patch_hidden_window(stage_dir)
    patch_disable_input_poll(stage_dir)
    patch_macos_main_thread_window(stage_dir)
    patch_macos_window_teardown(stage_dir)
    patch_darwin_blocks_flag(stage_dir)
    patch_pkgconfig_glib_bool_identifier(stage_dir)
    patch_pkgconfig_path_export(stage_dir)
    patch_cross_pkgconfig_exe_suffix(stage_dir)
    patch_freetype_pkgconfig_version_flag(stage_dir)
    patch_msysutils_python3_fixes(stage_dir)


if __name__ == "__main__":
    main()
