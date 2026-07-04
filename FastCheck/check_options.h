#pragma once

// FastCheck CLI 选项与退出码（fastcheck）。独立于 sync 的 CliOptions；退出码 0-4 为 check 专用，
// 一值一义（M12/FR-12/13/14/15），脚本仅凭退出码即可分支。

#include <cstdint>
#include <string>

namespace fc::check {

// 比对模式（FR-05）。默认 Fast。
enum class Mode { Fast, Strict, SizeOnly };

// 报告格式（FR-07）。默认 Text。
enum class Format { Text, Json };

// 逐文件清单过滤位集（FR-10）。默认列出 DIFF/MISSING/EXTRA，不列 SAME。
struct FilterSet {
    bool diff = true;
    bool missing = true;
    bool extra = true;
    bool same = false;
};

struct CheckOptions {
    std::string server;         // host（不含端口），必填（FR-04）
    uint16_t port = 27842;      // 默认端口，与主程序一致
    std::string target;         // 本地目录，必填（FR-04）
    std::string password;       // 必填（FR-04）
    Mode mode = Mode::Fast;
    uint32_t checkers = 8;      // --checkers：单连接内在飞 HashRequest 上限，正整数（FR-06）
    std::string output;         // 空=仅终端（FR-08）
    Format format = Format::Text;
    bool summaryOnly = false;   // FR-09
    FilterSet filter;           // FR-10
};

// check 专用退出码（M12）。不复用 sync 的 kExit*。
enum ExitCode {
    kIdentical = 0,           // 完整比对：两端一致
    kDiffFound = 1,           // 完整比对：存在差异
    kConnFailed = 2,          // 连接/握手/认证失败、比对中途断连、参数错误
    kLocalPrecondFailed = 3,  // 本地 target/output 前置条件失败、本地文件读失败
    kInterrupted = 4          // 用户中断（Ctrl+C），报告标 [PARTIAL]
};

}  // namespace fc::check
