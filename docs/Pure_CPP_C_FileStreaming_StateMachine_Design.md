# 基于 Qt 的文件流式传输状态机，如何迁移到纯 C/C++ 实现

## 1. 先说结论

你当前的文件流式传输**不是 Qt 自带的状态机**，而是你自己在 [SyncCoordinator.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/sync/SyncCoordinator.cpp) 里写出来的一套业务状态机。

Qt 在这里主要扮演的是“基础设施提供者”：

- 事件分发：`QObject`、信号槽、`QMetaObject::invokeMethod`
- 定时器：`QTimer`
- 网络：`QTcpSocket`、`QTcpServer`
- 缓冲区和序列化：`QByteArray`、`QDataStream`、`QJsonObject`
- 文件落盘：`QFile`、`QSaveFile`
- 哈希：`QCryptographicHash`

所以如果你想改成纯 C/C++，真正要做的不是“重写状态机逻辑”，而是把这些 Qt 基础设施一层层替换掉，同时保留你现在这套状态流转思路。

---

## 2. 你现在的状态机核心，其实在哪里

当前文件流式传输的核心不在 Qt，而在下面这些结构和函数里：

- 状态定义在 [SyncCoordinator.h](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/sync/SyncCoordinator.h)
  - `FileMeta`
  - `FileOffer`
  - `DownloadState`
- 状态推进在 [SyncCoordinator.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/sync/SyncCoordinator.cpp)
  - `startPendingRemoteFilesRequest(...)`
  - `requestNextWindow()`
  - `sendFileRequestWindow(...)`
  - `handleRemoteFileRequest(...)`
  - `handleRemoteFileChunk(...)`
  - `handleRequestWindowTimeout()`

可以把这套状态机概括成下面这条链：

1. 发送端先发 `FileOffer`
2. 接收端决定下载，进入 `startPendingRemoteFilesRequest(...)`
3. 调度器 `requestNextWindow()` 决定当前该请求哪个文件、哪个偏移范围
4. `sendFileRequestWindow(...)` 发出一个 `FileRequest`
5. 发送端 `handleRemoteFileRequest(...)` 按 `offset + length` 读取本地文件
6. 发送端不断回 `FileChunk`
7. 接收端 `handleRemoteFileChunk(...)` 校验、写盘、更新 `nextOffset`
8. 一个窗口收满后，再次进入 `requestNextWindow()`
9. 一个文件完成后，切到下一个文件
10. 全部文件完成后，状态机结束

这部分逻辑本身完全可以搬到纯 C/C++。

---

## 3. 当前实现里，Qt 分别帮你做了什么

### 3.1 事件驱动和回调连接

当前大量依赖 Qt 对象模型：

- [SyncCoordinator.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/sync/SyncCoordinator.cpp) 里用 `QObject::connect(...)`
- [TransportClient.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/transport/TransportClient.cpp) 里用 socket 信号
- [TransportServer.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/transport/TransportServer.cpp) 里用 `readyRead/newConnection`

如果换成纯 C/C++，你要自己决定：

- 单线程事件循环
- 还是多线程 + 队列
- 还是 reactor/proactor 模型

最常见替代方式：

- 纯 C：`select/poll/epoll/kqueue/IOCP` + 函数回调
- 纯 C++：`std::function` + 事件循环
- 跨平台更省力：`asio` 或 `libuv`

### 3.2 定时器和重试

当前超时重试依赖 [SyncCoordinator.h](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/sync/SyncCoordinator.h) 里的：

- `QTimer m_requestWindowTimer`

以及 [SyncCoordinator.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/sync/SyncCoordinator.cpp) 里的：

- `handleRequestWindowTimeout()`

纯 C/C++ 里要自己做：

- 每个下载窗口记录 `deadline`
- 主循环周期性检查是否超时
- 超时后重发 `FileRequest`

常见做法：

- 小项目：每轮循环都检查 `now >= deadline`
- 中项目：最小堆 timer queue
- 更成熟：时间轮 / 独立 timer 线程

### 3.3 网络收发

当前依赖：

- [TransportClient.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/transport/TransportClient.cpp) 的 `QTcpSocket`
- [TransportServer.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/transport/TransportServer.cpp) 的 `QTcpServer`

当前网络层做了两件事：

1. 发送端把协议包再包一层 TCP frame
2. 接收端从字节流里按 frame 切包

比如：

