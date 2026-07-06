# FastClone dev-map

> 落点/惯用法索引。首次创建（此前仓库根无 dev-map，见 unified-disk-io-driver 需求 D8）。
> 本次由 `unified-disk-io-driver` 任务补建，覆盖新增的统一异步磁盘 IO 驱动模块；其余既有模块
> 后续按「谁动谁补」增量填充。
>
> 分模块子图：
> - FastCheck 只读比对 + compare_phase 共享 TU + CheckAuth/Check 会话：见 `docs/dev-map/fastcheck.md`。

## 统一异步磁盘 IO 驱动（unified-disk-io-driver）

分层：`上游生产者 → DiskIoDriver(调度/公平/背压/取消/计数) → PlatformIoBackend(多 op 在飞) → 完成收割`。

| 文件 | 职责 | 关键符号 | 平台守卫 |
|---|---|---|---|
| `FastClone/disk_io_align.h/.cpp` | 运行时对齐查询 + 平台对齐分配（零硬编码 4096/512） | `AlignInfo`、`QueryAlign`（进程内按卷缓存，见 RS-02）、`AlignCacheSizeForTest`(测试探针)、`QueryPageSize`、`AlignUp/Down`、`IsAligned`、`AlignedAlloc/Free`、`kSmallFileBufferedMax` | Win `GetVolumePathNameW`+`GetDiskFreeSpaceW`+`_aligned_malloc`；Linux `BLKSSZGET`/`statvfs`+`posix_memalign`；mac `statvfs`+`posix_memalign` |
| `FastClone/disk_io_backend.h` | 后端抽象 + 共享请求/完成类型 + 工厂声明 | `PlatformIoBackend`、`IoRequest`、`IoCompletion`、`OpKind/Prio/IoStatus/BackendKind`、`IoDriverConfig`、`BackendCounters`、`CreatePlatformBackend` | 无（纯头） |
| `FastClone/disk_io_backend_pool.h` | POSIX 池内部工厂 + 共享计数块 | `CreatePosixPoolBackend(cfg, ioUringFallback=false)`（`ioUringFallback=true` 时把计数块 `ioUringFallback` 置 1，供 io_uring 降级用）、`PoolCounters` | `#if __APPLE__ || __linux__` |
| `FastClone/disk_io_backend_win.cpp` | Windows IOCP + `FILE_FLAG_NO_BUFFERING`；小文件/尾部缓冲兜底；写用 SetEndOfFile 收尾 | `WinIocpBackend`、`CreatePlatformBackend`(win) | `#if defined(_WIN32)` |
| `FastClone/disk_io_backend_posix.cpp` | pread/pwrite 线程池；Linux `O_DIRECT`+EINVAL 静默回退+`FADV_SEQUENTIAL`；mac `F_NOCACHE`（禁 O_DIRECT/aio） | `PosixPoolBackend`、`CreatePlatformBackend`(mac) | `#if __APPLE__ || __linux__` |
| `FastClone/disk_io_backend_uring.cpp` | Linux io_uring 运行期探测；`io_uring_queue_init` 失败 / 无 liburing 两条分支降级线程池并 `CreatePosixPoolBackend(cfg, true)` 置 `ioUringFallback=1`（AC-11） | `UringBackend`、`CreatePlatformBackend`(linux) | `#if defined(__linux__)` + `__has_include(<liburing.h>)` |
| `FastClone/disk_io_driver.h/.cpp` | 单调度线程；读/写双队列加权信用轮转 + 小 op 优先；有界背压；取消；per-file 完成路由 + per-file `waitForFile` 唤醒；观测计数；`SequentialReader` | `DiskIoDriver`、`IoCounters`、`SequentialReader` | 无（跨平台） |

