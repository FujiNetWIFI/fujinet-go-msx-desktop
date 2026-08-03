# libmsxsession: the toolkit-agnostic session core. Links openmsx-lib (the
# static archive tools/openmsx/build-openmsx-desktop.sh produces from
# openMSX's own build) plus the desktop session layer.

include(GNUInstallDirs)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

enable_testing()

# openMSX links SDL2 (not SDL3, unlike the sibling repos -- see
# COMPLIANCE.md / TODO): this app owns no audio/gamepad backend of its own,
# so SDL2 is needed here only for the event-injection types (SDL_Event,
# SDLK_*) msx_host.cc and input_map.c use to talk to openMSX's own
# EventDistributor.
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

add_library(msxsession STATIC
  src/session.c
  src/paths.c
  src/input_map.c
  src/fujinet_runtime.c
  openmsx/msx_host.cc
)
target_link_libraries(msxsession PUBLIC openmsx-lib PkgConfig::SDL2 Threads::Threads)
target_include_directories(msxsession
  PUBLIC "${CMAKE_SOURCE_DIR}/core/include"
  PRIVATE "${CMAKE_SOURCE_DIR}/core/src" "${CMAKE_SOURCE_DIR}/core/openmsx")
# -Wno-format-truncation: the path layer composes MSX_PATH_MAX-sized buffers
# from other max-sized buffers; snprintf truncation is the intended clamp
# there, not a bug (same reasoning as the sibling repos' path.c).
target_compile_options(msxsession PRIVATE -Wall -Wextra -Wno-format-truncation)
if(WIN32)
  target_link_libraries(msxsession PUBLIC ws2_32)
else()
  target_link_libraries(msxsession PUBLIC dl m)
endif()

# Where the openMSX runtime share tree (init.tcl, C-BIOS machines, the
# FujiNet extension) and the FujiNet runtime (libfujinet.* + fnconfig.ini +
# data/ + SD/) are found: a dev build's raw script output, and where a real
# install puts them (mirroring cmake/FujiNetRuntime.cmake's own
# ${CMAKE_INSTALL_DATADIR}/fujinet-go-msx/fujinet layout). See paths.c.
target_compile_definitions(msxsession PRIVATE
  _GNU_SOURCE
  MSX_DEV_OPENMSX_SHARE="${CMAKE_SOURCE_DIR}/tools/openmsx/work/out/share"
  MSX_INSTALL_DATADIR_SHARE="${CMAKE_INSTALL_FULL_DATADIR}/fujinet-go-msx/openmsx"
  MSX_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out"
  MSX_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/fujinet-go-msx"
  MSX_INSTALL_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/fujinet-go-msx")

install(DIRECTORY "${CMAKE_SOURCE_DIR}/tools/openmsx/work/out/share/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/fujinet-go-msx/openmsx"
        OPTIONAL)

# M1 gate, kept as a cheap standalone check that libopenmsx.a's symbols
# resolve independent of the rest of the session (see TODO M1).
add_executable(openmsx_link_test tests/openmsx_link_test.cpp)
target_link_libraries(openmsx_link_test PRIVATE openmsx-lib Threads::Threads)
add_test(NAME openmsx_link_test COMMAND openmsx_link_test)

# M2 gate: boots C-BIOS headless (no window, no toolkit) and confirms real,
# non-blank, correctly-sized frames arrive -- the test that makes a macOS or
# Windows CI job meaningful when this machine cannot run those platforms
# itself. Runs with FujiNet enabled (msxsession_default_opts' default) too,
# now that M3 exists, so it doubles as a basic FujiNet-present boot check;
# fujinet_smoke below is the one that actually proves the BOIP link comes up.
add_executable(boot_smoke tests/boot_smoke.c)
target_link_libraries(boot_smoke PRIVATE msxsession)
add_test(NAME boot_smoke COMMAND boot_smoke)
set_tests_properties(boot_smoke PROPERTIES TIMEOUT 60)

# M3 gate: boots openMSX with FujiNet enabled and confirms a real TCP
# connection comes up on MSXSESSION_BOIP_PORT -- proof the whole chain
# (session -> FujiNet dlopen -> BOIP listener -> openMSX's FujiNet
# cartridge device connecting out) works headless, without needing a
# display or a human to press a key and look at a screen. Exits 77 (ctest
# SKIP) when the FujiNet runtime isn't available, same shape as boot_smoke
# skipping on a ROM-less build -- see fujinet_smoke.c.
#
# NOT gated on `if(WITH_FUJINET)` here: that option is declared in
# cmake/FujiNetRuntime.cmake, which the top-level CMakeLists.txt includes
# *after* add_subdirectory(core) (so the FujiNet runtime can depend on
# targets core/ defines) -- so WITH_FUJINET does not exist yet at this
# point in the tree and reads as unset/false, silently skipping this
# test's registration even on a WITH_FUJINET=ON build. A real clean-clone
# test run caught this; CoCo's sibling repo already avoids it by never
# conditioning its own drivewire_smoke registration this way either.
add_executable(fujinet_smoke tests/fujinet_smoke.c)
target_link_libraries(fujinet_smoke PRIVATE msxsession)
add_test(NAME fujinet_smoke COMMAND fujinet_smoke)
set_tests_properties(fujinet_smoke PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
# Forward-references the target cmake/FujiNetRuntime.cmake defines later in
# this same configure pass; valid because add_dependencies() only needs the
# named target to exist by *generate* time, not at the point of this call.
add_dependencies(fujinet_smoke fujinet-runtime)