- 发送时：前 4 字节写 frame 长度，再 append 真实 packet
- 接收时：buffer 不够 4 字节先等；够了再读 frameSize；不够完整帧继续等

这部分用纯 C/C++ 非常好替换，因为本质就是标准 TCP 粘包拆包。

你只需要：

- `socket/connect/send/recv`
- 一个发送缓冲区
- 一个接收缓冲区
- 一个 `while` 循环不断拆 frame

### 3.4 协议序列化

当前协议头在 [ProtocolHeader.h](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/protocol/ProtocolHeader.h)，编解码在：

- [MessageCodec.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/protocol/MessageCodec.cpp)

Qt 帮你做了：

- `QDataStream` 写协议头
- `QByteArray` 管理字节缓冲
- `QJsonDocument/QJsonObject` 管理 `FileOffer/FileRequest/FileChunk` 元数据

纯 C/C++ 可替代为：

- 协议头：手写 `struct` + 字节序转换
- 缓冲区：`std::vector<uint8_t>` / `std::string` / 自己的 ring buffer
- 元数据：
  - 继续用 JSON：`nlohmann::json`、`yyjson`、`cJSON`
  - 或直接改成二进制结构体

如果追求性能和可控性，我会建议：

- 协议头保留二进制
- `FileRequest/FileChunk` 的元数据也尽量二进制化

这样可以少掉 JSON 解析开销。

### 3.5 文件读写和原子提交

当前发送端用：

- `QFile` 读源文件

接收端用：

- `QSaveFile` 写目标文件

`QSaveFile` 的意义很重要：它通常先写临时文件，最后 `commit()` 再原子替换目标文件，避免写到一半留下坏文件。

纯 C/C++ 中你要自己实现这一层：

1. 先写 `xxx.tmp`
2. 每个 chunk 追加写入
3. 下载完成且校验通过后 `rename(tmp, final)`
4. 失败时删除 `.tmp`

### 3.6 哈希和块校验

当前有两层校验：

- chunk 级：`crc32(chunk)`
- 文件级：`QCryptographicHash(Sha256)`

纯 C/C++ 里要自己替换成：

- CRC32：zlib 或自己保留现有实现
- SHA256：OpenSSL、mbedTLS、libsodium，或者自己嵌一个 SHA256 实现

---

## 4. 如果改成纯 C/C++，建议你把工程拆成这 6 层

### 4.1 `protocol`

负责协议头、消息类型、编解码，不依赖 UI 和平台。

建议职责：

- 定义 `MessageType`
- 定义 `MessageHeader`
- `encode_message(...)`
- `decode_message(...)`
- `build_file_request(...)`
- `parse_file_chunk(...)`

### 4.2 `transport`

负责 TCP 连接、frame 拆包、重连、发送缓存。

建议职责：

- client connect/reconnect
- server accept/read
- tx/rx buffer
- on_message 回调

### 4.3 `storage`

负责文件读写和临时文件提交。

建议职责：

- 打开源文件
- 从 offset 读取 chunk
- 创建下载中的临时文件
- 追加写入
- commit 或 abort

### 4.4 `hash`

负责：

- `crc32_update`
- `sha256_init/update/final`

### 4.5 `state_machine`

这是最关键的一层，建议保持“纯业务逻辑，不碰 UI，不碰具体 socket，不碰具体平台 API”。

它只做：

- 接收事件
- 更新状态
- 决定下一步动作

比如它收到事件后，产生命令：

- `SEND_FILE_REQUEST`
- `SEND_FILE_CHUNK`
- `OPEN_DOWNLOAD_FILE`
- `COMMIT_DOWNLOAD_FILE`
- `ABORT_DOWNLOAD`

### 4.6 `platform_adapter`

这一层才去接：

- 剪贴板
- 拖放
- 系统热键
- UI 窗口

这样以后你即使不要 Qt，也只是在最外层换皮，状态机可以保住。

---

## 5. 纯 C/C++ 版本的数据结构大概长什么样

### 5.1 纯 C 风格

```c
typedef struct FileMeta {
    char file_id[64];
    char path[1024];
    char name[256];
    int64_t size;
    int64_t mtime_ms;
    char sha256[65];
} FileMeta;

typedef struct FileOffer {
    uint64_t session_id;
    int64_t received_at_ms;
    FileMeta* files;
    size_t file_count;
} FileOffer;

typedef struct DownloadState {
    uint64_t session_id;
    size_t file_index;
    int64_t next_offset;
    int64_t requested_length;
    int64_t received_in_window;
    char request_id[64];
    char local_path[1024];
    int retry_count;
    int paused_by_disconnect;
    int64_t deadline_ms;
} DownloadState;
```