### 惯用法 / 约束
- **缓冲所有权按值（`std::vector`）**：读在完成里 move 出字节，写把源字节 move 进请求；内存上界 = 队列深度 × chunk，绝不按文件大小分配（NFR 内存/AC-04）。
- **平台后端编译期强隔离**（design D-04）：`CreatePlatformBackend` 每平台恰一处定义（win/posix(mac)/uring(linux)）；非本平台的 .cpp 编译成空 TU。macOS 路径源码**不出现** `O_DIRECT`/`aio_*`（AC-13）。
- **对齐一律运行时取**：`QueryAlign` → `ioGranularity = max(pageSize, deviceBlockSize)`；direct buffer 用 `AlignedAlloc(ioGranularity,…)`。禁止把 4096/512 当对齐常量（AC-14/15）。
- **停机次序**：`~DiskIoDriver` → `requestCancel()`（未提交 op 置 Cancelled）→ 置 `stop_` → join 调度线程（停机时队列空即退出，不依赖在飞归零）→ `backend_->shutdown()`（收割/释放在飞、join 后端线程）。
- **公平**：读:写默认 1:1 信用轮转；同方向内 `Prio::Small` 优先（`PopPreferSmall`）。`cfg.recordSchedule` 记录提交方向序列供 AC-19/20 断言。
- **POSIX direct IO 对齐判定只看 off+len**（F1）：`DoPread/DoPwrite` 的 aligned 判定**不**掺入调用方 `dst/src` 指针地址对齐——direct IO 走内部 bounce buffer `AlignedAlloc(g,len)`，指针只参与 `memcpy`。掺指针条件会让 fully aligned op 恒走 per-op buffered 兜底并 `tailZeroFallback++`（与 uring 后端 `IsAligned(off,g)&&IsAligned(len,g)` 一致）。
- **submit 失败合成 completion 必须回填原 op 字段**（F2）：`PickAndSubmit` 在 `backend_->submit(std::move(op))` 前捕获 `kind/fileId/offset/length/userTag`，失败时按 `opFileId` 归属（`completionsByFile_[opFileId]` + `completionOrder_.push_back(opFileId)`），不得落到 fileId 0。
- **`qmu_` 为 `mutable`**（F3）：`counters() const` 直接 `lock_guard(qmu_)` 读 `readQ_/writeQ_.size()`，**不**用 `const_cast`（对齐 `mutable std::mutex countMu_`）。
- **`SequentialReader` 提前 EOF 归错**（F4）：命中 `nextYieldOffset_` 的 completion 若为 `Error` 或 `Eof` 均置 `error_`、返回 0、`ok=false`；干净 EOF 只由 `nextYieldOffset_ >= fileSize_` 判定。reader 按 `fileSize_` 精确切片，满读返回 `Ok`，仅文件比预期短才 `Eof`。
- **Windows 卷扇区查询按 UTF-8 转宽**（F5）：`disk_io_align.cpp` `QueryDeviceBlockSize` 用 `MultiByteToWideChar(CP_UTF8,…)`（同 `disk_io_backend_win.cpp` `Widen`），不得用 `std::wstring(path.begin(),path.end())` 字节拓宽，否则非 ASCII 路径查询失败退化为仅 page-size 兜底。
- **per-file 等待唤醒**（fastcheck-smallfile-disk-perf）：`waitForFile(fileId)` 只等待对应 `fileId` 的 completion/cancel，入队统一走 `EnqueueCompletion` 并 `notify_one` 到该 file 的 condition_variable；`requestCancel()` 也按 fileId 路由 Cancelled completion。
- **默认并发与内存上界**（fastcheck-smallfile-disk-perf）：`IoDriverConfig` 默认 `maxInFlight=64`、`backendConcurrency=8`；默认 `chunkBytes=1MiB` 下，读在飞上界 `64MiB`、backend scratch 上界 `8MiB`，写队列上界仍为 `maxWriteQueue*chunkBytes`。

## 流式 BuildPlan（bit-identical，unified-disk-io-driver C6）

| 文件 | 职责 | 关键符号 |
|---|---|---|
| `FastClone/delta.h` | 声明流式入口（新增 `<functional>`；仍无 `<xxhash.h>`） | `ByteSource`、`CopyCapturedFn`、`StreamingPlanOptions`、`BuildPlanStreaming` |
| `FastClone/delta.cpp` | 复用现有 `BuildPlan` 算法，仅把随机 `oldData[]` 访问换成前向滑动窗口 | `SlidingWindow`（匿名）、`BuildPlanStreaming` |

- **等价性**：`BuildPlanStreaming(sig, oldLen, source, {opt})` 对每个 `DeltaPlan`/`DeltaStats` 字段与 `BuildPlan(sig, oldData, oldLen, opt)` 逐字节相等（`test_streaming_buildplan.cpp` 六类场景×多 chunk 验证）。现有 `BuildPlan`/`StreamingSigner` 未改。
- **命中即捕获**（D-01 A）：匹配到整块时用窗口内字节回调 `onCopy`，供客户端在单遍扫描内落 copy，无需二次读/整文件缓冲。

