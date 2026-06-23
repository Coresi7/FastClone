# FastClone

`FastClone` 是一个单文件、多连接（多路径）的高吞吐目录同步工具，面向超大规模目录树：可**迅速同步千万量级文件、传输 TB 级数据**，并在一次同步中同时高效处理海量碎文件与超大文件（例如 Unity/Unreal 工程目录、游戏构建产物、大型素材/数据集仓库）。

现在支持：
- Windows：使用 WinAPI 快路径（更高性能）
- GNU/Linux / macOS：使用跨平台通用实现（功能可用，性能可能略低于 Windows）

## 核心特性

- 单 EXE：同一个程序可作为 `server` 或 `client` 运行
- 多连接并行传输（多路径 / FC6）：可跨多块网卡叠加带宽；单网卡 / 单连接同样适用
- 镜像同步：服务端删除 -> 客户端也删除
- 回退比对：`size + mtime` 不一致时使用 `XXH3_128`
- 面向千万量级文件、TB 级数据的超大目录树：碎文件批量打包传输，多个文件在多条流 / 多条链路上并发（单个文件本身不跨流拆分）
- 协议版本强校验（版本不匹配直接拒绝）

## 预期性能
- i9 9900K + SSD(SATA)同步300w量级的Library，只需要约20秒（差量文件不大的情况下。否则取决于网卡速度）。

## 使用方式

### 服务端

```bash
FastClone server [--dir <path>] [--port <n>] [--server-hash-workers <n>] [--enable-hash-memcache] [--once] --password <pwd>
```

- `--dir`：服务根目录；默认当前目录
- `--port`：监听端口；默认 `27842`
- `--server-hash-workers`：服务进程级 hash 线程数（对所有 session 生效）；`0` 表示自动，范围 `0..512`
- `--enable-hash-memcache`：启用服务端内存 hash 缓存；当 `path + mtime + size` 一致时复用缓存 hash
- `--once`：一次性服务端模式——服务完一个真实会话后进程退出（仅服务端可用，与 `--enable-hash-memcache` 互斥）
- `--password`：预共享口令（必填）

### 客户端

```bash
FastClone client --server <host[:port]>[,host:port...] --target <path> --password <pwd> [--streams <n>] [--chunk-kb <n>] [--queued-file-size <size>] [--large-file-threshold <size>] [--link <localIP|iface>=<serverIP[:port]>]... [--reconnect-retries <n>] [--reconnect-window <duration>]
```

- `--server`：支持 `10.0.0.8:27842` 或 `10.0.0.8`（省略端口默认 `27842`）；可用逗号分隔或重复传入多个端点，作为多路径的服务端地址
- `--target`：本地目标目录
- `--password`：口令（与服务端一致）
- `--streams`：并发 stream 数；不传走 auto-tune（默认按 `4`，显式设置大于 `8` 会打印失败率风险警告）
- `--chunk-kb`：块大小（KB）；不传走 auto-tune，范围 `1..65536`
- `--queued-file-size`：接收队列内存软目标（用于自适应限速）；默认 `5G`，范围 `256M..64G`，支持 `K/M/G` 后缀
- `--large-file-threshold`：将 `>=` 该大小的文件固定走首要链路；默认 `1G`，范围 `1M..1T`，支持 `K/M/G` 后缀（与碎文件批处理阈值、接收队列阈值相互独立）
- `--link`：显式指定 `<本地IP|网卡名>=<服务端IP[:端口]>` 的链路配对（可重复）；指定后跳过自动选路，列表第一条为首要链路
- `--reconnect-retries`：网络闪断时会话重连次数上限；默认 `10`，`0` 禁用
- `--reconnect-window`：重连总时间窗口；默认 `30m`，支持 `s`/`m`/`h` 后缀

## 一次性服务端模式（`--once`）

`--once` 标志启用面向 CI/CD 场景的一次性服务端模式：服务端接受**恰好一个真实会话**，完成传输后立即退出。

