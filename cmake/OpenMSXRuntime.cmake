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

# The include path list is written by build-openmsx-desktop.sh from openMSX's
# own SOURCE_DIRS logic (every subdirectory of src, sorted) so a future
# openMSX version that adds a subdirectory does not silently break the build.
# It only exists after the first build, which is fine: this file is read at
# *generate* time for a target that already depends on openmsx-build, and a
# from-scratch configure re-runs generation once the custom command has run.
set(OPENMSX_INCLUDE_DIRS "")
if(EXISTS "${OPENMSX_OUT}/include-dirs.txt")
  file(STRINGS "${OPENMSX_OUT}/include-dirs.txt" _openmsx_subdirs)
  foreach(_d IN LISTS _openmsx_subdirs)
    list(APPEND OPENMSX_INCLUDE_DIRS "${OPENMSX_OUT}/include/openmsx/${_d}")
  endforeach()
endif()
list(APPEND OPENMSX_INCLUDE_DIRS
     "${OPENMSX_OUT}/include/openmsx"
     "${OPENMSX_OUT}/include/openmsx-config")

# CMake fatally errors on an IMPORTED target's INTERFACE_INCLUDE_DIRECTORIES
# containing a path that does not yet exist. Before the first build these
# directories are exactly that (build-openmsx-desktop.sh creates them), so
# create them empty now -- the standard ExternalProject_Add-style workaround.
# The real headers land inside them once the custom command below runs.
foreach(_d IN LISTS OPENMSX_INCLUDE_DIRS)
  file(MAKE_DIRECTORY "${_d}")
endforeach()

add_library(openmsx-lib STATIC IMPORTED GLOBAL)
set_target_properties(openmsx-lib PROPERTIES
  IMPORTED_LOCATION "${OPENMSX_LIB}"
  INTERFACE_INCLUDE_DIRECTORIES "${OPENMSX_INCLUDE_DIRS}")
add_dependencies(openmsx-lib openmsx-build)

# 3rd-party static archives (macOS/Windows only -- see build-openmsx-desktop.sh).
# Discovered at generate time from whatever the build actually produced, since
# the exact archive set depends on which openMSX version's 3rdparty.mk ran.
file(GLOB OPENMSX_3RDPARTY_LIBS "${OPENMSX_OUT}/lib/*.a")
list(REMOVE_ITEM OPENMSX_3RDPARTY_LIBS "${OPENMSX_LIB}")
if(OPENMSX_3RDPARTY_LIBS)
  set_property(TARGET openmsx-lib APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES "${OPENMSX_3RDPARTY_LIBS}")
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
  set_property(TARGET openmsx-lib APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES PkgConfig::OPENMSX_SYS_DEPS "${OPENMSX_TCL_LIBRARY}")
endif()
