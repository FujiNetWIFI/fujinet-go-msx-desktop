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
  src/input_map.c
  openmsx/msx_host.cc
)
target_link_libraries(msxsession PUBLIC openmsx-lib PkgConfig::SDL2 Threads::Threads)
target_include_directories(msxsession
  PUBLIC "${CMAKE_SOURCE_DIR}/core/include"
  PRIVATE "${CMAKE_SOURCE_DIR}/core/src" "${CMAKE_SOURCE_DIR}/core/openmsx")
target_compile_options(msxsession PRIVATE -Wall -Wextra)

# Where the openMSX runtime share tree (init.tcl, C-BIOS machines, the
# FujiNet extension) is found: a dev build's raw script output, and where a
# real install puts it (mirroring the FujiNet runtime's own
# ${CMAKE_INSTALL_DATADIR}/fujinet-go-msx/fujinet layout in
# cmake/FujiNetRuntime.cmake). See session.c's resolve_openmsx_share().
target_compile_definitions(msxsession PRIVATE
  MSX_DEV_OPENMSX_SHARE="${CMAKE_SOURCE_DIR}/tools/openmsx/work/out/share"
  MSX_INSTALL_DATADIR_SHARE="${CMAKE_INSTALL_FULL_DATADIR}/fujinet-go-msx/openmsx")

install(DIRECTORY "${CMAKE_SOURCE_DIR}/tools/openmsx/work/out/share/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/fujinet-go-msx/openmsx"
        OPTIONAL)

# M1 gate, kept as a cheap standalone check that libopenmsx.a's symbols
# resolve independent of the rest of the session (see TODO M1).
add_executable(openmsx_link_test tests/openmsx_link_test.cpp)
target_link_libraries(openmsx_link_test PRIVATE openmsx-lib Threads::Threads)
add_test(NAME openmsx_link_test COMMAND openmsx_link_test)

# M2 gate: boots C-BIOS headless (no window, no toolkit, no FujiNet) and
# confirms real, non-blank, correctly-sized frames arrive -- the test that
# makes a macOS or Windows CI job meaningful when this machine cannot run
# those platforms itself.
add_executable(boot_smoke tests/boot_smoke.c)
target_link_libraries(boot_smoke PRIVATE msxsession)
add_test(NAME boot_smoke COMMAND boot_smoke)
set_tests_properties(boot_smoke PROPERTIES TIMEOUT 60)
