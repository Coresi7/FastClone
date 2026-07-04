#pragma once

// FastCheck report rendering (fastcheck, M5/FR-22~24/NFR-05). Two formats, text/json, terminal summary + --output
// to disk. Field names/order/category labels/path format/counting basis are stable; later changes must be synced across requirements, design and tests.

#include "check_options.h"
#include "compare_phase.h"

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace fc::check {

// One per-file difference record. path is relative to the root, forward slashes (FR-02/AC-02). The missing side's size is nullopt,
// rendered as null in json (FR-24).
struct DiffEntry {
    fc::CompareCategory type = fc::CompareCategory::Same;
    std::string path;
    std::optional<uint64_t> localSize;
    std::optional<uint64_t> remoteSize;
    bool hashCompared = false;  // whether this file actually sent a HashRequest (FR-24/AC-22)
};

struct CheckResult {
    fc::CompareCounters counters;
    Mode mode = Mode::Fast;
    uint64_t durationMs = 0;
    bool partial = false;                 // Ctrl+C / disconnect / local read failure (FR-15/23/48)
    std::vector<DiffEntry> entries;       // entries to list after filtering (memory linear in differences, NFR-06)
};

// Render to any ostream (for unit testing). filter only affects the per-file listing; when summaryOnly, per-file entries are not listed.
void RenderText(std::ostream& os, const CheckResult& r, const FilterSet& f, bool summaryOnly);
void RenderJson(std::ostream& os, const CheckResult& r, const FilterSet& f, bool summaryOnly);

// The terminal always prints the final summary; when --output is passed, the full report is also written to the file (FR-08). Returns non-zero on file write failure.
int WriteReport(const CheckOptions& o, const CheckResult& r);

}  // namespace fc::check
