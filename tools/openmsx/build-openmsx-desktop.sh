#!/usr/bin/env bash
#
# Build openMSX from the already-staged-and-patched tree
# (core/openmsx-generated, produced by cmake/StageOpenMSX.cmake) into a
# static archive this project links, using openMSX's own GNUmakefile rather
# than reinventing its build. See COMPLIANCE.md and TODO for why.
#
# Invoked by cmake/OpenMSXRuntime.cmake, always through bash by name (never
# as a bare .sh -- MSYS2 cmake cannot execute_process() a script; the same
# reason tools/fujinet/build-fujinet-desktop.sh is invoked this way).
#
# Outputs, under tools/openmsx/work/out/:
#   lib/libopenmsx.a       every openMSX object except main.o (our own
#                           frontends provide main()), archived; on macOS/
#                           Windows this also has every 3rd-party static
#                           archive's objects merged straight in (Linux
#                           links the system copies dynamically instead) --
#                           one self-contained library, not a separate
#                           lib/*.a set a consumer would need to discover
#   include/openmsx-config/ the generated config headers (components.hh,
#                           build-info.hh, ...) for this platform+flavour --
#                           the only headers actually copied here. The rest
#                           of the include path (every subdirectory of
#                           openMSX's own src/) is pointed by
#                           cmake/OpenMSXRuntime.cmake straight at the
#                           staged tree (core/openmsx-generated/src, the
#                           same headers these objects were compiled
#                           against), computed at *configure* time so a
#                           truly fresh clone's first configure sees the
#                           real list -- not a file only this build-time
#                           script could have written. See that file's
#                           comment for the clean-clone bug this replaced.
#   share/                  the runtime tree (init.tcl, scripts/, machines/
#                           incl. C-BIOS, extensions/ incl. FujiNet.xml)
#
# Usage: OPENMSX_TARGET_OS=linux|darwin|mingw-w64 bash build-openmsx-desktop.sh

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)
PROJECT_ROOT=$(cd -- "${SCRIPT_DIR}/../.." &>/dev/null && pwd)
STAGE_DIR="${PROJECT_ROOT}/core/openmsx-generated"
WORK_OUT="${PROJECT_ROOT}/tools/openmsx/work/out"

fail() { echo "build-openmsx-desktop.sh: $*" >&2; exit 1; }

[[ -f "${STAGE_DIR}/src/main.cc" ]] || fail \
  "staged openMSX tree not found at ${STAGE_DIR} -- cmake/StageOpenMSX.cmake should have produced it"

# OS: explicit env var, else openMSX's own autodetection (build/detectsys.py)
# is left to run by not passing OPENMSX_TARGET_OS at all. CPU is always left
# to autodetect -- the only reason to pin OS here is that this is a desktop
# port targeting exactly three, named per PORTING.md's per-target table.
TARGET_OS="${OPENMSX_TARGET_OS:-}"
case "${TARGET_OS}" in
  linux|darwin|mingw-w64|"") ;;
  *) fail "OPENMSX_TARGET_OS must be linux, darwin or mingw-w64 (got: ${TARGET_OS})" ;;
esac

# Static 3rd-party linking (Tcl/SDL2/SDL2_ttf/freetype/libpng/zlib/ogg/
# vorbis/theora/GLEW built from source, statically) is what makes the macOS
# `otool -L` and Windows `objdump -p` self-containment checks pass -- see
# PORTING.md. Linux links the distribution's own copies dynamically, same as
# every other frontend package in this family (CPACK_DEBIAN_PACKAGE_SHLIBDEPS
# already declares those as real package dependencies).
NEED_3RDPARTY=0
case "${TARGET_OS}" in
  darwin|mingw-w64) NEED_3RDPARTY=1 ;;
esac

JOBS="${OPENMSX_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

MAKE_VARS=(OPENMSX_FLAVOUR=opt PYTHON=python3)
[[ -n "${TARGET_OS}" ]] && MAKE_VARS+=(OPENMSX_TARGET_OS="${TARGET_OS}")
if [[ "${NEED_3RDPARTY}" -eq 1 ]]; then
    MAKE_VARS+=(3RDPARTY_FLAG=true)
fi

pushd "${STAGE_DIR}" >/dev/null

