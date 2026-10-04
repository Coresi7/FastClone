# OM-4：「sticky exit 5」在 ctest 下间歇失败（单独跑全过）

- 状态：**待立项排查**
- 优先级：P2（不阻塞交付，但污染 CI 信号；flaky 会掩盖真实回归）
- 发现时间：2026-10-02（P0 修复验收期间）
- 用例：`tests/once_multi_integration.ps1` 场景 **OM-4**
- 定性：**flaky / 环境相关，非本次改动引入**（见 §3 三体对照）

---

## 1. 现象

ctest 下（Ninja 生成器，10 条用例）：

```
once_multi_integration ... Failed
  OM-4: expected sticky exit 5, got 0
```

即：期望「一个会话异常中断后，server 以 sticky exit 5 退出，且后续干净会话不得掩盖该失败」，实际拿到了 exit 0。

## 2. 单独跑：三个被测体**全部通过**

```
[OM-4] failure aggregation: aborted session -> sticky exit 5 (clean later session does not mask it)
  OK aborted session forced sticky exit=5
...
All once-multi integration tests passed.
```

| 被测体 | SHA256 / 来源 | 单独跑 OM-4 |
|---|---|---|
| P0 修复后产物 | `22fe224ca7f7bb7643cfa39d136aafd2223cdf37f327bfcb466468fcdd82e2f1` | ✅ PASS |
| 迁移后、P0 修复前 | `b25c6e41512b2edfd0888769dd9a6fa14ec522ed36c7843c36389f16e39e9405` | ✅ PASS |
| 旧版基准件（`E:\svn\...`，只读副本） | `c3f438030fad993f1ea2baaa1df874edd9f2a47ed6d7d6f9275c8d03dc734596` | ✅ PASS |

复现命令（单独跑）：

```bat
powershell -NoProfile -ExecutionPolicy Bypass ^
  -File d:\git\FastClone\tests\once_multi_integration.ps1 ^
  -ExePath d:\git\FastClone\build-msvc-x64\FastClone.exe -Port 27894
```

ctest 下复现：

```bat
build.cmd msvc Release x64 test
```

## 3. 定性依据（为什么判 flaky 而不是回归）

- **三体一致通过**：新版、`build_post`、旧版基准件单独跑 OM-4 均 PASS → 与本次 P0 修复无因果关系。
- **只在 ctest 调度下失败** → 差异来自执行环境/调度，而非被测代码。

## 4. 排查方向（**均为待验证假设，不是结论**）

1. **端口冲突（最可疑，优先验证）**：`tests/once_multi_integration.ps1` 与 `tests/data_integrity_integration.ps1` 的**默认端口同为 27894**（见两者 `param` 块的 `-Port` 默认值）。ctest 顺序执行时，若前一条用例的 server 进程未完全退出、端口仍被占用，后续用例的连接会被"认错"或对上错误的 server。
   - 验证方法：给两条用例显式传**互不相同的端口**再跑一轮 ctest，观察是否仍失败。
2. **残留进程**：以往测试中已观察到 `--once` server 在密码被拒后不自退、需 `taskkill` 清理的情况（参见 `legacy-interop-compat` 测试报告 §5）。残留进程可能干扰后续用例的端口与状态判定。
   - 验证方法：ctest 每轮结束前后 `tasklist /fi "imagename eq FastClone.exe"` 与 `netstat` 复查。
3. **idle-grace 时序竞争**：OM-4 依赖「异常会话 sticky 失败」与 idle-grace 计时器的交互。机器负载高时 grace 可能提前/滞后触发，导致 exit 0。
   - 验证方法：用固定端口、降低并发、连跑 ≥5 次统计失败率；必要时在脚本内增大 grace 余量或改为等待确定状态而非固定 sleep。
4. **用例间共享临时目录**：检查脚本是否使用固定路径导致串扰（各脚本用 `Get-Random` 生成目录名，此项可能性较低）。

## 5. 建议的修法方向（供立项时选择）

- 给所有集成用例**显式分配互不冲突的端口**（在 `CMakeLists.txt` 的 `add_test` 中通过 `-Port` 传入，而不是依赖脚本默认值）。
- ctest 注册时给用例加 `RESOURCE_LOCK` 或保持串行，并在用例首尾强制清理残留进程与端口。
- 消除脚本中的固定 `sleep` 等待，改为轮询确定状态。

## 6. 验收标准

1. **连跑 10 次 ctest，OM-4 不再出现失败**（或失败率降到 0）。
2. ctest 条目数保持不减少；其余 9 条用例通过率不下降（WCT-6 除外，见 `wct6-holdopen-prehandshake-crash.md`）。
3. 给出修复前后的失败次数统计对比（同一环境、同一命令）。
4. 若最终定位为端口/进程残留，需在文档中写明根因与防护手段，避免同类 flaky 复发。
