#pragma once

#include <filesystem>

namespace fc {

inline bool IsPathUnderRoot(const std::filesystem::path& root, const std::filesystem::path& path) {
    auto rootIt = root.begin();
    auto pathIt = path.begin();
    while (rootIt != root.end() && pathIt != path.end()) {
        if (*rootIt != *pathIt) {
            return false;
        }
        ++rootIt;
        ++pathIt;
    }
    return rootIt == root.end();
}

}  // namespace fc
