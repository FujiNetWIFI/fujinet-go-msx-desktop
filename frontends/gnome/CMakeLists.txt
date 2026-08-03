# GNOME frontend: GTK4 + libadwaita. Found-or-skipped so a lean machine can
# still build the core / other frontends.
#
# M2 scope: no embedded FujiNet web view yet (WITH_WEBVIEW / WebKitGTK land
# with the FujiNet runtime in M3). Debugger window added M6.

find_package(PkgConfig REQUIRED)
pkg_check_modules(ADW IMPORTED_TARGET libadwaita-1>=1.4 gtk4>=4.10)

if(NOT ADW_FOUND)
  message(STATUS "GNOME frontend: libadwaita/gtk4 not found -- skipping")
  return()
endif()

set(GNOME_SOURCES
  main.c
  window.c
  display.c
  debugger/dbg_window.c
)

set(GNOME_APP_ID online.fujinet.go.msx.gnome)

add_executable(fujinet-go-msx-gnome ${GNOME_SOURCES})
target_link_libraries(fujinet-go-msx-gnome PRIVATE msxsession PkgConfig::ADW)
target_compile_options(fujinet-go-msx-gnome PRIVATE -Wall -Wextra)
target_compile_definitions(fujinet-go-msx-gnome PRIVATE
  MSX_APP_ID="${GNOME_APP_ID}"
  # Uninstalled runs fall back to the in-tree icon theme (msx_icon_name).
  MSX_SOURCE_ICON_DIR="${CMAKE_SOURCE_DIR}/data/icons")

install(TARGETS fujinet-go-msx-gnome RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