if [[ "${NEED_3RDPARTY}" -eq 1 ]]; then
    echo "==> Building openMSX's static 3rd-party stack (this takes a while the first time)"
    make "${MAKE_VARS[@]}" -j"${JOBS}" 3rdparty \
        || fail "openMSX 3rd-party build failed"
fi

echo "==> Building openMSX (${TARGET_OS:-host}, flavour=opt$([[ ${NEED_3RDPARTY} -eq 1 ]] && echo ', static 3rdparty'))"
# The default target links the full openmsx executable (main.cc's main()
# included). We don't want that executable -- our own frontends supply
# main() -- and in fact it CANNOT succeed: the frame-sink hook
# patch-staged-tree.py installs in PostProcessor.cc calls
# msxhost_notify_frame(), which only core/openmsx/msx_host.cc (linked into
# our own session library, not into openMSX's own executable) defines. So a
# link failure here is expected and tolerated, exactly like the Android
# sibling's build script tolerates it for its own (different) reason -- what
# both actually need is the object tree the compile step leaves behind
# regardless of whether the final link succeeded, which is archived next.
make "${MAKE_VARS[@]}" -j"${JOBS}" \
    || echo "    (openMSX's own final executable link failed, as expected -- msxhost_notify_frame() is defined by our own core/openmsx/msx_host.cc, not by openMSX; archiving the compiled objects regardless)"

# BUILD_PATH (main.mk) = derived/<cpu>-<os>-<flavour>[-3rd]
DERIVED_DIR=$(find derived -maxdepth 1 -mindepth 1 -type d | sort | tail -1)
[[ -n "${DERIVED_DIR}" ]] || fail "no derived/<platform> build directory produced"
echo "    build directory: ${DERIVED_DIR}"

popd >/dev/null

mkdir -p "${WORK_OUT}/lib" "${WORK_OUT}/include/openmsx-config"

OBJ_COUNT=$(find "${STAGE_DIR}/${DERIVED_DIR}/obj" -name '*.o' ! -name 'main.o' 2>/dev/null | wc -l)
[[ "${OBJ_COUNT}" -gt 0 ]] || fail "no openMSX objects produced under ${DERIVED_DIR}/obj"

AR="${AR:-ar}"
[[ "${TARGET_OS}" == "mingw-w64" ]] && AR="x86_64-w64-mingw32-ar"

find "${STAGE_DIR}/${DERIVED_DIR}/obj" -name '*.o' ! -name 'main.o' -print0 \
    | xargs -0 "${AR}" rcs "${WORK_OUT}/lib/libopenmsx.a"
echo "    archived ${OBJ_COUNT} openMSX objects -> lib/libopenmsx.a"