## 测试落点
- `tests/test_disk_io_align.cpp` → `RunDiskIoAlignTests`（对齐数学/16K/64K 替身/对齐分配匹配；F5：`TestQueryAlignAscii` ASCII 回归、`#if _WIN32` `TestQueryAlignNonAsciiWindows` 非 ASCII UTF-8 路径；缓存：`TestQueryAlignSameVolumeReuse`/`TestQueryAlignBounded`/`TestQueryAlignConcurrent`、`#if _WIN32 TestQueryAlignMultiVolumeWindows`，见 RS-04）
- `tests/test_streaming_buildplan.cpp` → `RunStreamingBuildPlanTests`（流式 vs 内存 BuildPlan 等价 + 捕获重建）
- `tests/test_disk_io_driver.cpp` → `RunDiskIoDriverTests`（Mock 后端：往返/多 op 在飞/公平/背压/取消/计数；真实平台后端端到端往返含 NO_BUFFERING 大文件+小文件缓冲+尾部；`#if __linux__` `TestUringFallbackCounter` 断言池降级 `counters().ioUringFallback==1`、正常池为 0，AC-11/B-01。F2 `TestSubmitFailureAttribution`（`failSubmit` 开关）；F4 `TestSequentialReaderEarlyError/EarlyEof/CleanEof`（`forceReadError` 开关 + 短读 Eof）；`#if __linux__` F1 `TestPosixPoolAlignedDirectIo`（对齐 op `tailZeroFallback` 增量==0）。`MockBackend` 增默认 false 的 `failSubmit_`/`forceReadError_` 开关）
- 三者已登记进 `tests/test_main.cpp`、`CMakeLists.txt`、`FastCloneTests.vcxproj`。
- **TC-01 测试覆盖补充（fastcheck-test-coverage-supplement）**：无生产代码改动，全部 test-only，无生产 hook/导出/头声明/条件编译新增；落点复用已注册 `RunDiskIoDriverTests`/`RunCheckEngineTests`，不改构建系统。
  - **Gap A** `tests/test_disk_io_driver.cpp` `TestServerHashMissThreeWayEquivalence`（新增 `#include "file_index.h"`）：test-only `ReconstructOldServerHashMiss`（`SequentialReader`+`ComputeHashFromSource`+1 MiB chunk+4 read-ahead，`src` 拉取循环与被移除的 `sync_engine_server.cpp` hash-miss inline 块字节级一致，源见 git 父提交 `37d32ad`，C-01）与 `ComputeFileHashViaDriver`/`ComputeFileHash` 三路 Hash256 等价；尺寸矩阵 0/1/64/4096/65535/65536/262143/262144/262145/1048576/1048577/5242880/5000003；真实 `DiskIoDriver`，禁 mock。
  - **Gap B** `FastCheck/tests/test_check_engine.cpp`：`TestComputeFileHashViaDriverConsistency`（保留，含 256KiB±1/0/1 阈值边界）+`TestComputeFileHashViaDriverUnalignedTail`（>1 MiB 非对齐尾 1MiB+1/1MiB+123/5MiB+3/5000003，走 `SequentialReader` 慢路径）+`TestComputeFileHashViaDriverSparseFile`（`#if _WIN32` `FSCTL_SET_SPARSE` 真稀疏，失败/非 Windows 回退等长确定性内容文件，含 `#include <winioctl.h>`）；均断言 `ComputeFileHashViaDriver == ComputeFileHash`，真实 `DiskIoDriver`。
  - **Gap C** `tests/test_disk_io_driver.cpp` `TestReadOpenExpectedSizeContract`（1MiB-1/1MiB/1MiB+1 × `expectedSize∈{size,0}` 读回字节均等于文件内容，clean EOF + closeFile 由 `ReadAllViaDriver` 保证）+`TestReadOpenExpectedSizeMismatch`（过估 `expectedSize=size+2MiB` 为策略提示，非内容长度真源；`SequentialReader` 按真实 size 计划，读回仍等于真实内容，D-03 高估方向）。
  - **Gap D** `tests/test_disk_io_driver.cpp` `TestConcurrentQueryAlignAndReadIntegration`：同卷 32 文件（彼此不同确定性内容，每 8 个 ≥1 MiB 覆盖 unbuffered 路径），共享真实 `DiskIoDriver`，16 线程 round-robin `queryAlign`→真实 read `openFile`→`SequentialReader` 完整读回→逐字节校验→`closeFile`；原子计数断言全部读取校验通过、全部 fileId 关闭、无 open 失败（C-02：threads=16/files=32 为固定下限，仅 iters 可调）。
  - 复用 helper：`WriteViaDriver`（真实驱动写盘，Gap C/D 共用）、`ReadAllViaDriver`/`RandomBytes`（既有）、Gap B 复用 `MakeTempDir`/`WriteFile`/`MakeDeterministicContent`/`HashEquals`。

## 构建集成
- 新增源统一放入 CMake 变量 `FASTCLONE_DISK_IO_SOURCES`，同时挂到 `FastClone` 与 `FastCloneTests` 两个 target；四个后端 .cpp 全量列入、内部平台守卫择一。
- Linux 可选 liburing：CMake `find_path/find_library`，命中则 include+link，未命中则由 `__has_include` 守卫走线程池降级。
- 两个 `.vcxproj` 同步登记 `ClInclude`/`ClCompile`（四套配置 Debug/Release × Win32/x64）。

## 驱动接入点（unified-disk-io-driver C7–C10）

上游读写路径接入统一驱动。红线 bit-identical 由 C6 等价单测 + 四套构建 + Windows loopback E2E 冒烟守（`smoke.ps1`：全文件 SHA256 相等 + `delta_reconstructed>0`，即 delta 真走重建而非回退）。

| 接入点 | 文件 / 符号 | 做法 |
|---|---|---|
| C7 客户端 delta plan 读 | `sync_engine_client.cpp` `buildDeltaPlanOffloaded` + `clientDriver` | 删 `ReadWholeFile`；`SequentialReader`→`ByteSource`→`delta::BuildPlanStreaming`（bit-identical）；`CopyCapturedFn` 命中即写 temp（D-01 A）。读错回退 `old_unreadable`。 |
| C8 客户端下载写 | `sync_engine_client.cpp` `driverWriteWholeFile` + IO 写池 worker；`DownloadState`（单文件流式）；`DeltaFileState`/`buildDeltaPlanOffloaded`（delta temp） | **全部**客户端文件内容写经统一驱动（unbuffered-writes 完成 C8）：① 批量整文件仍由 `IoWriteTask` worker `driverWriteWholeFile` 提交；② 普通单文件 `DownloadState` 改**流式** driver 写（`pumpDownloadWrites` 每满 `chunkBytes` 提交对齐块，`FileEnd` flush 尾 + per-file 写完成门禁 `drainFileWritesToCompletion`）；③ delta temp 的 copy 写在 plan worker `onCopy` `submitDriverWrite`+就地收割，range 写在主循环 `DeltaRangeChunk` submit，`finalizeDelta` 校验前门禁全部写 completion（FR-08）。无缓冲意图由 `--unbuffered-writes` 经 `openFile(..., unbufferedWrites, expectedSize)` 逐文件透传（D-01）。 |
| C9 服务端 sig+hash 读 | `sync_engine_server.cpp` `GetServerDiskIoDriver`；HashRequest / BlockSigRequest miss | HashRequest：`SequentialReader`→`ComputeHashFromSource`（新增 `file_index`，XXH3 流式，同字节同 `Hash256`）。BlockSig：`SequentialReader`→`StreamingSigner`；**解除 `ReadGateGuard`**（并发改由驱动 `maxInFlight`+背压承担，`ReadGate`/`GetServerReadGate` 类型保留，D-08 A）。 |
| C10 落盘校验读 + 服务端 range 读 | `sync_engine_client.cpp` `finalizeDelta`；`sync_engine_server.cpp` `ServerRangeStream`/`DriverReadRangeChunk` | finalize 校验读经驱动喂 `ComputeHashFromSource`（同字节同校验判定）。服务端 delta range 读：`ServerRangeStream` 持驱动 `fileId`，发送环 `DriverReadRangeChunk` 单 op 阻塞读（与原同步 ifstream 同粒度，不改环路响应性）；完成/出错/会话拆解均 `closeFile`（防句柄泄漏）。 |

