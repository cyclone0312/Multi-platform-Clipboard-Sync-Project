# 纯 C++ 文件传输状态机六步实施计划

## 1. 文档目标

这份文档的目标是把当前 Qt 版本中的文件流式传输逻辑，逐步抽离成一个**不依赖 Qt 的纯 C++ 状态机核心**。

这里的“纯 C++ 状态机”指的是：

- 不直接依赖 `QObject`
- 不直接依赖 `QTimer`
- 不直接依赖 `QTcpSocket/QTcpServer`
- 不直接依赖 `QFile/QSaveFile`
- 不直接依赖 Qt 的信号槽

但它仍然保留你当前项目里的协议语义：

- `FileOffer`
- `FileRequest`
- `FileChunk`
- `FileComplete`
- `FileAbort`

最终目标不是立刻替换整个项目，而是先做出一个**可独立运行、可测试、可复用的纯 C++ 文件传输状态机核心**。

---

## 2. 总体思路

建议分 6 步推进：

1. 梳理现有状态和消息流
2. 定义纯 C++ 数据结构
3. 定义 `Action` 和 `StepResult`
4. 实现 `onRemoteFileRequest / onRemoteFileChunk`
5. 实现超时重试
6. 做最小 sender/receiver 控制台 Demo

这个顺序的核心原则是：

- 先梳理逻辑，再写结构
- 先抽状态机，再接 IO
- 先做闭环，再接回现有 Qt 工程

---

## 3. 建议目录结构

建议先在当前仓库中新增一个独立核心目录，而不是立刻新建整个项目：

```text
src/
  core/
    file_transfer_types.h
    file_transfer_actions.h
    file_transfer_state_machine.h
    file_transfer_state_machine.cpp
  demo/
    pure_cpp_sender_main.cpp
    pure_cpp_receiver_main.cpp
```

职责建议如下：

- `file_transfer_types.h`
  放纯 C++ 数据结构
- `file_transfer_actions.h`
  放状态机输出动作定义
- `file_transfer_state_machine.h/.cpp`
  放状态机本体
- `demo/`
  放最小控制台验证程序

---

## 4. 第一步：梳理现有状态和消息流

### 4.1 目标

先把当前 Qt 实现里的文件传输主链路画清楚，得到一份“纯逻辑视图”。

### 4.2 建议重点阅读位置

- [SyncCoordinator.h](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/sync/SyncCoordinator.h)
- [SyncCoordinator.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/sync/SyncCoordinator.cpp)
- [ProtocolHeader.h](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/protocol/ProtocolHeader.h)
- [MessageCodec.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/protocol/MessageCodec.cpp)
- [TransportClient.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/transport/TransportClient.cpp)
- [TransportServer.cpp](/d:/Workspace/Projects/Multi-platform-Clipboard-Sync-Project/src/transport/TransportServer.cpp)

### 4.3 这一阶段要产出的内容

建议你先整理出下面这张逻辑表：

| 消息/状态 | 由谁产生 | 谁处理 | 结果 |
| --- | --- | --- | --- |
| `FileOffer` | 发送端 | 接收端 | 建立远端可下载文件清单 |
| `FileRequest` | 接收端 | 发送端 | 请求某文件某偏移范围的数据 |
| `FileChunk` | 发送端 | 接收端 | 写入本地临时文件并推进下载进度 |
| `FileComplete` | 发送端 | 接收端 | 声明发送到文件末尾 |
| `FileAbort` | 任一端 | 对端 | 终止当前文件传输 |

### 4.4 这一阶段的验收标准

- 你能口头讲清楚 `FileOffer -> FileRequest -> FileChunk -> 完成/中止` 的流转过程
- 你能指出当前状态机核心函数在哪里
- 你能区分“状态机逻辑”和“Qt 基础设施”

---

## 5. 第二步：定义纯 C++ 数据结构

### 5.1 目标

把当前依赖 Qt 类型的状态和协议相关数据，先换成标准 C++ 类型。

### 5.2 建议定义的核心结构

建议至少先定义这几个：

```cpp
struct FileMeta;
struct FileOffer;
struct DownloadState;
struct TransferConfig;
struct FileRequestView;
struct FileChunkView;
```

### 5.3 推荐字段

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