### 5.2 纯 C++ 风格

```cpp
struct FileMeta {
    std::string fileId;
    std::string path;
    std::string name;
    std::int64_t size = 0;
    std::int64_t mtimeMs = 0;
    std::string sha256;
};

struct FileOffer {
    std::uint64_t sessionId = 0;
    std::int64_t receivedAtMs = 0;
    std::vector<FileMeta> files;
};

struct DownloadState {
    std::uint64_t sessionId = 0;
    std::size_t fileIndex = 0;
    std::int64_t nextOffset = 0;
    std::int64_t requestedLength = 0;
    std::int64_t receivedInWindow = 0;
    std::string requestId;
    std::string localPath;
    int retryCount = 0;
    bool pausedByDisconnect = false;
    std::int64_t deadlineMs = 0;
};
```

你会发现，这和你现在 [SyncCoordinator.h](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/sync/SyncCoordinator.h) 的结构几乎是同一个东西，只是把 Qt 类型换掉了。

---

## 6. 纯 C/C++ 状态机应该怎么写

核心建议：**把“状态更新”和“副作用执行”分开。**

也就是：

- 状态机函数只决定“下一步应该做什么”
- 真正的 socket 发送、文件写入、日志输出，由外层执行

### 6.1 你现在的 Qt 版本，实际是“状态机 + 副作用”写在一起

例如在 [SyncCoordinator.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/sync/SyncCoordinator.cpp)：

- `sendFileRequestWindow(...)` 里直接 `m_client->sendMessage(...)`
- `handleRemoteFileChunk(...)` 里直接 `m_downloadFile->write(...)`
- `requestNextWindow()` 里直接 `commit()` 和发信号

这在 Qt 项目里很常见，开发效率高，但耦合会比较重。

### 6.2 如果你要纯 C/C++，更推荐下面这种写法

```cpp
enum class ActionType {
    None,
    SendFileRequest,
    SendFileChunk,
    OpenTempFile,
    WriteChunkToFile,
    CommitFile,
    AbortFile,
    PublishCompletedFiles
};

struct Action {
    ActionType type = ActionType::None;
    std::vector<std::uint8_t> bytes;
    std::string path;
};

struct StepResult {
    bool ok = true;
    std::vector<Action> actions;
};
```

然后状态机接口像这样：

```cpp
StepResult on_file_offer(StateMachine* sm, const Message& msg);
StepResult on_file_chunk(StateMachine* sm, const Message& msg);
StepResult on_timeout(StateMachine* sm, std::int64_t now_ms);
```

外层伪代码：

```cpp
StepResult r = on_file_chunk(&sm, msg);
for (const Action& a : r.actions) {
    execute_action(io_ctx, a);
}
```

这样做的好处是：

- 单元测试非常容易写
- 不依赖 Qt 事件系统
- 将来换成 Windows 原生、Linux 原生、甚至嵌入式都更容易

---

## 7. 纯 C/C++ 发送端流程怎么实现

### 7.1 收到 `FileRequest`

发送端做这些事：

1. 解析 `fileId/requestId/offset/length`
2. 在本地 `local_offers` 里找到对应文件
3. 打开文件
4. `seek(offset)`
5. 按 `chunk_size` 循环读取
6. 每块生成 `FileChunk`
7. 写入发送队列
8. 如果到达文件末尾，再发 `FileComplete`

伪代码：

```cpp
void handle_remote_file_request(SenderCtx* ctx, const FileRequest& req) {
    FileMeta* meta = find_local_file(ctx, req.sessionId, req.fileId);
    if (!meta) {
        send_abort(ctx, req.sessionId, req.fileId, req.requestId, "file not found");
        return;
    }

    FileHandle f = file_open_read(meta->path.c_str());
    if (!f.ok || !file_seek(&f, req.offset)) {
        send_abort(ctx, req.sessionId, req.fileId, req.requestId, "source unavailable");
        return;
    }

    int64_t sent = 0;
    int64_t max_to_send = min(req.length, meta->size - req.offset);
    while (sent < max_to_send) {
        int64_t n = min(ctx->chunkSize, max_to_send - sent);
        Bytes chunk = file_read(&f, n);
        if (chunk.size == 0) {
            break;
        }

        FileChunk msg = make_file_chunk(req, req.offset + sent, chunk);
        transport_send(ctx->transport, encode_file_chunk(msg));
        sent += chunk.size;
    }

    if (req.offset + sent >= meta->size) {
        send_complete(ctx, req, meta->size, meta->sha256);
    }
}
```

