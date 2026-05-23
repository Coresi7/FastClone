# FastClone

`FastClone.exe` 是一个单文件、单连接的高吞吐目录同步工具，针对 Unity/Unreal 等海量碎文件场景做了优化。

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
FastClone.exe server [--dir <path>] [--port <n>] --password <pwd>
```

- `--dir`：服务根目录；默认当前目录
- `--port`：监听端口；默认 `27842`
- `--password`：预共享口令（必填）

### 客户端

```bash
FastClone.exe client --server <host[:port]> --target <path> --password <pwd> [--streams <n>] [--chunk-kb <n>]
```

- `--server`：支持 `10.0.0.8:27842` 或 `10.0.0.8`（省略端口默认 `27842`）
- `--target`：本地目标目录
- `--password`：口令（与服务端一致）
- `--streams`：并发 stream 数；不传走 auto-tune
- `--chunk-kb`：块大小（KB）；不传走 auto-tune，范围 `1..65536`

## 进度输出

客户端单行实时刷新以下计数：

- `Enumrated`：已枚举远端文件数
- `Compared`：已完成判定数
- `Skipped`：无需传输数
- `Transfered`：已传输完成数
- `Deleted`：镜像删除数（删除阶段结束后更新）

## 注意事项

- 当前为明文 TCP + 口令，建议只在可信网络使用
- 镜像模式会删除客户端多余文件/目录
- 不支持断点续传，中断后需重跑