struct TransferConfig {
    std::size_t chunkSizeBytes = 512 * 1024;
    std::size_t windowChunks = 16;
    int requestTimeoutMs = 8000;
    int maxRequestWindowRetries = 3;
};
```

### 5.4 这一阶段的验收标准

- 头文件中不再出现 Qt 容器和 Qt 基础类型
- 结构体足以承载你当前 `SyncCoordinator` 的文件传输状态
- 不涉及网络和文件 IO，仅定义状态表达

---

## 6. 第三步：定义 `Action` 和 `StepResult`

### 6.1 目标

把“状态推进”和“副作用执行”分离。

这是整个纯 C++ 改造里最关键的一步。

### 6.2 为什么要做这一步

你当前 Qt 代码是这样混在一起的：

- 收到消息
- 更新状态
- 直接发 socket
- 直接写文件
- 直接启动定时器

这样开发快，但状态机无法脱离 Qt 独立运行。

如果改成纯 C++，建议让状态机只负责：

- 接收事件
- 更新状态
- 产出“下一步动作”

而外层负责：

- 真正发送消息
- 真正读写文件
- 真正设置定时器

### 6.3 建议定义

```cpp
enum class ActionType {
    None,
    SendMessage,
    OpenTempFile,
    AppendTempFile,
    CommitTempFile,
    AbortTempFile,
    StartTimer,
    StopTimer,
    LogStatus,
    PublishCompletedFiles
};

struct Action {
    ActionType type = ActionType::None;
    std::vector<std::uint8_t> bytes;
    std::string path;
    std::string text;
    std::int64_t timeoutMs = 0;
};

struct StepResult {
    bool ok = true;
    std::vector<Action> actions;
};
```

### 6.4 这一阶段的验收标准

- 状态机函数不再直接依赖 socket/file/timer 类
- 每个处理函数都能通过 `StepResult` 输出动作
- 能够只靠伪造输入做单元测试

---

## 7. 第四步：实现 `onRemoteFileRequest / onRemoteFileChunk`

### 7.1 目标

先实现最核心的一发一收两条链路，因为它们就是文件传输的主体。

### 7.2 为什么先做这两个

因为这两个函数最能体现状态机的价值：

- `onRemoteFileRequest(...)`
  决定怎么按范围读取文件、怎么切分 `FileChunk`
- `onRemoteFileChunk(...)`
  决定怎么校验、写入、推进偏移、继续请求

这两条一旦能稳定工作，整个传输主干就已经成型了。

### 7.3 建议拆分的接口

```cpp
StepResult onRemoteFileRequest(const IncomingRequest& req);
StepResult onRemoteFileChunk(const IncomingChunk& chunk);
```

如果你想更干净一点，也可以这样拆：

- 解析层先把网络消息转成结构体
- 状态机只接收结构化输入

例如：

```cpp
struct IncomingFileRequest {
    std::uint64_t sessionId;
    std::string fileId;
    std::string requestId;
    std::int64_t offset = 0;
    std::int64_t length = 0;
};