这其实就是你当前 [SyncCoordinator.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/sync/SyncCoordinator.cpp) 里 `handleRemoteFileRequest(...)` 的纯 C/C++ 版本。

---

## 8. 纯 C/C++ 接收端流程怎么实现

### 8.1 收到 `FileOffer`

接收端：

1. 解析 offer
2. 存入 `remote_offers`
3. 如果当前没有活跃下载，就创建 `DownloadState`
4. 调度发第一条 `FileRequest`

### 8.2 收到 `FileChunk`

接收端：

1. 确认有活跃下载
2. 校验 `sessionId`
3. 校验 `fileId/requestId/offset`
4. 校验 chunk CRC32
5. 追加写入临时文件
6. 更新 `nextOffset/receivedInWindow`
7. 更新 SHA256
8. 如果当前文件完成，commit 文件
9. 如果当前窗口收满，发下一条 `FileRequest`

伪代码：

```cpp
void handle_remote_file_chunk(ReceiverCtx* ctx, const FileChunk& chunk) {
    DownloadState* st = &ctx->download;
    if (!st->active) return;

    FileMeta* meta = current_remote_file(ctx, st);
    if (!meta) return;

    if (chunk.fileId != meta->fileId) abort_download(ctx, "file id mismatch");
    if (chunk.requestId != st->requestId) abort_download(ctx, "request id mismatch");
    if (chunk.offset != st->nextOffset) abort_download(ctx, "offset mismatch");
    if (crc32(chunk.bytes) != chunk.chunkCrc32) abort_download(ctx, "crc mismatch");

    if (!temp_file_write(&ctx->tempFile, chunk.bytes)) {
        abort_download(ctx, "write failed");
        return;
    }

    sha256_update(&ctx->sha256, chunk.bytes);
    st->nextOffset += chunk.bytes.size();
    st->receivedInWindow += chunk.bytes.size();
    st->deadlineMs = now_ms() + ctx->requestTimeoutMs;
    st->retryCount = 0;

    if (st->nextOffset >= meta->size) {
        finalize_current_file(ctx);
        request_next_window_or_next_file(ctx);
        return;
    }

    if (st->receivedInWindow >= st->requestedLength) {
        send_next_file_request(ctx);
    }
}
```

这对应你当前的 `handleRemoteFileChunk(...)`。

---

## 9. 纯 C/C++ 里最关键的 3 个难点

### 9.1 定时器不再是白送的

Qt 里你只要：

```cpp
m_requestWindowTimer.start();
```

纯 C/C++ 里你要自己保证：

- 哪个请求什么时候发出的
- 多久算超时
- 超时时是否还在线
- 是否复用同一个 `requestId`
- 达到最大重试次数后怎么回收状态

所以定时器系统是第一个必须补齐的点。

### 9.2 线程模型必须先定下来

Qt 默认帮你把很多事收进事件循环里了。

纯 C/C++ 你得先选一种模型：

#### 模型 A：单线程事件循环

优点：

- 状态一致性最好
- 最适合你现在这类状态机

缺点：

- 文件哈希、大文件读写容易阻塞

#### 模型 B：网络线程 + 文件线程

优点：

- 性能更稳定

缺点：

- 要开始处理锁、队列和并发状态

对你当前项目，我更建议：

- **先做单线程 reactor**
- 文件读写如果将来卡顿，再拆线程

### 9.3 `QSaveFile` 这类“安全落盘”要自己补

很多人重写时会只想着 socket 和协议，忽略落盘安全。

但你这个文件传输链路里，真正容易留下脏状态的地方是：

- 下到一半崩溃
- 校验失败
- 超时中止
- 用户中断

所以纯 C/C++ 重写时，临时文件提交策略一定要保留。

---

## 10. Qt 到纯 C/C++ 的替换表