### 接入惯用法 / 约束
- **`std::min`/`std::max` 必带模板实参**（`std::min<size_t>(...)`）：client/server TU 经 `sync_engine_internal.h` 拉入 `<Windows.h>`，裸 `std::min(a,b)` 撞 `min` 宏（CMake 有 `NOMINMAX`，VS vcxproj 无 → 只在 MSBuild 炸）。这是本次踩过的坑。
- **读路径 ByteSource**：`SequentialReader::next(out,ok)` 的 `ok=false` 必须显式转成回退（客户端 `old_unreadable`/服务端 `DeltaError`/hash `0xFF`）；不能让短读被当成干净 EOF（否则对部分文件产出错误 plan/hash，破坏 bit-identical）。
- **驱动共享生命周期**：客户端 `clientDriver` 为 `RunClient` 栈局部，声明在所有 worker 池之前（析构在全部 join 之后）。服务端 `GetServerDiskIoDriver()` 进程级单例（同 `GetServerHashPool()`）。
- **驱动 fileId 必须 `closeFile`**：读一次性 open/close；服务端 range 跨接收→发送线程持 `fileId`，发送环与会话拆解两处兜底关闭。

## 无缓冲客户端写 / 写背压（unbuffered-writes）

`docs/tasks/unbuffered-writes/`。客户端 `--unbuffered-writes`（仅客户端，默认关闭）把全部文件内容写纳入统一驱动并可表达无缓冲意图；写压力反传接收侧，与 `--queued-file-size` 共用一个预算。

| 落点 | 文件 / 符号 | 做法 |
|---|---|---|
| CLI 开关 | `cli.h` `CliOptions::unbufferedWrites`；`cli.cpp` 解析分支 + server-only 校验(`"--unbuffered-writes is client-only"`) + `PrintUsage` | 无值幂等；`RunClient` 取 `const bool unbufferedWrites`。 |
| 逐文件透传 | `sync_engine_client.cpp` 三条写路径 `openFile(..., unbufferedWrites, expectedSize)` | D-01：读路径恒缓冲、写路径按开关；不改驱动/`sync_engine.h` 接口。 |
| 写完成门禁 | `submitDriverWrite`/`reapDriverWrites`/`drainFileWritesToCompletion`（RunClient 局部 lambda） | 单文件 `FileEnd`、delta `finalizeDelta` 校验读前阻塞收割至 `completed==submitted`（FR-08/D-06）；写错→`writeError`→`retryOrFail`/`reconstruct_io`（FR-16）。 |
| 写背压预算 | `driverWriteOutstandingBytes`(atomic) + `applyRecvBackpressure` | submit `+len`/收割 `-requested`；`pressure = incomingQueuedBytes + ioInFlightBytes + driverWriteOutstandingBytes` 比 `incomingSoftLimitBytes` 有界 sleep（FR-09/10/11）。 |

### 惯用法 / 约束
- **对齐才无缓冲，否则缓冲精确写**（D-03）：win `submit` 写路径 `useUnbuf = unbuffered && offAligned && (lenAligned || offset+length==expectedSize/*EOF尾*/)`；未对齐**中间**片段走缓冲句柄精确长度写（不 `AlignUp`、不零填），杜绝污染邻接扇区。posix/uring `DoPwrite`/`IssueOrInline` 本就是该模型。
- **win 写需双句柄共存**：写 open 用 `FILE_SHARE_READ|FILE_SHARE_WRITE`，否则 `hUnbuf` 打开后 `hBuf` 撞共享冲突（未对齐片段落到无效句柄）。
- **写 open 恒定尾**：三后端 `closeFile` 对 `OpKind::Write` 一律 `SetEndOfFile`/`ftruncate(expectedSize)`（含 0），等价 `ofstream trunc` 覆盖语义（不残留旧尾字节，FR-14/空文件）。
- **小文件门槛仅读路径**（D-02）：`openFile` 的 `expectedSize>=kSmallFileBufferedMax` 降级只对 `OpKind::Read` 生效（读 open 传 `expectedSize=0` 故恒缓冲）；写按意图不因文件总大小整文件降级（FR-13）。
- **单文件流式对齐**：`pumpDownloadWrites` 每满 `chunkBytes`（1MiB，扇区对齐）提交一块，尾块在 `FileEnd` 提交；`writeBuffer` 常驻 ~1 chunk，不整文件驻留（AC-04）。
- **驱动 fileId 必须 closeFile**：单文件 `FileEnd`/`FileError`、delta `finalizeDelta`/`DeltaError`、`failoverScan`、会话拆解均收割+closeFile；后端 `shutdown` 额外兜底关闭遗留句柄（防会话重连泄漏）。
- **delta temp 保持无缓冲意图**：copy/range 写落点两两互斥、对齐 op 独占整扇区（D-03），压测 12/12 逐字节一致；极少数写失败按 FR-16 回退全量下载（内容仍正确）。

## FastCheck 并行哈希管线（fastcheck-parallel-hash）

