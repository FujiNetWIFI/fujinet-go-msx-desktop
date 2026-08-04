# Build openMSX (staged by cmake/StageOpenMSX.cmake) into a static archive
# via tools/openmsx/build-openmsx-desktop.sh, which drives openMSX's own
# GNUmakefile. See that script and COMPLIANCE.md for why this project does
# not compile the staged tree with its own CMake the way the sibling repos
# compile XRoar/AppleWin/adamcore.
#
# Mirrors cmake/FujiNetRuntime.cmake's shape: a stamp file so re-configuring
# does not trigger a rebuild, an add_custom_command producing the real
# OUTPUT (libopenmsx.a), an ALL target depending on it, and a `-refresh`
# escape hatch for iterating on the openMSX sources themselves.

set(OPENMSX_OUT "${CMAKE_SOURCE_DIR}/tools/openmsx/work/out")
set(OPENMSX_LIB "${OPENMSX_OUT}/lib/libopenmsx.a")
set(OPENMSX_BUILD_SCRIPT
    "${CMAKE_SOURCE_DIR}/tools/openmsx/build-openmsx-desktop.sh")

find_program(BASH_EXECUTABLE bash)
if(NOT BASH_EXECUTABLE)
  message(FATAL_ERROR
    "bash is required to build openMSX. On Windows, build from an MSYS2 "
    "UCRT64 shell.")
endif()

# Which of the three platforms openMSX's GNUmakefile targets. Left empty on
# an ordinary host build so openMSX's own build/detectsys.py autodetects
# (matters for e.g. a Linux ARM dev machine); the mingw-w64 toolchain file
# and the macOS CI job set this explicitly.
if(WIN32)
  set(_openmsx_target_os "mingw-w64")
elseif(APPLE)
  set(_openmsx_target_os "darwin")
elseif(UNIX)
  set(_openmsx_target_os "linux")
else()
  set(_openmsx_target_os "")
endif()

# Re-run the build when the recipe or the patched source identity changes.
# Embedding .source-info's own content (StageOpenMSX.cmake's output) means
# an -DOPENMSX_RESTAGE=ON re-stage also triggers a rebuild.
# configure_file(COPYONLY) only touches the destination's mtime when the
# content actually differs, so an ordinary re-configure does not retrigger
# the expensive build.
set(_openmsx_source_info "")
if(EXISTS "${OPENMSX_GEN}/.source-info")
  file(READ "${OPENMSX_GEN}/.source-info" _openmsx_source_info)
endif()
set(OPENMSX_STAMP "${CMAKE_BINARY_DIR}/openmsx-source.stamp")
file(WRITE "${CMAKE_BINARY_DIR}/CMakeFiles/openmsx-source.stamp.in"
     "${_openmsx_target_os}\n${_openmsx_source_info}")
configure_file("${CMAKE_BINARY_DIR}/CMakeFiles/openmsx-source.stamp.in"
               "${OPENMSX_STAMP}" COPYONLY)

add_custom_command(
  OUTPUT "${OPENMSX_LIB}"
  COMMAND ${CMAKE_COMMAND} -E env "OPENMSX_TARGET_OS=${_openmsx_target_os}"
          "${BASH_EXECUTABLE}" "${OPENMSX_BUILD_SCRIPT}"
  DEPENDS "${OPENMSX_BUILD_SCRIPT}" "${OPENMSX_GEN}/.source-info"
          "${OPENMSX_STAMP}"
  COMMENT "Building openMSX (first build takes several minutes)"
  USES_TERMINAL
  VERBATIM)
add_custom_target(openmsx-build ALL DEPENDS "${OPENMSX_LIB}")

add_custom_target(openmsx-build-refresh
  COMMAND ${CMAKE_COMMAND} -E env "OPENMSX_TARGET_OS=${_openmsx_target_os}"
          "${BASH_EXECUTABLE}" "${OPENMSX_BUILD_SCRIPT}"
  COMMENT "Rebuilding openMSX unconditionally"
  USES_TERMINAL
  VERBATIM)