| 当前 Qt 组件 | 当前作用 | 纯 C/C++ 替代建议 |
| --- | --- | --- |
| `QObject + signal/slot` | 事件派发 | 回调函数、事件队列、观察者模式 |
| `QTimer` | 请求窗口超时 | deadline 检查、timer heap、timer thread |
| `QTcpSocket` | TCP 连接与收发 | BSD socket / Winsock / ASIO |
| `QTcpServer` | 监听连接 | BSD socket / Winsock accept |
| `QByteArray` | 可变字节缓冲 | `std::vector<uint8_t>` / ring buffer |
| `QDataStream` | 协议头序列化 | 手写 pack/unpack |
| `QJsonObject/QJsonDocument` | 元数据编码 | `nlohmann::json` / `cJSON` / 自定义二进制协议 |
| `QFile` | 源文件读取 | `fopen/fread` / `std::ifstream` / OS file API |
| `QSaveFile` | 安全落盘 | `.tmp + rename` |
| `QCryptographicHash` | SHA256 | OpenSSL / mbedTLS / 自实现 |
| `QMetaObject::invokeMethod` | 投递异步任务 | 任务队列 / post task |

---

## 11. 如果让我从零写一个“纯 C/C++ 版本”，我会怎么落地

### 第一步：先把状态机从 Qt 里剥出来

目标不是先去掉 Qt，而是先把逻辑收拢成纯业务模块。

建议先做一个新模块：

- `src/core/file_transfer_state_machine.h`
- `src/core/file_transfer_state_machine.cpp`

里面只放：

- `FileMeta`
- `FileOffer`
- `DownloadState`
- `on_file_offer`
- `on_file_request`
- `on_file_chunk`
- `on_timeout`

让它先继续被 Qt 项目调用。

### 第二步：把“副作用”抽象成接口

例如：

```cpp
struct TransferIo {
    virtual bool send_message(const Message& msg) = 0;
    virtual bool open_temp_file(const std::string& path) = 0;
    virtual bool append_file(const void* data, size_t size) = 0;
    virtual bool commit_file() = 0;
    virtual void abort_file() = 0;
    virtual int64_t now_ms() const = 0;
};
```

然后 Qt 版本先实现一版 `QtTransferIo`。

### 第三步：再把 transport 换掉

当状态机已经不依赖 Qt 时，再去替换：

- `QTcpSocket/QTcpServer`
- `QTimer`
- `QByteArray/QDataStream`

这样风险最小。

### 第四步：最后才处理剪贴板和拖放

因为：

- 文件传输状态机和平台 UI 不是一个层级
- 真正难迁移的是平台输入输出，不是中间状态机

---

## 12. 如果你想用纯 C 而不是纯 C++，要额外注意什么

纯 C 当然也能写，但会更偏“系统编程风格”。

你要自己处理：

- 内存生命周期
- 字符串缓冲区大小
- 动态数组扩容
- 错误码而不是异常/RAII
- 文件句柄和 socket 句柄回收

如果目标是“以后做长期维护的工程”，我更建议：

- **状态机用现代 C++**
- **平台/系统边界允许用 C 风格 API**

这样通常是最平衡的方案。

---

## 13. 一句话总结

你现在的文件流式传输，**业务状态机是你自己写的，Qt 只是给它提供了事件循环、socket、定时器、字节缓冲、JSON、文件和哈希能力**。

如果要换成纯 C/C++，最稳的做法不是直接把 Qt 全删掉，而是：

1. 先把状态机核心从 `SyncCoordinator` 中抽出来
2. 用接口隔离网络、计时器、文件和哈希
3. 再逐步把 Qt 基础设施替换成 socket/timer/file/hash 的原生实现

这样你现在的 `FileOffer -> FileRequest -> FileChunk -> commit` 这条主逻辑可以基本原样保留下来。

---

## 14. 你接下来如果想继续深入，建议按这个顺序看

1. [SyncCoordinator.h](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/sync/SyncCoordinator.h)
   先看状态结构体，理解状态里到底存了什么
2. [SyncCoordinator.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/sync/SyncCoordinator.cpp)
   重点看 `requestNextWindow(...)`、`sendFileRequestWindow(...)`、`handleRemoteFileRequest(...)`、`handleRemoteFileChunk(...)`
3. [TransportClient.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/transport/TransportClient.cpp)
   看发送缓冲和 frame 封包
4. [TransportServer.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/transport/TransportServer.cpp)
   看接收缓冲和拆包
5. [MessageCodec.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/protocol/MessageCodec.cpp)
   看协议头是怎么编码和校验的

如果你愿意，我下一步还可以继续给你补一份：

- “纯 C++ 版文件传输状态机类设计草图”

或者直接在这个仓库里给你起一个最小 `core/file_transfer_state_machine.h/.cpp` 骨架。  