**行为：**

- 服务端监听等待连接。握手前的探测连接（如可达性检查，在发送任何字节前即关闭）会被静默忽略，不计入真实会话。
- 当唯一的真实会话干净完成（所有连接正常结束，无任何 lane 错误）时，服务端以退出码 `0` 退出。
- 若该会话的任意一条 lane 发生错误，服务端以退出码 `5` 退出（会话失败/中止）。
- 若首个会话仍在传输中时有第二个独立会话到达，会被拒绝服务。
- 与 `--enable-hash-memcache` 互斥（服务端进程仅服务一次即退出，长生命周期缓存无意义）。

**退出流程：**

1. 最后一个连接的工作线程设定终止判定并关闭监听 socket。
2. 主 accept 循环被 socket 关闭唤醒，读取已记录的判定结果，返回对应退出码。
3. 进程干净退出——整个过程中无 `exit()` 或 `abort()` 调用。

此模式**仅服务端可用**；在客户端传入 `--once` 会导致 CLI 报错。

## 进度输出

客户端单行实时刷新以下计数：

- `Enumrated`：已枚举远端文件数
- `Compared`：已完成判定数
- `Unchanged`：无需传输数
- `Failed`：传输失败且重试（最多 3 次）后仍失败的文件数
- `Transfered`：已传输完成数
- `Connections`：当前已建立的连接（链路）数

镜像删除在所有传输完成后进行，不计入上面的实时计数；删除阶段结束时单独打印 `Delete done, <n> files`。

## 网络闪断与自动重连

客户端在连接中断（manifest 未完整接收）时会自动重连并继续同步，无需手动重跑整条命令：

- 默认最多 **10** 次会话重连，总窗口 **30 分钟**，指数退避（1s → 2s → 4s … 上限 30s）
- 重连等待期间若服务端尚未就绪（`connect failed` 等），同样计入重连预算并退避重试，不会立即退出
- `--reconnect-retries 0` 可禁用自动重连（行为与旧版一致：中断后退出码 `3`）
- 已落盘且 `size+mtime` 一致的文件在重连后自动跳过，无需二次传输
- **非块级断点续传**：大文件若传输中断，重连后从文件头重传（协议无 offset 字段）
- **当前线协议为 FC6**（多路径会话：新增 `SessionJoin`，`AuthOk` 携带会话标识与服务端端点列表）；多连接下单条链路断开会把其在途文件迁移到健康链路重传，仅当所有链路都断开才触发会话级重连
- 协议/鉴权错误、帧 desync 等不可恢复错误立即退出（退出码 `1`），不消耗重连预算
- 已知不可恢复错误包括：密码不匹配、协议版本不匹配（FC6）、服务端 Error 帧、帧 desync

新增 CLI 参数：

- `--reconnect-retries <n>`：最大会话重连次数；默认 `10`，`0` = 禁用
- `--reconnect-window <duration>`：重连总时间窗口；默认 `30m`，支持 `s`/`m`/`h` 后缀

## 多网卡并行传输（多路径，FC6）

当服务端与客户端各自拥有多块网卡时，FastClone 可在一次同步中**同时使用多条物理链路**以叠加带宽：

- 一个会话由一个**连接池**组成：先建立首要链路（`--server` 的第一个端点 / `--link` 的第一条），再尽力建立辅助链路。
- **自动选路**：客户端枚举本机网卡、对服务端下发的端点做可达性探测，按"地址族（IPv4 优先）> 同子网 > RTT"择优配对，并保证**每块物理网卡（客户端与服务端两侧）至多一条连接**（双栈网卡的 v4/v6 不会被当成两条链路）。
- **显式指定**：用 `--link <本地IP|网卡名>=<服务端IP[:端口]>` 可绕过自动选路，列表第一条为首要链路。
- **大文件固定走首要链路**：单个文件无法跨链路拆分，因此大小 `>= --large-file-threshold`（默认 `1G`）的文件固定走首要链路（假定其为最佳链路）；其余文件 / 碎文件批按实测吞吐自适应分摊到各链路。
- 单网卡 / 单端点时自动退化为单连接，行为与此前一致。

