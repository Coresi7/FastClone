#pragma once

// Single source of truth for the FastClone / FastCheck version string.
//
// The CMake build consumes this header directly: at configure time it parses the
// string below (regex) to derive the base version, auto-increments the patch field
// using a persistent counter (`.fcversion`, gitignored) or a git-derived count on a
// clean CI checkout, and writes the new value back here. cli.cpp / check_cli.cpp
// include this header for `--version`. This is the ONLY place the version is authored.
//
// The release workflow's drift guard asserts that the built version's MAJOR.MINOR
// agrees with the release git tag (the patch field may differ, since it is
// auto-incremented at build time). So a release requires the major/minor in this
// string to match the tag; the patch is managed automatically.

namespace fc {

constexpr const char* kFastCloneVersion = "1.0.0";

}  // namespace fc
