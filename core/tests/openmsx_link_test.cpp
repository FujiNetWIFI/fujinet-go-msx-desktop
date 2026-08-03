// M1 gate: proves libopenmsx.a (tools/openmsx/build-openmsx-desktop.sh's
// output) actually links and its symbols resolve. Superseded once
// core/openmsx/msx_host.cc exists in M2, but kept as the cheapest possible
// "did the openMSX build produce something real" check.

#include <cstdio>

#include "Version.hh"

int main() {
    std::printf("%s\n", openmsx::Version::full().c_str());
    return 0;
}