`docs/tasks/fastcheck-parallel-hash/`。FastCheck strict 原由 `check_engine.cpp` 接收循环内串行 `ComputeFileHash` 支配耗时；本任务把本地哈希异步化，并把 FastCheck 本地文件内容读取收敛到 FastClone 统一 diskio。

| 落点 | 文件 / 符号 | 做法 |
|---|---|---|
| 共享哈希入口 | `FastClone/file_index.h/.cpp` `ComputeFileHashViaDriver(DiskIoDriver&, path)` | 单一来源：`fs::file_size → openFile(Read,unbuffered)`；`fileSize<=256KiB` 走一次对齐 driver read + `ComputeBufferHash`，`fileSize>256KiB` 走 `SequentialReader(1MiB,readAhead=4) → ComputeHashFromSource`；最终 `closeFile`。任一失败 throw。头文件仅前置声明 `fc::io::DiskIoDriver`，实现 TU include `disk_io_driver.h`。 |
| 服务端共用 | `sync_engine_server.cpp` HashRequest miss lambda | 内联 driver 读块替换为 `hash = ComputeFileHashViaDriver(GetServerDiskIoDriver(), abs)`，外层 `try{...}catch(...){hashOk=false;}` → `hash.fill(0xFF)` 逐字节等价保留。 |
| FastCheck 客户端 | `FastCheck/check_engine.cpp` `RunCheck` worker 池 | eager 入队本地 hash + pending `HashResponse` + drain + 双 AIMD + 进度日志 + 收尾 join。 |
| CLI | `FastCheck/check_options.h`、`check_cli.cpp` | `--checkers` 默认 8→32；新增 `--hash-workers`（0=auto）、`--no-diskio-driver`。 |
| 构建 | `CMakeLists.txt` `FASTCHECK_SHARED_SOURCES` | 追加 `${FASTCLONE_DISK_IO_SOURCES}`；liburing `foreach` 扩含 `FastCheck FastCheckTests`（该块移到全部 target 定义之后）。 |

### 惯用法 / 约束（D-01..D-05）
- **D-01 diskio 统一约束**：FastCheck 本地文件内容 hash 读取以 `ComputeFileHashViaDriver` 为入口；**禁止**在 FastCheck 新增 `ComputeFileHash` 线程池替代 driver 读取，降级仅经 `--no-diskio-driver` 开关（worker 分支选 `ComputeFileHash`，其余管线不变），不新增旁路实现。
- **D-02 共享入口位置**：`ComputeFileHashViaDriver` 位于 `FastClone/file_index.h/.cpp`；FastClone 服务端 hash miss 与 FastCheck 客户端本地 hash **共用**该函数；失败 throw、调用方 catch（服务端 `0xFF`，客户端 `localHashFailed`）。
- **D-03 构建 / 读取**：FastCheck / FastCheckTests target 链接统一 diskio 五件套（复用 `${FASTCLONE_DISK_IO_SOURCES}`），用 `DiskIoDriver + SequentialReader` 执行本地 hash 读取。`file_index.cpp` 因此依赖 disk_io，凡链它的 target（FastClone/FastCloneTests/FastCheck/FastCheckTests）必须链 disk_io；`FastCloneRouteProbe` 不链 `file_index.cpp`（免疫）。
- **D-04 check pipeline 惯用法**：发 `HashRequest` 同步入 `hashTaskQueue`（eager）；`HashResponse` 早到存 `serverHashReady`（pending），本地 hash 早到即在 `HashResponse` 归类；主循环每轮 `drainReady` 配对归类，一次且仅一次（首次 `awaiting.erase` 保证幂等）。唤醒模型：`expectFrame=(!manifestDone)||(inFlight>0)` 为真则阻塞 `recv`，否则仅本地 hash 待完成 → `readyCv.wait_for(200ms)` 避免死锁。退出路径统一 `hashStop`+`join` 全 worker+末次 drain；`clientDriver`（`std::optional`）随 `RunCheck` 作用域在 join 后析构。
- **D-05 并发分界**：`--hash-workers` 管本地 hash 维度（固定 `maxWorkers=max(init,4*hw)` 线程池 + 原子 `hashWorkerCap` 门控活跃数，AIMD 纯函数 `NextLocalWorkerCap`）；`--checkers` 管网络 hash request 窗口（`hashWindow` 初值 `--checkers`，RTT EWMA 驱动 `NextNetWindow` AIMD，`[1,256]`）。两维度各自 AIMD、互不写对方目标值；pump 双门（网络窗口 + `kLocalBacklogFactor*hashWorkerCap` 本地背压）为读取式背压，不违反解耦。AIMD 规则抽为 `check_engine.h` 纯函数，供 `test_check_engine.cpp` 断言。
- **D-06 小文件 hash 快路径**（fastcheck-smallfile-disk-perf）：`kSmallFileDirectThreshold=256*1024`；`ComputeFileHashViaDriver` 在 driver 模式下小文件使用单次对齐读并只 hash 真实 `fileSize` 字节，不得绕过 `DiskIoDriver`。`--no-diskio-driver` 降级语义保持不变。

### strict 延迟本地探测（fastcheck-strict-defer-probe，叠加于 parallel-hash 之上）

`docs/tasks/fastcheck-strict-defer-probe/`。strict 模式原在 `RunCheck` 接收线程同步 `ProbeLocal`（每条 manifest 一次 metadata syscall），海量小文件下把接收速率压到发送速率以下 → TCP 背压 → 服务端枚举阻塞 → hash 窗口钉死。本任务把 strict 的本地存在性/大小判定从接收线程延后到本地 hash worker。仅改 `FastCheck/check_engine.cpp`（+测试），红线文件（`compare_phase.*`/`check_engine.h`/`check_options.h`/`check_cli.cpp`/`sync_engine_server*`/`disk_io_*`/协议）零改动。

