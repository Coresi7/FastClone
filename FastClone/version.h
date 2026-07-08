#pragma once

// Single source of truth for the FastClone / FastCheck version string.
//
// Both the hand-maintained Visual Studio solution (FastClone.slnx) and the CMake
// build consume this header directly: cli.cpp / check_cli.cpp include it for
// `--version`, and CMake parses the string below (regex) for package/install
// metadata. CI additionally asserts this string matches the release git tag
// (`v<x.y.z>`), so bumping the version means editing this one line and pushing
// the matching tag.

namespace fc {

constexpr const char* kFastCloneVersion = "1.0.0";

}  // namespace fc
