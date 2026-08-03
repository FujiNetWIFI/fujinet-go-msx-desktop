# macOS frontend: native AppKit (Objective-C) over the shared session core.
# Built only on Apple hosts; verified by the macOS CI job.

if(NOT APPLE)
  return()
endif()

enable_language(OBJC)

# Same rendered art as the other frontends (tools/icons/make-icons.py), just
# packed as .icns instead of hicolor PNGs. Listing it as a source with a
# MACOSX_PACKAGE_LOCATION is what makes CMake copy it into Contents/Resources
# for both the Xcode and Ninja/Makefiles generators.
set(MACOS_ICON "${CMAKE_SOURCE_DIR}/data/icons/fujinet-go-msx.icns")
set_source_files_properties(${MACOS_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

add_executable(fujinet-go-msx-macos MACOSX_BUNDLE
  main.m
  AppDelegate.m
  DisplayView.m
  debugger/DebuggerWindow.m
  ${MACOS_ICON}
)
set_target_properties(fujinet-go-msx-macos PROPERTIES
  OUTPUT_NAME "FujiNet Go MSX"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  MACOSX_BUNDLE_ICON_FILE "fujinet-go-msx.icns"
)
target_link_libraries(fujinet-go-msx-macos PRIVATE
  msxsession
  "-framework Cocoa"
  "-framework CoreVideo"
  "-framework QuartzCore"
  "-framework UniformTypeIdentifiers"
)
target_compile_options(fujinet-go-msx-macos PRIVATE -Wall -Wextra -fobjc-arc)

# Pack the openMSX runtime share tree (init.tcl, scripts/, machines/ incl.
# C-BIOS, extensions/ incl. FujiNet.xml) into Contents/Resources/openmsx --
# main.m looks for it there. Built by cmake/OpenMSXRuntime.cmake (the
# openmsx-build target), so wait for it.
add_dependencies(fujinet-go-msx-macos openmsx-build)
add_custom_command(TARGET fujinet-go-msx-macos POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_SOURCE_DIR}/tools/openmsx/work/out/share"
          "$<TARGET_BUNDLE_CONTENT_DIR:fujinet-go-msx-macos>/Resources/openmsx"
  COMMENT "Bundling the openMSX runtime share tree into the app"
)

# Pack the FujiNet runtime into the bundle: the dylib into
# Contents/Frameworks, the pristine runtime tree into
# Contents/Resources/fujinet (main.m hands both paths to the session). The
# runtime is built by this same build (cmake/FujiNetRuntime.cmake), so wait
# for it; a tree left by an earlier manual run of the script is used as-is.
if(TARGET fujinet-runtime)
  add_dependencies(fujinet-go-msx-macos fujinet-runtime)
endif()
if(TARGET fujinet-runtime OR EXISTS "${FUJINET_OUT}/libfujinet.dylib")
  add_custom_command(TARGET fujinet-go-msx-macos POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:fujinet-go-msx-macos>/Frameworks"
    COMMAND ${CMAKE_COMMAND} -E copy "${FUJINET_OUT}/libfujinet.dylib"
            "$<TARGET_BUNDLE_CONTENT_DIR:fujinet-go-msx-macos>/Frameworks/"
    COMMAND ${CMAKE_COMMAND} -E copy "${FUJINET_OUT}/fnconfig.ini"
            "$<TARGET_BUNDLE_CONTENT_DIR:fujinet-go-msx-macos>/Resources/fujinet/fnconfig.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${FUJINET_OUT}/data"
            "$<TARGET_BUNDLE_CONTENT_DIR:fujinet-go-msx-macos>/Resources/fujinet/data"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${FUJINET_OUT}/SD"
            "$<TARGET_BUNDLE_CONTENT_DIR:fujinet-go-msx-macos>/Resources/fujinet/SD"
    COMMENT "Bundling the FujiNet runtime into the app"
  )
else()
  message(STATUS "macOS frontend: FujiNet runtime disabled; the app will run without FujiNet")
endif()

install(TARGETS fujinet-go-msx-macos BUNDLE DESTINATION .)