- **S-01 recv 分叉**：strict 收到文件型 `ManifestEntry` **不** `ProbeLocal`，一律 `hashQueue.push_back(HashNeed{remote, nullopt})` + 记账 + `pump()`；Fast/SizeOnly recv 分支（`ProbeLocal`+`DecideCompare`+`record`/入队）**逐行保留**。
- **S-02 worker 延迟探测**：`HashTask` 增 `deferredStat`(strict=true)+`remote`（`sendHashRequest` eager 构造）；worker 对 `deferredStat` 任务先 `ProbeLocal`→`DecideCompare(Strict,...)`：`!needHash` 产 `LocalResult{Missing|SizeDiff, local}` 不 hash，size-equal 走 `hashOne` 产 `Hashed`。**分类唯一真源仍是 compare_phase 的 `DecideCompare`/`ClassifyByHash`**（worker 复用、不内联复制）。
- **S-03 结果表**：`localResults`（`LocalResult{Kind{Hashed,Missing,SizeDiff,Failed}, hash, local}`）**取代** parallel-hash 的 `localHashes`/`localHashFailed`，仍受 `hashResultMu` 保护；strict 的 `local_size` 由 worker `ProbeLocal` 回填进 `lr.local`（Fast 仍取 `awaiting.local`）。
- **S-04 配对定案（候选 A）**：`drainReady`/`handleHashResponse` 共用主线程内联 `finalizePaired(awaitingIt, LocalResult, serverHash)`，仍「worker 结论 + 远端 HashResponse 两事件到齐才 `record`+`awaiting.erase`」（一次且仅一次）。**Missing/SizeDiff 也占 `inFlight` 直到远端响应返回**，但分类只用 worker 结论、丢弃远端 hash。**严禁** `resolvedNoHash` 或「先 erase awaiting 再等响应减 inFlight」的错位；`inFlight` 只由 `handleHashResponse` 减一。Failed 分支 `readFailSignal.fetch_add`+`localReadFailed=true`+`return`（不 erase，交 teardown），与基线逐位一致（AC-13 走查覆盖）。
- **S-05 记账回退**：strict recv 对**每个**文件先 `++hashEnqueued`/`hashBytesTotal += remote.fileSize`（上界），worker 判 Missing/SizeDiff 后在 `finalizePaired` 回退该两项；稳态 `hashBytesTotal` 收敛为真正参与 hash 的字节和。Fast/SizeOnly 永不触发回退。
- **S-06 已知 pre-existing 挂起**：`FastCheckTests.exe` 偶发不退出（~13-20%），根因是 parallel-hash 管线/`disk_io` 并发本地 hash 读取路径的 worker 卡死（`joinWorkers()` 卡在 worker；MockChannel `recv` 空即抛、`readyCv` 均 200ms 超时，唯一无限等待点即 worker 未返回）。严格隔离对比（仅回退本任务 engine 改动，各 30 次带 30s 超时）证明其在 strict-defer 改动**前**即存在：基线 6/30 vs 本任务 4/30。属 **parallel-hash / disk_io（Commit A）** 范畴，受 N1/N2 约束不在 strict-defer 任务内修，待 reviewer 判定是否另开任务。

### 冗余元数据 syscall 消除（fastcheck-redundant-syscall-elim，叠加于 strict-defer / smallfile-perf 之上）

`docs/tasks/fastcheck-redundant-syscall-elim/`。三处协同改动消除 strict 与共享 disk IO 读路径上的重复 metadata syscall，行为/分类/报告/退出码/hash/协议/诊断字段全部保持不变。

- **RS-01 strict worker 大小判定（Change 1，`FastCheck/check_engine.cpp` `deferredStat` 分支）**：worker **不再** `ProbeLocal`+`DecideCompare`，改为单次 `std::filesystem::directory_entry(task.abs, ec)` 的缓存元数据查询直接判定：非普通文件/不存在/目录/大小不可读 → `Missing`；`localSize != remote.fileSize` → `SizeDiff`（回填仅含 `fileSize` 的最小 `FileEntry`）；相等 → 现有 `hashOne`（`Hashed`/`Failed` 语义不变）。**取代** S-02 中「worker 先 `ProbeLocal`→`DecideCompare`」的旧写法（strict 忽略 mtime，size 判定与 `DecideCompare(Strict,…)` 等价）。
  - **偏差（重要）**：设计 §3.1.2 伪码用 `fs::file_size(path, ec)` 并假定其对目录返回非空 `ec`→Missing。**MSVC STL 实测：`fs::file_size` 对目录返回 0 且 `ec` 为空**，会把目录误判成 `SizeDiff`、令 AC-04 失败。故改用 `directory_entry`（构造即一次缓存查询，同时给出类型+大小；`is_regular_file()/file_size()` 读缓存不再 syscall），满足 FR-01「单次 std::filesystem 本地元数据查询、无 ProbeLocal、无 mtime」意图 + FR-02 + AC-04，且仍是**单次** metadata syscall（优于 `fs::file_size`+`is_regular_file` 两次）。AC-02「grep `fs::file_size`」为走查项、无自动化测试；已在此说明用 `directory_entry::file_size(ec)` 承接。
  - **诊断**：删除该分支对 `probeUsSum`/`probeCount` 的累加；两原子与 `[check-phases] probe_us_avg/probe_count` 输出保留（strict 稳态 `probe_count` 不随文件数增长，`probe_us_avg` 恒 0）。`fs_us_avg` 等其余字段与 `GetHashPhaseTimings` 累加点不变（FR-08/FR-26/AC-15/AC-16）。Fast/SizeOnly recv `ProbeLocal` 分支与 `--no-diskio-driver` 分支零改动（FR-09/M13/M14）。
