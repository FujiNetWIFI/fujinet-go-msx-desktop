# Cross-compile the Windows frontend from Linux with mingw-w64.
#
# The maintainer has no Windows machine, so this is how Windows changes to
# the *session/frontend* code get compiled and linked before CI (which
# builds natively under MSYS2/UCRT64) ever sees them. Wine can then run the
# result for a smoke test.
#
# This toolchain file is NOT how openMSX itself gets cross-compiled for
# Windows -- openMSX's own GNUmakefile (OPENMSX_TARGET_OS=mingw-w64) drives
# that, invoked by tools/openmsx/build-openmsx-desktop.sh, and needs to run
# from an actual MSYS2/UCRT64 shell (its 3rdparty chain expects MSYS2's own
# toolchain layout). windows.yml therefore builds natively; this file is
# scoped to smoke-testing the small C session/frontend code and the FujiNet
# runtime cross-build against a *pre-built* libopenmsx.a produced elsewhere
# (or with -DWITH_FUJINET=OFF and a stub, for a pure link-surface check).
#
#   # once: SDL2 + SDL2_ttf for the cross target (openMSX links these
#   # itself when built via MSYS2; this is only needed if smoke-testing
#   # session.c's own use of SDL2 headers without a full openMSX build)
#   git clone --depth 1 --branch release-2.30.7 \
#       https://github.com/libsdl-org/SDL.git /tmp/sdl2-src
#   cmake -S /tmp/sdl2-src -B /tmp/sdl2-src/build -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/toolchains/mingw-w64.cmake \
#       -DCMAKE_INSTALL_PREFIX=/tmp/sdl2-mingw \
#       -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF
#   cmake --build /tmp/sdl2-src/build && cmake --install /tmp/sdl2-src/build
#
#   cmake -B build-win -G Ninja -DWITH_FUJINET=OFF \
#       -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/toolchains/mingw-w64.cmake \
#       -DCMAKE_PREFIX_PATH="/tmp/sdl2-mingw"
#   cmake --build build-win
#
# For the full app *including* fujinet.dll, FujiNet's own dependencies have
# to be cross-built too (MSYS2 provides them as packages; here they are
# built from source into one prefix, say /tmp/win-deps) -- see
# fujinet-go-coco-desktop's copy of this file for the expat/zlib/mbedTLS/
# openssl recipe, unchanged for this target.
#
# Running it under wine wants the runtime beside the exe, exactly like the
# shipped folder:
#
#   cp tools/fujinet/work/out/fujinet.dll build-win/frontends/windows/
#   cp -r tools/fujinet/work/out/{data,SD} tools/fujinet/work/out/fnconfig.ini \
#       build-win/frontends/windows/fujinet/
#   WINEPATH='Z:\usr\x86_64-w64-mingw32\bin' wine \
#       build-win/frontends/windows/fujinet-go-msx-windows.exe

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)

# Search only the cross sysroot and whatever prefixes the caller named, so
# host headers and libraries cannot leak into a Windows build. (Without
# this, a find_package can report "found version 1.3.2" from /usr/include
# and then fail on the missing library, which is a confusing way to learn
# that a dependency has not been cross-built yet.)
set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX} ${CMAKE_PREFIX_PATH})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