#### 如何启用 / 禁用可达性探测

**服务端** — 端点下发始终开启。客户端首次连接时，服务端自动枚举本机所有网卡地址，按物理接口分组后通过 `AuthOk` 帧下发给客户端。当前无 CLI 参数可关闭端点下发；如需阻止客户端探测额外端点，可在网络/防火墙层面限制只有一块网卡可达。

**客户端** — 可达性探测是否触发取决于是否指定了 `--link`：

| 模式 | 条件 | 行为 |
|------|------|------|
| 自动（探测启用） | 未指定 `--link`，且服务端下发 > 1 个端点**或**客户端有 > 1 个探测候选 | 客户端枚举本机网卡，对所有服务端端点做可达性探测，然后择优配对链路 |
| 自动退化（探测跳过） | 未指定 `--link`，但服务端仅 1 个端点 + 客户端仅 1 个探测候选 | 跳过探测，直接单链路 |
| 显式（探测绕过） | 指定了一条或多条 `--link` | 客户端直接使用指定的链路，不进行任何探测 |

若需**完全禁用**探测，请使用 `--link` 显式指定链路。即使只给一条 `--link`（仅首要链路），也会跳过所有自动选路与探测。

诊断工具：解决方案中附带一个**可选编译**的 `FastCloneRouteProbe` 工程，可对指定服务端做真实可达性探测并打印可达性矩阵与最终选路结果，便于现场排查链路分配。

## 注意事项

- 当前为明文 TCP + 口令，建议只在可信网络使用
- 镜像模式会删除客户端多余文件/目录
- 不支持单文件块级断点续传；中断后依赖自动重连或重跑（重跑/重连均可自动对比，相同文件无需二次传输）

## 退出码

**客户端：**
- `0`：同步成功（无失败文件）
- `1`：参数错误或运行时异常
- `2`：同步完成但存在失败文件（可结合 `--streams` 降低并发后重试）
- `3`：同步未完成且自动重连已禁用（`--reconnect-retries 0`），或连接中断后未启用重连
- `4`：自动重连预算耗尽，同步仍未完成

**服务端 `--once`：**
- `0`：唯一真实会话干净完成
- `1`：参数/用法错误（如 `--once` 与 `--enable-hash-memcache` 同用，或客户端误用 `--once`）
- `5`：唯一真实会话失败或中止（任意 lane 发生错误）——与客户端的 `2` 区分，确保每个退出码语义唯一

## 跨平台构建（Linux/macOS）

依赖：
- C++20 编译器
- CMake 3.16+
- xxhash 开发包（不同发行版包名不同）

macOS 安装依赖：

```bash
brew install cmake xxhash
```

安装 xxhash 开发包（Linux）：

- Debian / Ubuntu

```bash
sudo apt update
sudo apt install -y libxxhash-dev
```

- Fedora / RHEL / Rocky / Alma / CentOS Stream

```bash
sudo dnf install -y xxhash-devel
```

如果 `xxhash-devel` 找不到，先启用额外仓库后重试：

```bash
sudo dnf install -y epel-release
sudo dnf config-manager --set-enabled crb
sudo dnf makecache
sudo dnf install -y xxhash-devel
```

- Arch / Manjaro

```bash
sudo pacman -S --needed xxhash
```

- openSUSE

```bash
sudo zypper install -y xxhash-devel
```

可选自检（确认头文件和 pkg-config 可见）：

```bash
pkg-config --modversion libxxhash
pkg-config --cflags --libs libxxhash
```

构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

安装到系统路径（类似 `make install`）：

```bash
cmake --install build
```

自定义安装前缀（示例）：

```bash
cmake --install build --prefix /usr/local
```

安装后可直接在任意路径调用：

```bash
FastClone --help
```