- **RS-02 QueryAlign 进程内按卷缓存（Change 2，`FastClone/disk_io_align.h/.cpp`）**：`QueryAlign(path)` 增单锁（`std::mutex g_alignCacheMu` + `unordered_map` `g_alignCache`）check-then-insert 缓存。键 = 卷根标识：Win `GetVolumePathNameW`（UTF-8→UTF-16 用 `MultiByteToWideChar(CP_UTF8)`，键为 UTF-16 字节），POSIX `::stat` 的 `st_dev`。键解析失败（新增 `ResolveVolumeKey` 返回 false）不缓存、退回原始未缓存 `MakeAlignInfo(QueryPageSize(), QueryDeviceBlockSize(path))`（FR-14，不污染其他卷）。命中/未命中返回值与旧未缓存查询逐字段一致（M9/FR-11）。缓存条目数按卷/设备数增长（FR-13/NFR-05）。测试探针 `AlignCacheSizeForTest()`（`.h` 声明、非生产 API、仅测试读条目数，N7/N8 安全）。
- **RS-03 read openFile 复用已知大小（Change 3，`FastClone/disk_io_backend_win.cpp` + 契约注释 + 调用点）**：
  - Win backend read 分支：`expectedSize>0` 时 `sizeForPolicy=expectedSize` 并**跳过** `FileSizeOnDisk`；`expectedSize==0` 保留 `FileSizeOnDisk`（0 作未知/失败哨兵 + small-file fallback + tail clamp 旧行为）。写模式不变（FR-15/16/17）。`disk_io_backend.h` `openFile` 注释补充 read 正/零 `expectedSize` 语义（FR-18/AC-27）。
  - POSIX/uring：read direct 门控与 `expectedSize` **解耦**（`mode==Read ? false : true`），read 恒 buffered，保持既有 direct/buffered 策略与 `smallFileFallback` 计数（FR-25/AC-39）。写路径不变。
  - 调用点传已知大小：`file_index.cpp ComputeFileHashViaDriver`（传 `fileSize`）；`sync_engine_server.cpp` BlockSig miss（传 `fileSize`）、DeltaRangeOpen（前置 零长度/`offset+length` 溢出 → `DeltaError` 不 open，否则传 `offset+length` 读取边界）；`sync_engine_client.cpp` `finalizeDelta`（传 `verifySize`）、`buildDeltaPlanOffloaded`（旧文件传 `oldLen`）。HashRequest miss 仍走 `ComputeFileHashViaDriver(GetServerDiskIoDriver(), abs)`，无服务端专用 hash 分支（FR-19..FR-24/M12）。
- **RS-04 测试落点（追加）**：`FastCheck/tests/test_check_engine.cpp` +`TestStrictSizeDiffLocalGreater`(AC-06) +`TestStrictDirIsMissing`(AC-04) +`#if _WIN32 TestStrictToctouFailed`(AC-09，无共享句柄锁使 driver read open 失败)；既有 strict 用例（Missing/SizeDiff/ordering/no-diskio/mixed/hash）不变即回归基线。`tests/test_disk_io_align.cpp` +`TestQueryAlignSameVolumeReuse`(AC-17) +`TestQueryAlignBounded`(AC-20，用 `AlignCacheSizeForTest`) +`TestQueryAlignConcurrent`(AC-19) +`#if _WIN32 TestQueryAlignMultiVolumeWindows`(AC-18，环境门控 ≥2 固定卷否则 skip)。`tests/test_disk_io_driver.cpp` +`RealBackendReadKnownSize`(AC-24/25，known vs unknown expectedSize 读回字节一致)。

## 热路径 syscall 降低（fastcheck-perf-tune）

`docs/tasks/fastcheck-perf-tune/`。在**不增加 auto 模式 worker 数**、不改 hash / 协议 / 分类 / mtime 单位 / 诊断输出 / 同步行为的前提下，减少四条热路径的每文件 kernel transition 与目录锁争用。四项改动互不耦合，按 A–E 提交拆分。

