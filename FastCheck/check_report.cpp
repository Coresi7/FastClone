#include "check_report.h"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace fc::check {

namespace {

const char* ModeString(Mode mode) {
    switch (mode) {
        case Mode::Strict:
            return "strict";
        case Mode::SizeOnly:
            return "size-only";
        case Mode::Fast:
        default:
            return "fast";
    }
}

// text 逐文件行的定宽类别标签。
const char* CategoryLabel(fc::CompareCategory cat) {
    switch (cat) {
        case fc::CompareCategory::Diff:
            return "[DIFF]   ";
        case fc::CompareCategory::Missing:
            return "[MISSING]";
        case fc::CompareCategory::Extra:
            return "[EXTRA]  ";
        case fc::CompareCategory::Same:
        default:
            return "[SAME]   ";
    }
}

// json type 字段（大写类别名）。
const char* CategoryJson(fc::CompareCategory cat) {
    switch (cat) {
        case fc::CompareCategory::Diff:
            return "DIFF";
        case fc::CompareCategory::Missing:
            return "MISSING";
        case fc::CompareCategory::Extra:
            return "EXTRA";
        case fc::CompareCategory::Same:
        default:
            return "SAME";
    }
}

// 该类别是否应出现在逐文件清单（按 filter）。
bool PassesFilter(fc::CompareCategory cat, const FilterSet& f) {
    switch (cat) {
        case fc::CompareCategory::Diff:
            return f.diff;
        case fc::CompareCategory::Missing:
            return f.missing;
        case fc::CompareCategory::Extra:
            return f.extra;
        case fc::CompareCategory::Same:
        default:
            return f.same;
    }
}

// 最小 JSON 字符串转义（引号/反斜杠/控制字符）。路径为正斜杠，反斜杠罕见但仍需安全。
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (const char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                    out += kHex[static_cast<unsigned char>(c) & 0xF];
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void RenderSummaryLineText(std::ostream& os, const CheckResult& r) {
    const fc::CompareCounters& c = r.counters;
    // NFR-07：partial 绝不输出「比对完成」成功语义。
    if (r.partial) {
        os << "[PARTIAL] Check incomplete.";
    } else {
        os << "Check completed.";
    }
    os << " same=" << c.same << " diff=" << c.diff << " missing=" << c.missing
       << " extra_local=" << c.extraLocal << " total=" << c.TotalCompared()
       << " mode=" << ModeString(r.mode) << " duration_ms=" << r.durationMs << "\n";
}

}  // namespace

void RenderText(std::ostream& os, const CheckResult& r, const FilterSet& f, bool summaryOnly) {
    RenderSummaryLineText(os, r);
    if (summaryOnly) {
        return;
    }
    for (const DiffEntry& e : r.entries) {
        if (!PassesFilter(e.type, f)) {
            continue;
        }
        os << CategoryLabel(e.type) << " " << e.path;
        if (e.type == fc::CompareCategory::Diff) {
            os << "  local=" << (e.localSize ? std::to_string(*e.localSize) : "-")
               << " remote=" << (e.remoteSize ? std::to_string(*e.remoteSize) : "-");
        }
        os << "\n";
    }
}

void RenderJson(std::ostream& os, const CheckResult& r, const FilterSet& f, bool summaryOnly) {
    const fc::CompareCounters& c = r.counters;
    os << "{\n";
    os << "  \"summary\": {\n";
    os << "    \"same\": " << c.same << ",\n";
    os << "    \"diff\": " << c.diff << ",\n";
    os << "    \"missing\": " << c.missing << ",\n";
    os << "    \"extra_local\": " << c.extraLocal << ",\n";
    os << "    \"total_compared\": " << c.TotalCompared() << ",\n";
    os << "    \"mode\": \"" << ModeString(r.mode) << "\",\n";
    os << "    \"duration_ms\": " << r.durationMs << ",\n";
    os << "    \"partial\": " << (r.partial ? "true" : "false") << "\n";
    os << "  },\n";
    os << "  \"differences\": [";
    bool first = true;
    if (!summaryOnly) {
        for (const DiffEntry& e : r.entries) {
            if (!PassesFilter(e.type, f)) {
                continue;
            }
            os << (first ? "\n" : ",\n");
            first = false;
            os << "    {";
            os << "\"type\": \"" << CategoryJson(e.type) << "\", ";
            os << "\"path\": \"" << JsonEscape(e.path) << "\", ";
            os << "\"local_size\": " << (e.localSize ? std::to_string(*e.localSize) : "null") << ", ";
            os << "\"remote_size\": " << (e.remoteSize ? std::to_string(*e.remoteSize) : "null") << ", ";
            os << "\"hash_compared\": " << (e.hashCompared ? "true" : "false");
            os << "}";
        }
    }
    os << (first ? "]\n" : "\n  ]\n");
    os << "}\n";
}

int WriteReport(const CheckOptions& o, const CheckResult& r) {
    if (o.output.empty()) {
        // 仅终端：按所选格式输出完整报告。
        if (o.format == Format::Json) {
            RenderJson(std::cout, r, o.filter, o.summaryOnly);
        } else {
            RenderText(std::cout, r, o.filter, o.summaryOnly);
        }
        return 0;
    }
    // --output：终端仅输出终态摘要行；完整报告写文件（FR-08）。
    RenderText(std::cout, r, o.filter, /*summaryOnly=*/true);
    std::ofstream file(std::filesystem::path(o.output), std::ios::binary | std::ios::trunc);
    if (!file) {
        std::cerr << "error: cannot open --output file for writing: " << o.output << std::endl;
        return 1;
    }
    if (o.format == Format::Json) {
        RenderJson(file, r, o.filter, o.summaryOnly);
    } else {
        RenderText(file, r, o.filter, o.summaryOnly);
    }
    return file.good() ? 0 : 1;
}

}  // namespace fc::check
