# Provide the openMSX sources (see cmake/Dependencies.cmake) and stage the
# subset we build into core/openmsx-generated, which
# tools/openmsx/build-openmsx-desktop.sh compiles with openMSX's own
# GNUmakefile.
#
# Unlike XRoar/AppleWin/adamcore in the sibling repos, our own CMake does not
# compile the staged tree directly -- openMSX is 570 C++23 translation units
# with generated config headers and a real 3rd-party dependency chain (Tcl,
# SDL2, SDL2_ttf, freetype, libpng, zlib, ogg/vorbis/theora, GLEW) that
# openMSX's own build/3rdparty chain already solves per-platform. See
# COMPLIANCE.md and TODO for the reasoning. This module only stages and
# patches; cmake/OpenMSXRuntime.cmake drives the actual (heavy, slow) build
# through bash, the same way cmake/FujiNetRuntime.cmake drives FujiNet's.
#
# The copy is done in CMake rather than by a shell script: MSYS2/MinGW
# configures cannot execute a .sh through execute_process ("inappropriate
# file type or format"). The source patches live in
# tools/openmsx/patches/patch-staged-tree.py because a native Windows python
# IS directly invocable where a .sh is not.
#
# Staging is automatic: it runs when the staged tree is missing, when the
# source has moved, when the patch script changes, or on demand with
# -DOPENMSX_RESTAGE=ON (which is also how to pick up uncommitted edits in a
# working checkout pointed at by OPENMSX_SRC).

set(OPENMSX_GEN "${CMAKE_SOURCE_DIR}/core/openmsx-generated")
set(OPENMSX_PATCH_SCRIPT
    "${CMAKE_SOURCE_DIR}/tools/openmsx/patches/patch-staged-tree.py")

option(OPENMSX_RESTAGE "Re-stage openMSX sources from the source checkout" OFF)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

msx_provide_dependency(
  NAME openMSX
  PATH third_party/openmsx
  URL "${OPENMSX_URL}"
  COMMIT "${OPENMSX_COMMIT}"
  SENTINEL src/main.cc
  OVERRIDE OPENMSX_SRC
  RESULT OPENMSX_DIR)

# What the staged tree was made from: the source identity plus a hash of the
# thing that transforms it, so that editing the patch script re-stages
# rather than silently leaving the old result in place.
set(_openmsx_head "")
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${OPENMSX_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE _openmsx_head OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT _openmsx_head)
  set(_openmsx_head "unknown ${OPENMSX_DIR}")
endif()
file(SHA256 "${OPENMSX_PATCH_SCRIPT}" _openmsx_patch_hash)
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _openmsx_stage_hash)
set(_openmsx_want
    "${_openmsx_head} ${_openmsx_patch_hash} ${_openmsx_stage_hash}")

set(_openmsx_staged "")
if(EXISTS "${OPENMSX_GEN}/.source-info")
  file(READ "${OPENMSX_GEN}/.source-info" _openmsx_staged)
  string(STRIP "${_openmsx_staged}" _openmsx_staged)
endif()

set(_openmsx_sentinel "${OPENMSX_GEN}/src/main.cc")

if(OPENMSX_RESTAGE OR NOT EXISTS "${_openmsx_sentinel}"
   OR NOT _openmsx_staged STREQUAL _openmsx_want)
  message(STATUS "Staging openMSX sources from ${OPENMSX_DIR}")
  file(REMOVE_RECURSE "${OPENMSX_GEN}")
  file(MAKE_DIRECTORY "${OPENMSX_GEN}")

  # Stage exactly what openMSX's own build + our runtime need: GNUmakefile +
  # build/ (the make chain, incl. its 3rdparty downloader), src/ (the
  # emulator), share/ (init.tcl + scripts/ + machines/ + extensions/ --
  # openMSX will not boot without this bundled alongside the binary), and
  # Contrib/cbios (the freely redistributable BIOS this app bundles -- see
  # COMPLIANCE.md). Not staged: doc/, meson*.build (we build via GNUmakefile,
  # not meson), configure (autotools, unused), derived/ (openMSX's own build
  # output directory, regenerated fresh here).
  foreach(_d src build share Contrib/cbios)
    if(NOT IS_DIRECTORY "${OPENMSX_DIR}/${_d}")
      message(FATAL_ERROR "openMSX source ${OPENMSX_DIR} is missing ${_d}/")
    endif()
  endforeach()
  if(NOT EXISTS "${OPENMSX_DIR}/GNUmakefile")
    message(FATAL_ERROR "openMSX source ${OPENMSX_DIR} is missing GNUmakefile")
  endif()

  file(COPY "${OPENMSX_DIR}/GNUmakefile" DESTINATION "${OPENMSX_GEN}")
  file(COPY "${OPENMSX_DIR}/src" "${OPENMSX_DIR}/build" "${OPENMSX_DIR}/share"
       DESTINATION "${OPENMSX_GEN}"
       PATTERN ".git" EXCLUDE
       PATTERN "derived" EXCLUDE
       REGEX "\\.(o|a|so|d|lo|la)$" EXCLUDE)
  file(MAKE_DIRECTORY "${OPENMSX_GEN}/Contrib")
  file(COPY "${OPENMSX_DIR}/Contrib/cbios"
       DESTINATION "${OPENMSX_GEN}/Contrib")

  if(NOT EXISTS "${_openmsx_sentinel}")
    message(FATAL_ERROR "openMSX staging failed (source: ${OPENMSX_DIR})")
  endif()

  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${OPENMSX_PATCH_SCRIPT}" "${OPENMSX_GEN}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    # Leave nothing half-patched behind: the next configure must re-stage
    # from pristine sources rather than trying to patch a partial tree.
    file(REMOVE_RECURSE "${OPENMSX_GEN}")
    message(FATAL_ERROR
      "openMSX staging patches failed. The pinned commit "
      "(${OPENMSX_COMMIT}) and the anchors in\n  ${OPENMSX_PATCH_SCRIPT}\n"
      "have to agree; a source checkout of a different commit is the usual "
      "cause.\n${_out}${_err}")
  endif()

  file(WRITE "${OPENMSX_GEN}/.source-info" "${_openmsx_want}\n")
endif()

message(STATUS "openMSX staged from ${OPENMSX_DIR}")