# The per-subdirectory include path list is computed here, at *configure*
# time, straight from the staged source tree (core/openmsx-generated/src --
# StageOpenMSX.cmake has already populated it by the time this file runs),
# mirroring openMSX's own SOURCE_DIRS logic (build/main.mk:260: every
# subdirectory of src, sorted) so a future openMSX version that adds a
# subdirectory does not silently break the build.
#
# This *must* come from the staged tree, not a file build-openmsx-desktop.sh
# writes during the build: a target's INTERFACE_INCLUDE_DIRECTORIES is baked
# into build.ninja at generate time, which happens once per `cmake -B build`
# invocation and is never revisited just because a later `cmake --build`
# step produces a new file. A from-scratch clone's first configure would
# otherwise permanently bake in an empty subdirectory list (only the two
# fallback entries below), because no build has run yet to write it -- a
# real bug this comment used to (wrongly) claim CMake reconfigures around;
# the clean-clone gate test in TODO caught it. Globbing the staged source
# instead needs no such build-then-reconfigure dance, since the directories
# being listed already exist at configure time.
file(GLOB_RECURSE _openmsx_src_dirs LIST_DIRECTORIES true
     "${OPENMSX_GEN}/src/*")
set(OPENMSX_INCLUDE_DIRS "")
foreach(_d IN LISTS _openmsx_src_dirs)
  if(IS_DIRECTORY "${_d}")
    list(APPEND OPENMSX_INCLUDE_DIRS "${_d}")
  endif()
endforeach()
list(APPEND OPENMSX_INCLUDE_DIRS
     "${OPENMSX_GEN}/src"
     "${OPENMSX_OUT}/include/openmsx-config")

# The generated config headers (components.hh, build-info.hh, ...) are
# genuinely build-time output (platform+flavour specific, written by
# build-openmsx-desktop.sh from derived/<platform>/config once it knows
# which platform it built for) -- unlike the subdirectory list above, this
# one path cannot be known from the staged source alone. CMake fatally
# errors on an IMPORTED target's INTERFACE_INCLUDE_DIRECTORIES containing a
# path that does not yet exist, so create it empty now (the standard
# ExternalProject_Add-style workaround); the real headers land inside once
# the custom command below runs, which is fine -- only the *path* needs to
# exist at configure time, not populated contents, since compiling against
# it always happens after that custom command in the dependency graph.
file(MAKE_DIRECTORY "${OPENMSX_OUT}/include/openmsx-config")

# macOS/Windows only (NEED_3RDPARTY in build-openmsx-desktop.sh): that
# script copies the 3rd-party chain's own from-source SDL2/Tcl/etc headers
# here, since there is no system SDL2 to pkg-config against on those
# platforms -- core/CMakeLists.txt needs SDL2's headers for msx_host.cc/
# input_map.c's event-injection types. Same "the path must exist at
# configure time even though the build hasn't produced its real contents
# yet" reasoning as openmsx-config above; harmless and unused on Linux,
# where core/CMakeLists.txt's own pkg_check_modules(sdl2) covers it instead.
if(NOT _openmsx_target_os STREQUAL "linux")
  file(MAKE_DIRECTORY "${OPENMSX_OUT}/include/3rdparty")
  list(APPEND OPENMSX_INCLUDE_DIRS "${OPENMSX_OUT}/include/3rdparty")
  # SDL2's own CMake install layout nests its headers under an SDL2/
  # subdirectory (.../include/3rdparty/SDL2/SDL.h), not flat -- confirmed
  # by the first real macOS CI run, which built openMSX's whole static
  # 3rd-party chain successfully but then failed to compile msxsession's
  # own input_map.c/msx_host.cc with "SDL.h: file not found" against just
  # the flat path above. Linux does not need this extra entry: pkg-config
  # already resolves sdl2's cflags to the equally-specific
  # /usr/include/SDL2 on its own.
  file(MAKE_DIRECTORY "${OPENMSX_OUT}/include/3rdparty/SDL2")
  list(APPEND OPENMSX_INCLUDE_DIRS "${OPENMSX_OUT}/include/3rdparty/SDL2")
