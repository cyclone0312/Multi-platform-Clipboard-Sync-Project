# Multi-platform Clipboard Sync (Qt 5.15.2)

这是一个基于 Qt 5.15.2 + CMake 的跨平台剪贴板同步 MVP 工程骨架。

当前能力：

- 监听本地剪贴板文本变化
- TCP 发送文本到对端
- 接收对端文本并写回本地剪贴板
- 基于时间窗+哈希的简单防回环

## 1. 构建

确保本机已安装 Qt 5.15.2（包含 Core/Gui/Network）与 CMake。

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH="D:/Qt/5.15.2/msvc2019_64"
cmake --build build --config Release
```

如果是 MinGW 套件，请把 `CMAKE_PREFIX_PATH` 指到对应目录，例如：

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH="D:/Qt/5.15.2/mingw81_64"
cmake --build build
```

## 2. 运行

用两台机器或同机两个进程互连测试。

进程 A（监听 45454，连到 45455）：

```powershell
$env:CSYNC_LISTEN_PORT="45454"
$env:CSYNC_PEER_HOST="127.0.0.1"
$env:CSYNC_PEER_PORT="45455"
./build/clipboard_sync.exe
```

进程 B（监听 45455，连到 45454）：

```powershell
$env:CSYNC_LISTEN_PORT="45455"
$env:CSYNC_PEER_HOST="127.0.0.1"
$env:CSYNC_PEER_PORT="45454"
./build/clipboard_sync.exe
```

## 3. 可配置环境变量

- `CSYNC_LISTEN_PORT` 本地监听端口
- `CSYNC_PEER_HOST` 对端地址
- `CSYNC_PEER_PORT` 对端端口
- `CSYNC_NODE_ID` 可选节点标识（默认自动生成）

## 4. 后续扩展建议

- 在协议层启用 `TEXT_HTML`、`TEXT_RTF`、`IMAGE_BITMAP`
- 把 `ClipboardMonitor/Writer` 扩展到 `QMimeData` 多格式读写
- 增加 ACK/NACK 与断线重发队列
