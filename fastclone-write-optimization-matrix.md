# FastClone 落盘优化评估表

目标场景：Windows 客户端大量小文件落盘约 `4000 files/s`。评分是基于当前代码阅读后的工程判断，收益 `1-5` 越高越值得优先验证。

## 总体结论

当前最像瓶颈的是“每文件固定 metadata 成本”，不是纯写带宽。小文件批量路径里，每个文件都要经历 open、write completion gate、close/SetEndOfFile、再单独 open 设置 mtime。P0 优先做两件事：

1. 把 `SetFileTime` 合并到已有写句柄，避免每文件为 mtime 再开一次文件。
2. 小文件写默认只开单个 buffered 句柄，避免无收益的 unbuffered 句柄和 IOCP 关联成本。

建议先加轻量阶段计时，分别统计 `EnsureParentDir`、open/write wait、close/SetEndOfFile、SetFileTime 的 p50/p95。确认 open/close/mtime 占主导后，再推进 P0。

## 收益 - 改动规模 - 风险表

| 优先级 | 优化项 | 收益 | 改动规模 | 风险 | 为什么 | 最小动作 | 主要文件 |
|---|---|---:|---|---|---|---|---|
| P0 | mtime 合并到已有写句柄 | 5/5 | 中 | 中 | 每文件少一次 `CreateFileW` / `SetFileTime` / `CloseHandle` 链路，直接削减固定 metadata 成本。 | 给写 close 或 `driverWriteWholeFile` 传入 mtime，关闭前 `SetFileTime`；batch 写成功后不再二次打开。 | `FastClone/disk_io_backend_win.cpp`, `FastClone/sync_engine_client.cpp`, `FastClone/file_index.cpp` |
| P0 | 小文件只开单 buffered 句柄 | 5/5 | 中 | 低 | 默认 buffered 写不需要 unbuffered 句柄；可省双句柄、IOCP 关联和 close 的大量固定成本。 | 当 `!unbufferedWrites` 或小于阈值时跳过 `hUnbuf`，仅保留 buffered overlapped handle。 | `FastClone/disk_io_backend_win.cpp` |
| P1 | 小文件同步快路径 | 5/5 | 大 | 中 | 绕过 `DiskIoDriver` 调度、`OpCtx` 分配、aligned bounce 和 completion gate，适合一次写完的小文件。 | 对小于 `128KiB` 或 `256KiB` 的 batch 文件直接 `CreateFileW + WriteFile + SetFileTime + CloseHandle`。 | `FastClone/sync_engine_client.cpp`, `FastClone/file_index.cpp` |
| P1 | 写 worker 上限调高/可配置 | 4/5 | 小 | 低 | 当前最多 16 条 per-file metadata 链路；优秀 SSD 上可能需要 32 条左右才能打满。 | 先从 `clamp(hw*2, 8, 32)` 或 env/CLI 开关开始，保留回退能力。 | `FastClone/sync_engine_client.cpp` |
| P1 | 零字节文件移出主线程 | 4/5 | 小 | 低 | 空文件现在在主循环同步创建并设置 mtime，大量空文件会阻塞接收和调度。 | 零字节文件也投递到 `IoWriteTask`，或新建 metadata worker 队列统一处理。 | `FastClone/sync_engine_client.cpp` |
| P2 | 条件跳过 SetEndOfFile | 3/5 | 中 | 高 | `SetEndOfFile` 是每文件固定 syscall，但它承载覆盖旧大文件和 unbuffered 尾部截断语义。 | 仅在 `CREATE_ALWAYS`、纯 buffered、完整顺序写等严格条件下跳过，先加断言和测试。 | `FastClone/disk_io_backend_win.cpp` |
| P2 | `OpCtx` / aligned buffer 对象池 | 3/5 | 中 | 中 | 减少每写 op 的 `new/delete`、`AlignedAlloc/Free` 和 allocator 压力。 | 在 `WinIocpBackend` 内做固定大小池，先覆盖小文件常见 `4K/64K/256K` 桶。 | `FastClone/disk_io_backend_win.cpp` |
| P2 | 减少 `WinFile` 拷贝和锁往返 | 2/5 | 中 | 中 | `submit` 热路径每 op 拷贝 `WinFile` 并穿过多个锁；小文件 op/file 时占比被放大。 | `files_` 存 `shared_ptr` 或稳定句柄状态，明确 close 与 in-flight op 生命周期。 | `FastClone/disk_io_backend_win.cpp`, `FastClone/disk_io_driver.cpp` |
| P3 | server batch open 元数据压缩 | 2/5 | 中 | 低 | 服务端每文件 `exists/is_regular/file_size/probe open/mtime` 有重复 metadata 查询。 | Windows 上用 `GetFileAttributesExW` 或一次 probe handle 承接 size、mtime、可读性。 | `FastClone/sync_engine_server.cpp` |

## 推荐执行顺序

1. **先做诊断**：给写 worker 加阶段计时，确认 `4000 files/s` 的具体墙在 metadata 还是 driver 调度。
2. **做 P0 两项**：mtime 合并到已有句柄、小文件单 buffered 句柄。这两项收益最大，且不改协议。
3. **做低风险 P1**：写 worker 可配置上限、零字节文件异步化。
4. **再评估快路径**：如果 P0 + 低风险 P1 后仍不够，再考虑小文件同步快路径。
5. **P2/P3 放后**：`SetEndOfFile` 条件跳过和对象池需要更多测试，适合按 profiling 结果决定。

## 需要重点验证的指标

- batch 小文件写入阶段：files/s、p50/p95/p99 per-file latency。
- `EnsureParentDir` 命中率和 miss 耗时。
- `openFile` 到首个 write submit 的耗时。
- write completion wait 耗时。
- `closeFile`，尤其是 `SetEndOfFile` 耗时。
- `SetFileModifyTime` 耗时。
- `clientDriver.counters()` 中 `bufferedFallback`、`directIo`、`tailZeroFallback` 的比例。

## 风险提醒

- `SetEndOfFile` 不要贸然全局删除，它保证覆盖旧大文件时不残留尾部，也承担 unbuffered 尾扇区裁剪语义。
- 小文件快路径要处理和现有 driver 一致的失败重试、mtime、目录创建、计数和 backpressure。
- 提高 worker 并发可能被杀软、过滤驱动或 NTFS metadata 锁放大抖动，最好做成可调参数。