endif()

add_library(openmsx-lib STATIC IMPORTED GLOBAL)
set_target_properties(openmsx-lib PROPERTIES
  IMPORTED_LOCATION "${OPENMSX_LIB}"
  INTERFACE_INCLUDE_DIRECTORIES "${OPENMSX_INCLUDE_DIRS}")
add_dependencies(openmsx-lib openmsx-build)

# macOS/Windows only: build-openmsx-desktop.sh merges every 3rd-party static
# archive's objects straight into libopenmsx.a itself (see that script's own
# comment for why -- in short, a file(GLOB) here to discover them as
# separate lib/*.a files would run at CMake *configure* time, before the
# build-time custom command that produces them has ever run, so on a truly
# fresh clone's first configure it would always find nothing and silently
# link in zero 3rd-party symbols; a real macOS CI failure confirmed exactly
# that). Nothing further to declare here as a result -- IMPORTED_LOCATION
# above already points at the one, now self-contained archive.
#
# What IS still needed here, and cannot come from any archive: macOS system
# frameworks. openMSX's own build/platform-darwin.mk links CoreFoundation/
# CoreServices/ApplicationServices/CoreMIDI/Foundation itself (copied
# verbatim from there, the authoritative source); OpenGL is what
# build/libraries.py resolves the GL library to on this platform (`-framework
# OpenGL`, not a pkg-config module the way Linux's `gl` is); the rest is the
# standard framework set SDL2's own build links on macOS for windowing/
# audio/joystick/haptics -- confirmed necessary across two real macOS CI
# failures, first one whose undefined-symbol list spanned CoreFoundation,
# CoreMIDI, Foundation (Objective-C runtime calls) and OpenGL functions all
# at once (a build with no framework linkage at all), then a second,
# far smaller one (Metal/CoreHaptics symbols only, from SDL2's optional
# Metal renderer backend and its GameController-based rumble/haptics
# support) once the first, much larger gap was closed -- both fixed from
# the same real-CI-evidence loop, not guessed at once from a components
# list.
if(APPLE)
  set_property(TARGET openmsx-lib APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES
    "-framework CoreFoundation" "-framework CoreServices"
    "-framework ApplicationServices" "-framework CoreMIDI"
    "-framework Foundation" "-framework OpenGL" "-framework Cocoa"
    "-framework CoreVideo" "-framework CoreAudio" "-framework AudioToolbox"
    "-framework IOKit" "-framework Carbon" "-framework ForceFeedback"
    "-framework GameController" "-framework Metal" "-framework CoreHaptics")
endif()

