# libmsxsession: the toolkit-agnostic session core. Links openmsx-lib (the
# static archive tools/openmsx/build-openmsx-desktop.sh produces from
# openMSX's own build) plus the desktop session/FujiNet/debugger layers.
#
# M1 state: only the openMSX link-smoke-test exists. core/openmsx/msx_host,
# core/src/session.c and the msxsession.h contract land in M2.

include(GNUInstallDirs)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

enable_testing()

# M1 gate (see TODO): libopenmsx.a actually links and its symbols resolve,
# from a clean clone with no OPENMSX_SRC override. This links a real openMSX
# symbol (Version::full(), chosen for having no further dependencies) rather
# than just compiling an empty translation unit against the headers, so a
# link-only build tells the real story.
add_executable(openmsx_link_test tests/openmsx_link_test.cpp)
target_link_libraries(openmsx_link_test PRIVATE openmsx-lib Threads::Threads)
add_test(NAME openmsx_link_test COMMAND openmsx_link_test)