| Change | 文件 / 符号 | 做法 |
|---|---|---|
| 0 (commit A) hash-worker auto pin | `FastCheck/check_engine.cpp/.h` `ResolveMaxHashWorkers(hashWorkers, hardwareThreads)` | auto（`--hash-workers 0`）把 `maxWorkers` **钉为 `hardwareThreads`**（不再 AIMD 上探到 `4*hw`）；显式 `--hash-workers N` 保持 `max(N, 4*hw)`。上限公式抽为纯函数（与 `NextLocalWorkerCap`/`NextNetWindow` 同范式），`RunCheck` 调用之，值完全等价。`hashWorkerCap` 活跃门控/`--checkers`/HashRequest/HashResponse/drain/`--no-diskio-driver`/诊断行零改动。 |
| 1 (commit B) 服务端 manifest 复用 directory_entry | `FastClone/sync_engine_server.cpp` POSIX `listOneDir` | `fs::file_size(absPath, ec)` → `it->file_size(ec)`；`ToUnixNs(fs::last_write_time(absPath, ec))` → `ToUnixNs(it->last_write_time(ec))`。`ToUnixNs` 调用点、单位、错误置 0 逐字保留。**仅 POSIX 分支**：Windows 分支走 `FindFirstFileW`（`WIN32_FIND_DATAW` 已含 size+mtime，无 `fs::file_size` 可换），不触碰。 |
| 2 (commit C) EnsureParentDir 每 worker 目录缓存 | `FastClone/sync_util.h/.cpp` `PerWorkerDirCache` + `EnsureParentDir(path, cache)` + `EnsureParentDirCached<EnsureFn>`；`sync_engine_client.cpp` 四落点 | 每 worker 一份 `PerWorkerDirCache`（无锁、非线程安全、不跨 worker 共享）。命中 → 零 filesystem syscall；miss → 走现有 `CreateDirectoriesLong` 后 `DirExists` 探测，成功才 `addWithAncestors`（parent + 全部 ancestor，键切分复用 `CreateDirectoriesLong` 的卷根定位逻辑，保证与后续 `contains` 查询字符串一致）；失败不缓存（下次仍走 miss）。核心抽成模板 `EnsureParentDirCached<EnsureFn>`（命中 early-return 不触碰算子，零开销；测试注入计数/失败替身）。 |
| 3 (commit D) BuildIndex 枚举期捕获 file_size | `FastClone/file_index.cpp` `BuildIndex` | `Candidate` 增 `uint64_t fileSize`；迭代阶段对普通文件 `item.file_size(ec)`（失败即跳过该候选，等价旧 worker `fs::file_size` 失败→`continue` 的用户可见结果），worker 阶段用 `c.fileSize` 复用，**不再** `fs::file_size(c.absPath, sec)`。mtime 路径不动，继续 `ReadFileMtimeCanonical`。 |
| E (commit E) | 本节 | dev-map 追加记录。 |

### 客户端四落点（Change 2，FR-13）

| # | 位置 | worker 上下文 | cache 归属 |
|---|---|---|---|
| 1 | 整文件下载写 `EnsureParentDir(abs, dirCache)` | `ioWorkers` 池 lambda | lambda 顶部（while 前）`PerWorkerDirCache dirCache`，跨 batch 复用 |
| 2 | 单文件下载 open 前（`tryStartTransfers`） | 主调度循环（单线程） | 会话作用域 `PerWorkerDirCache mainThreadDirCache` |
| 3 | delta 重建 temp 文件（`buildDeltaPlanOffloaded`） | `deltaPlanWorkers` 池 | 给 `buildDeltaPlanOffloaded` 增 `PerWorkerDirCache&` 参数，worker while 顶部持一份传入 |
| 4 | batch 零字节文件 finalize（`processIncomingFrame`） | 主/帧处理（单线程） | 复用落点 2 的 `mainThreadDirCache`（`tryStartTransfers` 与 `processIncomingFrame` 均只在主循环线程 L3537/L3895 调用，同线程私有，无跨 worker 共享） |

### 行为等价红线（不得触碰）
- `Hash256` 长度 / XXH3 / hash 字节布局 / 比较规则；网络协议 / frame 编解码 / 握手 / HashRequest / HashResponse / manifest / delta payload。
- `compare_phase*` 分类规则 / 报告字段 / 退出码；mtime 单位 / 规范化 / `ReadFileMtimeCanonical`（Change 3 只碰 size，mtime 完全不动）。
- disk IO scheduler / 默认 disk IO 并发；**auto 模式 worker count 不得增加**（Change 0 只 pin 上限）；delta verify read-back；不新增 client hash memcache / 跨进程 / 持久化 / 全局目录 cache；既有诊断行不删 / 不改名 / 不弱化。

### 测试 / 构建
- 新增测试：`FastCheck/tests/test_check_engine.cpp` `TestResolveMaxHashWorkers`（auto hw=1/2/8/20 pin、显式 `max(N,4*hw)` 含 hw=1 边界，AC-03/04）；`tests/test_manifest_dirent.cpp` `RunManifestDirentTests`（`directory_entry` vs `fs` 的 size/mtime/ec 及 `ToUnixNs` 逐值等价 + 删除后错误路径，AC-06..10）；`tests/test_sync_util.cpp` `RunSyncUtilTests`（`EnsureParentDirCached` 注入计数替身：hit/miss/ancestor/fail-not-cached/多 worker，AC-12..16）；`tests/test_file_index.cpp` `RunBuildIndexFileSizeTests`（空/小/大/目录 size 捕获等价，AC-21；既有 mtime 断言守 AC-22）。两个新 FastClone 测试文件登记进 `CMakeLists.txt` `FastCloneTests` 源列表 + `tests/test_main.cpp` `RunManifestDirentTests`/`RunSyncUtilTests`。
- **测试闭包边界**：`sync_engine_server.cpp` / `sync_engine_client.cpp` 未链入 `FastCloneTests`（`CMakeLists.txt` 源列表实测），故 Change 1/2 用**被替换 API 的等价单测 + 静态审查**，非整条 server/client 走查（既有边界，NFR-07 禁止扩链）。
- 构建/运行：`cmake --build _bl_x64 --target FastCheckTests --config Release`、`... FastCloneTests ...`；`_bl_x64\Release\FastCheckTests.exe`、`_bl_x64\Release\FastCloneTests.exe`。
- **FastCheckTests 已知间歇性 hang**（S-06 记载的 pre-existing 问题，非本任务范畴）：运行时若 hang/超时/未通过，**最多重试 3 次**，3 次内一次通过即算通过；每次结果记入实现文档。