# Windows: the mingw-w64 equivalent of the macOS framework block above --
# system import libraries the statically-merged 3rd-party archives and
# openMSX's own src/ tree need that cannot come from any archive either.
# A real Windows CI run's undefined-symbol list, once every earlier build-
# tooling/compile-time issue was fixed and the link step itself finally ran
# for the first time, was verified symbol-by-symbol against this dev
# machine's own x86_64-w64-mingw32 toolchain (same version windows.yml
# uses) rather than assigned from memory:
#   secur32   -- src/security/SspiNegotiateServer.cc/SspiUtils.cc, openMSX's
#                own Windows-native SSPI auth for its remote-control server
#                (AcquireCredentialsHandleW, AcceptSecurityContext, ...)
#   opengl32  -- openMSX's own GL renderer (glBindTexture, glReadPixels,
#                ...) plus wglGetCurrentDC/wglGetProcAddress -- Linux gets
#                this from pkg-config's gl module and macOS from
#                -framework OpenGL above; Windows has no equivalent
#                automatic source, it is always a plain system import lib
#   setupapi,
#   cfgmgr32  -- SDL2's hidapi joystick backend (SetupDiGetClassDevsA,
#                CM_Get_Device_IDA, ...) -- these two are always a pair for
#                Windows device enumeration
#   imm32     -- SDL2's Input Method Editor support (ImmGetContext, ...)
#   winmm     -- SDL2's own multimedia timer (timeBeginPeriod/
#                timeEndPeriod) and openMSX's own native MIDI/WaveAudio
#                backends (midiInOpen/waveOutOpen and the rest of that
#                family)
#   version   -- SDL2's own driver-version checks (GetFileVersionInfoA,
#                VerQueryValueA)
#   netapi32,
#   userenv   -- Tcl's own static-link requirements, confirmed directly
#                from this same CI run's own probe.py output (the exact
#                "-lnetapi32 -lkernel32 -luser32 -ladvapi32 -luserenv
#                -lws2_32" linker command build/libraries.py's TCL class
#                sources from tclConfig.sh) -- kernel32/user32/advapi32/
#                ws2_32 all resolved without being listed explicitly here
#                (MinGW's own default-linked library set already covers
#                the first three; ws2_32 is msxsession's own WIN32 block
#                in core/CMakeLists.txt, unrelated to Tcl specifically)
if(WIN32)
  set_property(TARGET openmsx-lib APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES
    secur32 opengl32 setupapi cfgmgr32 imm32 winmm version netapi32 userenv)
endif()

# Linux: openMSX links these dynamically against the distribution's own
# copies (LINK_MODE=SYS_DYN, build-openmsx-desktop.sh's default there), so
# anything linking libopenmsx.a needs them too -- exactly what
# `ldd derived/*/bin/openmsx` on a native openMSX build shows. macOS and
# mingw-w64 instead get these from the static 3rd-party archives just
# globbed above (NEED_3RDPARTY in build-openmsx-desktop.sh), so this block
# is Linux-only; CPACK_DEBIAN_PACKAGE_SHLIBDEPS/rpmbuild's own dependency
# generator turn the same linkage into real installed-package dependencies.
if(UNIX AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(OPENMSX_SYS_DEPS REQUIRED IMPORTED_TARGET
    sdl2 SDL2_ttf libpng ogg theoradec vorbis glew zlib gl alsa freetype2)
  find_library(OPENMSX_TCL_LIBRARY NAMES tcl8.6 tcl)
  if(NOT OPENMSX_TCL_LIBRARY)
    message(FATAL_ERROR "Tcl library not found (openMSX's scripting engine "
                        "needs libtcl8.6 or libtcl; no pkg-config module "
                        "exists for it, so this is a plain find_library).")
  endif()
  # tcl.h's directory, separately from the library above: this dev machine's
  # tcl.h happens to sit straight in /usr/include (already on every
  # compiler's default search path, so this went unnoticed for a while), but
  # that is not guaranteed -- Debian/Ubuntu's tcl-dev package installs it
  # under a versioned .../tcl8.6/tcl.h subdirectory instead, and a flatpak
  # module's own /app/include is never on the default path at all. Search
  # explicitly (PATH_SUFFIXES covers the versioned-subdirectory case; the
  # unversioned case still matches because find_path also tries the bare
  # directory) rather than continue relying on luck.
  find_path(OPENMSX_TCL_INCLUDE_DIR NAMES tcl.h PATH_SUFFIXES tcl8.6 tcl)
  if(NOT OPENMSX_TCL_INCLUDE_DIR)
    message(FATAL_ERROR "tcl.h not found (checked plain include dirs and "
                        "their tcl8.6/tcl subdirectories).")
  endif()
  set_property(TARGET openmsx-lib APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES PkgConfig::OPENMSX_SYS_DEPS "${OPENMSX_TCL_LIBRARY}")
  set_property(TARGET openmsx-lib APPEND PROPERTY
    INTERFACE_INCLUDE_DIRECTORIES "${OPENMSX_TCL_INCLUDE_DIR}")
endif()
