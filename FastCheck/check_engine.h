#pragma once

// FastCheck 单连接轻编排引擎（fastcheck，M7/M11/FR-27/28）。握手后：请求 manifest → 逐帧分派
// → 按模式判定 → 需要时流水线化 HashRequest（在飞上限=--checkers）→ 枚举本地多余项 → 组装报告。
// 不含任何 sync 多 lane/传输队列/delta/reconnect 逻辑。网络 I/O 经可注入的 FrameChannel 抽象，
// 便于单测用内存脚本替身（不起真 socket）。

#include "check_options.h"
#include "check_report.h"
#include "protocol.h"

#include <atomic>
#include <functional>

namespace fc::check {

// 帧收发抽象。send 发一帧；recv 阻塞读一帧，断连时抛异常（engine 据此判 partial + 退出码 2）。
struct FrameChannel {
    std::function<void(const Frame&)> send;
    std::function<Frame()> recv;
};

// 编排结果：报告 + 建议退出码。engine 本身不 exit()，退出码回传给 check_main。
struct EngineOutcome {
    CheckResult result;
    ExitCode exit = kIdentical;
};

// 执行一次完整（或被中断的）比对。interrupted 为 Ctrl+C 原子标志，engine 每轮观察。
EngineOutcome RunCheck(const CheckOptions& o, FrameChannel& ch,
                       const std::atomic<bool>& interrupted);

}  // namespace fc::check
