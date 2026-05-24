# FastClone

`FastClone` 是一个单文件、单连接的高吞吐目录同步工具，针对 Unity/Unreal 等海量碎文件场景做了优化。

现在支持：
- Windows：使用 WinAPI 快路径（更高性能）
- GNU/Linux / macOS：使用跨平台通用实现（功能可用，性能可能略低于 Windows）

## 核心特性

- 单 EXE：同一个程序可作为 `server` 或 `client` 运行
- 单 TCP 连接多路并发传输
- 镜像同步：服务端删除 -> 客户端也删除
- 回退比对：`size + mtime` 不一致时使用 `XXH3_128`
- 支持超大目录与小文件批处理传输
- 协议版本强校验（版本不匹配直接拒绝）

## 预期性能
- i9 9900K + SSD(SATA)同步300w量级的Library，只需要3分钟（差量文件不大的情况下。否则取决于网卡速度）。

## 使用方式

### 服务端

```bash
FastClone server [--dir <path>] [--port <n>] --password <pwd>
```

- `--dir`：服务根目录；默认当前目录
- `--port`：监听端口；默认 `27842`
- `--password`：预共享口令（必填）

### 客户端

```bash
FastClone client --server <host[:port]> --target <path> --password <pwd> [--streams <n>] [--chunk-kb <n>]
```

- `--server`：支持 `10.0.0.8:27842` 或 `10.0.0.8`（省略端口默认 `27842`）
- `--target`：本地目标目录
- `--password`：口令（与服务端一致）
- `--streams`：并发 stream 数；不传走 auto-tune（默认按 `4`，显式设置大于 `8` 会打印失败率风险警告）
- `--chunk-kb`：块大小（KB）；不传走 auto-tune，范围 `1..65536`

## 进度输出

客户端单行实时刷新以下计数：

- `Enumrated`：已枚举远端文件数
- `Compared`：已完成判定数
- `Unchanged`：无需传输数
- `Failed`：传输失败且重试（最多 3 次）后仍失败的文件数
- `Transfered`：已传输完成数
- `Deleted`：镜像删除数（删除阶段结束后更新）

## 注意事项

- 当前为明文 TCP + 口令，建议只在可信网络使用
- 镜像模式会删除客户端多余文件/目录
- 不支持断点续传，中断后需重跑（但重跑可自动对比，相同文件无需二次传输）

## 退出码

- `0`：同步成功（无失败文件）
- `1`：参数错误或运行时异常
- `2`：同步完成但存在失败文件（可结合 `--streams` 降低并发后重试）

## 跨平台构建（Linux/macOS）

依赖：
- C++20 编译器
- CMake 3.16+
- xxhash 开发包（不同发行版包名不同）

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

