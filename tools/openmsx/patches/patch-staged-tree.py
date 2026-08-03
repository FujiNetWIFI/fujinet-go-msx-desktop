#!/usr/bin/env python3
"""Idempotent source patches applied to the staged openMSX tree.

Invoked by cmake/StageOpenMSX.cmake, never run directly. Each patch is
anchored to exact text and hard-fails (nonzero exit) when its anchor is
missing, so a pin that has drifted from what these patches expect is a loud
build error, not a silent no-op -- same discipline as
tools/xroar/patch-staged-tree.py in the sibling CoCo repo.

Two patches:

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


def main() -> None:
    if len(sys.argv) != 2:
        fail("usage: patch-staged-tree.py <staged-openmsx-dir>")
    stage_dir = sys.argv[1]
    patch_fujinet_port(stage_dir)
    patch_frame_hook(stage_dir)


if __name__ == "__main__":
    main()