if [[ "${NEED_3RDPARTY}" -eq 1 ]]; then
    # Merge every 3rd-party static archive's objects directly into
    # libopenmsx.a itself, rather than copying them alongside it as
    # separate .a files for cmake/OpenMSXRuntime.cmake to discover with a
    # file(GLOB) afterward. That glob has to run at CMake *configure*
    # time, but these archives only exist once this build script -- a
    # build-time custom command -- has actually finished running; on a
    # truly fresh clone and build directory, the very first `cmake -B
    # build` therefore always sees an empty lib/ and silently links in
    # ZERO 3rd-party symbols, no error, nothing -- confirmed by a real
    # macOS CI failure where every SDL2/SDL2_ttf/Tcl/libpng/ogg/vorbis/
    # theora/GLEW/zlib symbol was undefined at the final link of this
    # project's own binaries. This dev machine's local builds never
    # tripped over it because tools/openmsx/work/out/lib already had
    # these archives sitting there from an earlier run's incremental
    # rebuild, so the glob "worked" by the same kind of luck the Tcl
    # search and 3rdparty install-path bugs already caught elsewhere in
    # this file relied on before their own fixes. Merging into
    # libopenmsx.a needs no discovery step at any time, at any point in
    # the configure/build sequence -- one self-contained static library.
    #
    # Each archive is extracted into its own numbered subdirectory and
    # every object renamed with that number as a prefix before being
    # added: ar's archive member table is a flat namespace keyed on
    # basename alone, so two different libraries that happen to ship a
    # same-named object (not far-fetched for generic names like init.o)
    # would otherwise silently clobber one another once merged into one
    # archive.
    merge_dir="$(mktemp -d)"
    trap 'rm -rf "${merge_dir}"' EXIT
    lib_count=0
    while IFS= read -r -d '' archive; do
        lib_count=$((lib_count + 1))
        extract_dir="${merge_dir}/${lib_count}"
        mkdir -p "${extract_dir}"
        ( cd "${extract_dir}" && "${AR}" x "${archive}" )
        for obj in "${extract_dir}"/*.o; do
            [[ -e "${obj}" ]] || continue
            mv "${obj}" "${extract_dir}/${lib_count}_$(basename "${obj}")"
        done
    done < <(find "${STAGE_DIR}/${DERIVED_DIR}/3rdparty" -name '*.a' -print0)
    if [[ "${lib_count}" -gt 0 ]]; then
        find "${merge_dir}" -name '*.o' -print0 \
            | xargs -0 "${AR}" rcs "${WORK_OUT}/lib/libopenmsx.a"
        echo "    merged ${lib_count} 3rd-party static archive(s) into lib/libopenmsx.a"
    fi
    # The 3rd-party chain's own install prefix (build/3rdparty.mk:
    # INSTALL_DIR=$(BUILD_PATH)/install, invoked with
    # BUILD_PATH=derived/<platform>/3rdparty -- one directory level below
    # DERIVED_DIR, confirmed by reading the actual `make` invocation's
    # BUILD_PATH= argument in a real macOS CI log, not assumed) is where
    # its from-source SDL2/Tcl/etc headers land -- core/CMakeLists.txt
    # needs SDL2's headers for the event-injection types msx_host.cc/
    # input_map.c use (SDL_Event, SDLK_*), and on this platform there is
    # no system SDL2 to pkg-config against (that is the whole point of
    # building it statically here). A first version of this copy step
    # omitted the "3rdparty" path component and silently copied nothing
    # (the [[ -d ]] guard below just skipped it) -- caught by the first
    # real macOS CI run reaching an actual "SDL.h: file not found" instead
    # of a missing-directory warning, since the include path itself
    # (added in cmake/OpenMSXRuntime.cmake) was already correct.
    #
    # Copied rather than pointed at in place so cmake/OpenMSXRuntime.cmake's
    # include-path logic has one stable location regardless of the
    # derived/<platform> directory name.
    rm -rf "${WORK_OUT}/include/3rdparty"
    mkdir -p "${WORK_OUT}/include/3rdparty"
    if [[ -d "${STAGE_DIR}/${DERIVED_DIR}/3rdparty/install/include" ]]; then
        cp -a "${STAGE_DIR}/${DERIVED_DIR}/3rdparty/install/include/." \
              "${WORK_OUT}/include/3rdparty/"
    fi
fi

# Headers: cmake/OpenMSXRuntime.cmake points msxsession straight at the
# staged tree (core/openmsx-generated/src -- the same headers these objects
# were compiled against, so there is no separate copy that could drift), by
# globbing its subdirectories at *configure* time. Only the generated config
# headers are genuinely build-time output (platform+flavour specific) and
# copied here.
cp -a "${STAGE_DIR}/${DERIVED_DIR}/config/." "${WORK_OUT}/include/openmsx-config/"

# Runtime share tree: openMSX boots by executing <systemdatadir>/init.tcl,
# which pulls in share/scripts/*; a subset fails with "Couldn't find
# init.tcl". C-BIOS (freely redistributable, see COMPLIANCE.md) overlays the
# machine configs + ROMs so MSX/MSX2/MSX2+ boot with no manufacturer
# firmware, and the FujiNet extension + fujinet-config.rom are what the
# FujiNet cartridge device needs to be selectable at boot.
rm -rf "${WORK_OUT}/share"
cp -a "${STAGE_DIR}/share" "${WORK_OUT}/share"
cp -a "${STAGE_DIR}/Contrib/cbios/." "${WORK_OUT}/share/machines/"
[[ -f "${WORK_OUT}/share/extensions/FujiNet.xml" ]] || fail \
    "FujiNet.xml missing under ${STAGE_DIR}/share/extensions (openMSX source not the FujiNetWIFI fork?)"
[[ -f "${WORK_OUT}/share/extensions/fujinet-config.rom" ]] || fail \
    "fujinet-config.rom missing under ${STAGE_DIR}/share/extensions"

echo "openMSX build complete:"
echo "  lib     : ${WORK_OUT}/lib"
echo "  include : ${WORK_OUT}/include"
echo "  share   : ${WORK_OUT}/share"