struct IncomingFileChunk {
    std::uint64_t sessionId;
    std::string fileId;
    std::string requestId;
    std::int64_t offset = 0;
    std::uint32_t chunkCrc32 = 0;
    std::vector<std::uint8_t> bytes;
};
```

### 7.4 这一阶段的内部逻辑重点

`onRemoteFileRequest(...)` 里至少要做：

- 校验 `sessionId/fileId/requestId`
- 找到本地源文件
- 确定本次最多发送多少字节
- 按 `chunkSizeBytes` 切块
- 生成一个或多个 `SendMessage` 动作
- 到文件末尾时追加 `FileComplete`

`onRemoteFileChunk(...)` 里至少要做：

- 校验是否有活跃下载
- 校验 `fileId/requestId/offset`
- 校验 `CRC32`
- 追加写入动作
- 更新 `nextOffset/receivedInWindow`
- 判断是否切下一个窗口或下一个文件

### 7.5 这一阶段的验收标准

- 能在不依赖 Qt 的情况下完成 chunk 发送与接收推进
- 能正确处理顺序错误、CRC 错误、文件不存在等情况
- 处理结果通过 `StepResult` 对外表达

---

## 8. 第五步：实现超时重试

### 8.1 目标

为状态机补齐“窗口级超时和重试”能力。

### 8.2 当前语义建议保持一致

你现在 Qt 版本是“窗口级超时”，不是“单块超时”。

也就是：

- 发出一个 `FileRequest`
- 等待这一窗口的数据推进
- 超时后重发同一个请求窗口

纯 C++ 版建议保持这个思路，不要一开始就改成更复杂的逐块确认。

### 8.3 建议接口

```cpp
StepResult onTimeout(std::int64_t nowMs);
```

### 8.4 建议逻辑

- 没有活跃下载则忽略
- 如果 `nowMs < deadlineMs` 则忽略
- 如果超时且未超过最大重试次数：
  - 复用当前 `requestId`
  - 重新发当前窗口 `FileRequest`
  - 增加 `retryCount`
  - 更新新的 `deadlineMs`
- 如果超过最大重试次数：
  - 终止当前下载
  - 产出 `AbortTempFile` / `LogStatus`

### 8.5 这一阶段的验收标准

- 状态机能处理超时重发
- 重试次数可控
- 能在超时后稳定终止或继续

---

## 9. 第六步：做最小 sender/receiver 控制台 Demo

### 9.1 目标

验证纯 C++ 状态机真的可以脱离 Qt 独立工作。

### 9.2 Demo 的范围要尽量小

这个 Demo 不要一上来做：

- 剪贴板
- 拖拽
- GUI
- 多文件并发

第一版只做：

- 单文件
- 单会话
- sender/receiver 两个控制台程序
- 本地 TCP 或 loopback 通信

### 9.3 最小闭环

建议流程：

1. sender 读取一个本地文件并准备 `FileOffer`
2. receiver 收到 `FileOffer` 后生成第一个 `FileRequest`
3. sender 收到 `FileRequest` 后回多个 `FileChunk`
4. receiver 收到 `FileChunk` 后写入临时文件
5. 全部接收完成后做完整性校验
6. 校验通过后 rename 为最终文件

### 9.4 推荐实现方式

为了把重点放在状态机而不是网络细节上，Demo 可以有两种选择：

#### 方案 A：假网络

不真正起 socket，只做：

- sender 状态机产出消息
- 直接喂给 receiver

优点：

- 最快验证状态机本体

#### 方案 B：真 TCP 控制台

用标准 socket 或 `asio` 做一个最小双端程序。

优点：

- 更接近真实运行环境

建议顺序是：

先做 A，再做 B。

### 9.5 这一阶段的验收标准

- 不依赖 Qt 也能完成一次文件传输
- 能看见清晰日志：`Offer -> Request -> Chunk -> Complete`
- 生成的目标文件内容正确

---

## 10. 各阶段依赖关系

这 6 步最好按顺序推进，不建议跳。

依赖关系如下：

```text
第1步 梳理消息流
  -> 第2步 定义纯 C++ 数据结构
    -> 第3步 定义 Action / StepResult
      -> 第4步 实现 Request / Chunk 主逻辑
        -> 第5步 实现超时重试
          -> 第6步 做最小 Demo
```

其中：

- 第 2 步和第 3 步是“骨架”
- 第 4 步和第 5 步是“核心能力”
- 第 6 步是“验证闭环”

---

## 11. 每一步建议产出物

| 步骤 | 产出物 | 是否写代码 |
| --- | --- | --- |
| 第一步 | 消息流和状态图 | 否 |
| 第二步 | `file_transfer_types.h` | 是 |
| 第三步 | `file_transfer_actions.h` | 是 |
| 第四步 | `file_transfer_state_machine.h/.cpp` 初版 | 是 |
| 第五步 | 超时与重试逻辑 | 是 |
| 第六步 | `demo/sender`、`demo/receiver` | 是 |

---

## 12. 建议你当前的实际推进方式

如果按“风险最低”的方式推进，我建议你接下来这么做：

1. 先只完成第 1 步的状态图整理
2. 然后直接写第 2 步和第 3 步的头文件
3. 再只实现第 4 步里的 `onRemoteFileRequest(...)`
4. 确认发送侧逻辑清楚后，再实现 `onRemoteFileChunk(...)`
5. 最后补第 5 步和第 6 步

这样做的好处是：

- 你不会一下子进入“大改造”
- 每一步都能独立验证
- 导师如果中途看进度，你也能清楚展示当前阶段成果

---

## 13. 一句话总结

这 6 步的本质不是“重写一个新项目”，而是：

**先把你现有 Qt 工程里的文件传输状态机提纯，再用最小 Demo 证明它已经能脱离 Qt 独立运行。**

如果你愿意，我下一步可以直接继续帮你做第 1 步，把当前项目里的 `FileOffer / FileRequest / FileChunk` 流转整理成一张更细的“状态机时序文档”。  
