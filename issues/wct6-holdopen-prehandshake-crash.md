# WCT-6：`--once` server 在「已 accept 但未握手」连接下崩溃（0xC0000409）

- 状态：**待立项修复**
- 优先级：P1（server 进程崩溃，属可用性/健壮性缺陷）
- 发现时间：2026-09-30（CMake 迁移验收期间），2026-10-02 复核确认
- 用例：`tests/wait_connect_timeout_integration.ps1` 场景 **WCT-6**
- 是否本次 CMake 迁移引入：**否**（见 §4 反证）

---

## 1. 现象

```
[WCT-6] --once, accepted TCP connection held open w/o handshake -> still exit 6 (B-01)
FAILED: WCT-6: held-open pre-handshake connection -> expected exit 6, got -1073740791
```

- 实际退出码 `-1073740791` = `0xC0000409`（`STATUS_STACK_BUFFER_OVERRUN`）。
- 同脚本的 WCT-1 ~ WCT-5 **全部通过**，仅 WCT-6 失败。
- **确定性复现**：单独重跑稳定复现同一错误码，非随机 flaky。

## 2. 期望行为 vs 实际行为

| | 行为 |
|---|---|
| 期望 | server 以 `--once` 启动，收到一个只完成 TCP 三次握手、**不发送握手数据**的连接；`--wait-connect-timeout` 到期后 server 应**正常超时退出，退出码 6** |
| 实际 | server 进程**崩溃**，退出码 `0xC0000409` |

## 3. 复现步骤

```bat
rem 用任意构建产物，例如一键编译的 MSVC 产物
build.cmd msvc Release x64

rem 直接跑该用例（端口可换，但需避开其他用例占用的端口）
powershell -NoProfile -ExecutionPolicy Bypass ^
  -File d:\git\FastClone\tests\wait_connect_timeout_integration.ps1 ^
  -ExePath d:\git\FastClone\build-msvc-x64\FastClone.exe -Port 27895
```

在 ctest 中同样复现：

```bat
build.cmd msvc Release x64 test
```

## 4. 已排除项（反证，勿重复排查）

| 假设 | 结论 | 证据 |
|---|---|---|
| 由本次 CMake 迁移 / 库分层引入 | **排除** | 迁移前的 `build_baseline` 产物上同样复现；迁移后、P0 修复前的 `build_post` 产物同样复现 |
| 由新旧版本协议不兼容引入 | **排除** | 兼容性任务 `legacy-interop-compat` M1~M6 全 PASS，四方向 byte-for-byte 一致 |
| CLI / 参数解析问题 | **排除** | 同一脚本 WCT-1 ~ WCT-5 使用同类参数均正常 |
| flaky | **排除** | 单跑与 ctest 下均稳定复现同一错误码 |

被测体指纹（供复现对齐）：

| 角色 | SHA256 |
|---|---|
| P0 修复后产物 | `22fe224ca7f7bb7643cfa39d136aafd2223cdf37f327bfcb466468fcdd82e2f1` |
| 迁移后、P0 修复前 | `b25c6e41512b2edfd0888769dd9a6fa14ec522ed36c7843c36389f16e39e9405` |
| 旧版基准件（`E:\svn\...`，只读） | `c3f438030fad993f1ea2baaa1df874edd9f2a47ed6d7d6f9275c8d03dc734596` |

## 5. 影响面

- 任何**只建 TCP 连接、不发握手数据**的对端都能让 server 崩溃：端口扫描、健康检查探针、负载均衡探活、异常中断的客户端、网络中间件。
- 崩溃而非「超时优雅退出」，意味着 server 无法靠正常退出码区分「等待超时」与「异常终止」，上层守护/重启逻辑会误判。
- 与 `wait_connect_timeout` 的设计目标（AC-07/AC-13：无客户端时 exit 6）直接冲突。

## 6. 排查方向（**均为待验证假设，不是结论**）

> 规则要求：不得仅凭函数名/符号名推断根因。以下只指出应从哪条链路取证。

1. 取崩溃点：用 Debug 构建 + 调试器（或 `--diag` / 日志）定位崩溃发生在 **wait-connect 超时路径** 还是 **握手读取路径**。
2. `0xC0000409` 常见于栈缓冲破坏或 MSVC `/GS` 安全检查失败 —— 需以实际调用栈确认，**不得据此直接下结论**。
3. 对比 WCT-5（`--once-multi` + 仅探针 → 正确 exit 6）与 WCT-6 的代码路径差异：两者语义相近却结果不同，差异点即重点。
4. 检查该路径下是否存在固定大小缓冲区被越界写入、或回调/状态机在连接未握手时访问了未初始化字段。

## 7. 验收标准

1. `tests/wait_connect_timeout_integration.ps1` 的 **WCT-6 通过**：held-open 连接场景下 server 以 **exit 6** 正常退出。
2. 单独跑与 ctest 下**各连跑 3 次均稳定通过**（排除偶发）。
3. WCT-1 ~ WCT-5 保持通过，无回归。
4. server 崩溃路径消失：进程退出码不再出现 `0xC0000409`。
5. 提供修复前后对比证据（同一命令、同一端口、同一 fixture）。
