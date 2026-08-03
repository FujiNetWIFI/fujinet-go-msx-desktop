# External source dependencies.
#
# Standard practice for every target: the build provides each dependency for
# itself. A plain `git clone` of this repository (no --recurse-submodules),
# GNOME Builder, a flatpak build or a source tarball all end up with a usable
# checkout without the developer having to know the dependency exists.
#
# Two dependencies, both pinned git checkouts (unlike CoCo's XRoar, neither
# ships as a tarball -- both are live forks with git history worth having).
# Neither is registered as a real git submodule (no .gitmodules): CMake
# clones the pin into third_party/<name> itself the first time it is needed,
# which sidesteps the usual submodule footguns (a plain `git clone` with no
# --recurse-submodules still works, there is no gitlink SHA to go stale
# relative to a commit here, and there is no separate `git submodule update`
# step to forget). third_party/ is entirely gitignored.
#
#   openMSX             third_party/openmsx. The FujiNetWIFI fork (branch
#                        feat/fujinet), which adds the FujiNet cartridge
#                        device.
#   fujinet-firmware     third_party/fujinet-firmware.
#
# Resolution order for each dependency:
#   1. <NAME>_SRC (cache variable or environment) -- an out-of-tree working
#      checkout, which is how the dependencies are developed in tandem, e.g.
#      cmake -B build -DOPENMSX_SRC=~/Workspace/openMSX
#   2. third_party/<name>, cloned fresh if not already present.
#   3. (Kept for parity with the other family members' dependency function,
#      in case a submodule is ever registered here: initialising it when this
#      tree is a git checkout with a matching .gitmodules entry.)

find_package(Git QUIET)

# openMSX -- FujiNetWIFI's fork of the openMSX emulator, branch feat/fujinet,
# which adds the FujiNet cartridge device (src/serial/FujiNet.cc,
# FujiBusPacket). GPL-2.0-or-later; see COMPLIANCE.md for the licence
# determination.
set(OPENMSX_COMMIT "b11b550120fe9d342f9bd64128de7bd06e1a7758")
set(OPENMSX_URL "https://github.com/FujiNetWIFI/openMSX")

set(FUJINET_COMMIT "b9a27cfa145134f5a7088535ac2721eb3cdf41be")
set(FUJINET_URL "https://github.com/FujiNetWIFI/fujinet-firmware")

# msx_provide_dependency(NAME <n> PATH <p> SENTINEL <file> OVERRIDE <VAR>
#                        RESULT <out> URL <u> COMMIT <sha>)
#
# SENTINEL is a path inside the checkout that only exists once the sources are
# really there -- an empty submodule directory is otherwise indistinguishable
# from a populated one.
function(msx_provide_dependency)
  cmake_parse_arguments(DEP ""
    "NAME;PATH;URL;COMMIT;SENTINEL;OVERRIDE;RESULT" ""
    ${ARGN})

  # 1. Explicit override: a checkout the developer maintains themselves.
  set(_override "")
  if(DEFINED ${DEP_OVERRIDE})
    set(_override "${${DEP_OVERRIDE}}")
  elseif(DEFINED ENV{${DEP_OVERRIDE}})
    set(_override "$ENV{${DEP_OVERRIDE}}")
  endif()
  if(_override)
    if(NOT EXISTS "${_override}/${DEP_SENTINEL}")
      message(FATAL_ERROR
        "${DEP_OVERRIDE}=${_override} does not look like a ${DEP_NAME} "
        "checkout (no ${DEP_SENTINEL}).")
    endif()
    message(STATUS "${DEP_NAME}: using ${_override} (${DEP_OVERRIDE})")
    set(${DEP_RESULT} "${_override}" PARENT_SCOPE)
    return()
  endif()

  set(_path "${CMAKE_SOURCE_DIR}/${DEP_PATH}")

  if(NOT EXISTS "${_path}/${DEP_SENTINEL}")
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
      message(FATAL_ERROR
        "${DEP_NAME} is missing and git is not installed. Either install git "
        "or unpack ${DEP_URL} (commit ${DEP_COMMIT}) into ${DEP_PATH}.")
    endif()

    if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
      # 2. Submodule checkout. --filter=blob:none keeps the fetch to the
      # history the build needs; both dependencies are large repositories.
      message(STATUS "${DEP_NAME}: fetching submodule ${DEP_PATH}")
      execute_process(
        COMMAND ${GIT_EXECUTABLE} submodule update --init --filter=blob:none
                -- "${DEP_PATH}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _rc)
      if(NOT _rc EQUAL 0)
        # Older servers/mirrors may refuse partial clones.
        execute_process(
          COMMAND ${GIT_EXECUTABLE} submodule update --init -- "${DEP_PATH}"
          WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
          RESULT_VARIABLE _rc)
      endif()
    endif()

    if(NOT EXISTS "${_path}/${DEP_SENTINEL}")
      # 3. No git metadata (e.g. a source tarball with no .git): clone the
      # pin outright.
      message(STATUS "${DEP_NAME}: cloning ${DEP_URL} @ ${DEP_COMMIT}")
      file(REMOVE_RECURSE "${_path}")
      execute_process(
        COMMAND ${GIT_EXECUTABLE} clone --filter=blob:none "${DEP_URL}" "${_path}"
        RESULT_VARIABLE _rc)
      if(_rc EQUAL 0)
        execute_process(
          COMMAND ${GIT_EXECUTABLE} -c advice.detachedHead=false checkout
                  --quiet "${DEP_COMMIT}"
          WORKING_DIRECTORY "${_path}"
          RESULT_VARIABLE _rc)
      endif()
    endif()
  endif()

  if(NOT EXISTS "${_path}/${DEP_SENTINEL}")
    message(FATAL_ERROR
      "Could not provide ${DEP_NAME}. Fetch it manually with\n"
      "    git submodule update --init ${DEP_PATH}\n"
      "or point ${DEP_OVERRIDE} at an existing checkout.")
  endif()

  # Warn when a git checkout has drifted from the pin recorded here -- the
  # FujiNet source patches and the openMSX staged-source patches are anchored
  # to exact text and fail confusingly against a drifted tree.
  if(DEP_COMMIT AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    find_package(Git QUIET)
    if(GIT_FOUND)
      execute_process(
        COMMAND ${GIT_EXECUTABLE} -C "${_path}" rev-parse HEAD
        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _rc)
      if(_rc EQUAL 0 AND NOT _head STREQUAL DEP_COMMIT)
        message(STATUS
          "${DEP_NAME}: checkout is ${_head}, pinned ${DEP_COMMIT} "
          "(cmake/Dependencies.cmake)")
      endif()
    endif()
  endif()

  message(STATUS "${DEP_NAME}: ${_path}")
  set(${DEP_RESULT} "${_path}" PARENT_SCOPE)
endfunction()
