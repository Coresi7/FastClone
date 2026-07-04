#pragma once

// FastCheck 报告渲染（fastcheck，M5/FR-22~24/NFR-05）。text/json 两种格式，终端摘要 + --output
// 落盘。字段名/顺序/类别标签/路径格式/计数口径稳定，后续变更需同步需求、设计与测试。

#include "check_options.h"
#include "compare_phase.h"

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace fc::check {

// 单条逐文件差异记录。path 为相对根目录、正斜杠（FR-02/AC-02）。缺失一侧的 size 为 nullopt，
// json 渲染成 null（FR-24）。
struct DiffEntry {
    fc::CompareCategory type = fc::CompareCategory::Same;
    std::string path;
    std::optional<uint64_t> localSize;
    std::optional<uint64_t> remoteSize;
    bool hashCompared = false;  // 该文件是否实际发过 HashRequest（FR-24/AC-22）
};

struct CheckResult {
    fc::CompareCounters counters;
    Mode mode = Mode::Fast;
    uint64_t durationMs = 0;
    bool partial = false;                 // Ctrl+C / 断连 / 本地读失败（FR-15/23/48）
    std::vector<DiffEntry> entries;       // 经 filter 后需列出的条目（内存随差异线性，NFR-06）
};

// 渲染到任意 ostream（便于单测）。filter 只影响逐文件清单；summaryOnly 时不列逐文件。
void RenderText(std::ostream& os, const CheckResult& r, const FilterSet& f, bool summaryOnly);
void RenderJson(std::ostream& os, const CheckResult& r, const FilterSet& f, bool summaryOnly);

// 终端始终打印终态摘要；传入 --output 时同时把完整报告写文件（FR-08）。写文件失败返回非 0。
int WriteReport(const CheckOptions& o, const CheckResult& r);

}  // namespace fc::check
