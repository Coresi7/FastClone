#pragma once

// FastCheck CLI 解析与前置检查（fastcheck，M6/FR-04~12）。解析风格照抄 route_probe_main：
// ArgAt/ParseLongStrict/ParseHostPort、错误抛 std::runtime_error、usage 到 stderr。
// 关键约束：--checkers（非 --streams）；--streams/--chunk-kb 视为未知参数直接报错；usage 不出现
// --streams。参数错误在任何 TCP 之前失败（调用方映射退出码 2，非 0/1）。

#include "check_options.h"

#include <string>
#include <vector>

namespace fc::check {

void PrintUsage();

// 解析参数。缺失必填 / 非法值 / 未知参数（含 --streams）时抛 std::runtime_error。
CheckOptions ParseCheckArgs(const std::vector<std::string>& args);

// 前置本地/输出路径检查（在任何 TCP 之前，FR-12/AC-15/16）。失败打印错误到 stderr 并返回 false
// （调用方返回退出码 3）；不创建 --output 父目录、不连接 server。
bool CheckLocalPreconditions(const CheckOptions& o);

}  // namespace fc::check
