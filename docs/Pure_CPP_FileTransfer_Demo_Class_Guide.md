# Pure C++ File Transfer Demo Class Guide

## 1. 这份 demo 到底是什么

这不是一个真正起 TCP 连接的 sender/receiver 双进程程序。

它是一个**单进程、假网络、纯 C++ 文件传输闭环 demo**：

- 同一个进程里同时创建 `sender` 和 `receiver` 两个状态机
- `receiver` 产出 `SendFileRequest`
- demo 不走 socket，而是直接把请求喂给 `sender`
- `sender` 再产出 `SendFileChunk`
- demo 再把 chunk 直接喂回 `receiver`

所以它的重点不是“网络编程”，而是验证这套**文件传输状态机本身**已经能脱离 Qt 独立工作。

---

## 2. 建议阅读顺序

第一次看建议按这个顺序：

1. `src/demo/pure_cpp_file_transfer_demo.cpp`
2. `src/core/file_transfer_actions.h`
3. `src/core/file_transfer_state_machine.h`
4. `src/core/file_transfer_state_machine.cpp`
5. `src/core/file_transfer_types.h`
6. `src/core/file_transfer_hash.h/.cpp`

原因是：

- 先看 demo，先建立“这东西怎么跑起来”的整体感觉
- 再看 `Action`，理解状态机和外层是怎么分工的
- 再看状态机头文件，知道核心接口是什么
- 最后再看状态机实现细节

---

## 3. 涉及到的主要类和结构体

### 3.1 `DemoHarness`

文件：

- `src/demo/pure_cpp_file_transfer_demo.cpp`

它是 demo 的总控类，可以理解为：

**“测试驱动器 + 假网络适配层 + 文件落盘执行器”**

它负责的事情：

- 解析输入文件路径和输出目录
- 构造一个 `FileOffer`
- 创建 `sender` / `receiver` 两个状态机实例
- 把状态机返回的 `Action` 真正执行掉
- 最后核对输出文件是否存在、SHA256 是否一致

它最核心的方法是：

- `run(...)`
- `processActions(...)`

其中：

- `run(...)` 负责“启动一次传输”
- `processActions(...)` 负责“执行状态机产出的动作”

---

### 3.2 `DemoDataSource`

文件：

- `src/demo/pure_cpp_file_transfer_demo.cpp`

它实现了 `IFileTransferDataSource` 接口。

你可以把它理解为：

**“状态机访问源文件的最小文件读取适配器”**

它只做两件事：

1. `readSourceBytes(...)`
   按 `offset + length` 从源文件里读字节

2. `makeRequestId()`
   生成请求窗口 ID，例如 `req-1`、`req-2`

为什么需要它：

- 状态机本身不直接读文件
- 状态机只知道“我需要某个文件某个偏移的一段字节”
- 真正怎么读，由 `IFileTransferDataSource` 的实现决定

所以 `DemoDataSource` 是 demo 环境下的一个简单实现。

---

### 3.3 `IFileTransferDataSource`

文件：

- `src/core/file_transfer_state_machine.h`

这是一个纯抽象接口。

它的作用是：

**把“状态机逻辑”和“文件读取方式”隔离开。**

状态机只依赖这个接口，不依赖：

- `std::ifstream`
- `QFile`
- 任何平台 API

这样以后如果你想换成：

- Qt 版本读取
- Windows 原生文件 API
- Linux 原生文件 API
- 内存 buffer

都可以只换接口实现，不改状态机主体。

---

### 3.4 `FileTransferStateMachine`

文件：

- `src/core/file_transfer_state_machine.h`
- `src/core/file_transfer_state_machine.cpp`

这是整个 demo 的核心类。

你可以把它理解为：

**“纯业务状态机”**

它不直接做这些事：

- 不直接发网络包
- 不直接写文件
- 不直接起定时器

它只负责：

- 接收事件
- 更新内部状态
- 产出接下来应该执行的动作 `Action`

这就是它最重要的设计思想：

> 状态机只决定“该做什么”，外层再决定“怎么做”。

它最关键的公开接口有：

- `registerLocalOffer(...)`
- `onRemoteFileOffer(...)`
- `beginDownload(...)`
- `beginLatestDownload(...)`
- `onRemoteFileRequest(...)`
- `onRemoteFileChunk(...)`
- `onRemoteFileComplete(...)`
- `onRemoteFileAbort(...)`
- `onTimeout(...)`

---

### 3.5 `Action` 和 `StepResult`

文件：

- `src/core/file_transfer_actions.h`

它们不是类，但它们是这套设计里最关键的“桥梁”。

#### `Action`

表示：

**状态机希望外层执行的一个动作**

例如：

- `SendFileRequest`
- `SendFileChunk`
- `OpenTempFile`
- `AppendTempFile`
- `CommitTempFile`
- `PublishCompletedFiles`

#### `StepResult`

表示：

**状态机处理完一个事件后的结果**

里面通常包含：

- `ok`
- `error`
- `actions`

也就是说，状态机每推进一步，都会返回一个 `StepResult`。

外层再遍历 `actions` 去执行它们。

---

### 3.6 `FileMeta`

文件：

- `src/core/file_transfer_types.h`

它表示一个文件的元信息。

主要字段：

- `fileId`
- `path`
- `name`
- `size`
- `mtimeMs`
- `sha256`

用途：

- sender 用它知道从哪里读文件
- receiver 用它知道最终文件名、大小和校验值

---

### 3.7 `FileOffer`

文件：

- `src/core/file_transfer_types.h`

它表示：

**“我这次会话打算传哪些文件”**

主要字段：

- `sessionId`
- `receivedAtMs`
- `files`

它只描述文件列表，不带文件字节本体。

真正的文件内容是在后面的：

- `FileRequest`
- `FileChunk`

阶段传输的。

---

### 3.8 `DownloadState`

文件：

- `src/core/file_transfer_types.h`

它表示 receiver 端当前正在进行的下载状态。

核心字段：

- `sessionId`
- `fileIndex`
- `nextOffset`
- `requestedLength`
- `receivedInWindow`
- `requestId`
- `localPath`
- `retryCount`
- `deadlineMs`

你可以把它理解为：

**“receiver 现在下载到哪里了”**

---

### 3.9 `TransferConfig`

文件：

- `src/core/file_transfer_types.h`

它表示一些传输配置参数，比如：

- 单个 chunk 大小
- 一个请求窗口最多多少 chunk
- 请求超时时间
- 最大重试次数

demo 里把它配置得比较小，是为了让你能更容易观察到多轮窗口推进。

---

### 3.10 `IncomingFileRequest / IncomingFileChunk / IncomingFileComplete / IncomingFileAbort`

文件：

- `src/core/file_transfer_types.h`

这些结构体表示：

**状态机的结构化输入事件**

也就是说，网络层如果存在，应该先把字节流解析成这些结构体，再喂给状态机。

当前 demo 没有真实网络，所以它是直接在内存里构造这些结构体再调用状态机。

---

### 3.11 `Sha256` 与 `crc32`

文件：

- `src/core/file_transfer_hash.h`
- `src/core/file_transfer_hash.cpp`

作用：

- `crc32` 用于块级校验
- `Sha256` 用于文件级完整性校验

所以它们分别解决两个问题：

- 这个 chunk 有没有坏
- 整个文件是不是完整正确

---

## 4. 类与类之间的关系

可以先用一张关系图来理解：

```text
DemoHarness
  ├─ 持有 DemoDataSource(sender)
  ├─ 持有 DemoDataSource(receiver)
  ├─ 持有 FileTransferStateMachine(sender)
  └─ 持有 FileTransferStateMachine(receiver)

FileTransferStateMachine
  ├─ 依赖 IFileTransferDataSource
  ├─ 使用 FileOffer / FileMeta / DownloadState
  ├─ 输出 Action / StepResult
  └─ 使用 Sha256 / crc32 做校验
```

更具体一点：

### `DemoHarness` 和 `FileTransferStateMachine`

关系是：

**外层驱动器 -> 内层状态机**

`DemoHarness` 调用状态机的方法，拿到 `StepResult`，然后执行动作。

---

### `FileTransferStateMachine` 和 `IFileTransferDataSource`

关系是：

**状态机依赖抽象接口，不依赖具体文件实现**

这是典型的依赖倒置。

---

### `DemoDataSource` 和 `IFileTransferDataSource`

关系是：

**具体实现类 -> 接口**

`DemoDataSource` 给 demo 提供“按偏移读文件”的能力。

---

### `FileTransferStateMachine` 和 `Action`

关系是：

**状态机输出 Action，外层消费 Action**

这是理解这套设计最关键的一条。

---

## 5. 调用流程是怎样的

下面按一次真实 demo 运行过程来讲。

### 第一步：程序入口

入口在：

- `main(int argc, char** argv)`

它做的事情很简单：

1. 解析命令行参数
2. 创建 `DemoHarness`
3. 调用 `harness.run(...)`

---

### 第二步：`DemoHarness::run(...)` 准备文件和目录

这一步负责：

- 准备输出目录
- 确定源文件路径
- 计算源文件大小和 SHA256
- 构造 `FileOffer`

这一步相当于 sender 在说：

> “我这里有一个文件，名字叫 XXX，大小是 YYY，SHA256 是 ZZZ，你可以来请求。”

---

### 第三步：sender 登记本地 Offer

调用：

- `m_sender.registerLocalOffer(offer)`

作用：

- 把本地可发送文件登记到 `m_localOffers`

以后 receiver 发来 `FileRequest` 时，sender 就能根据：

- `sessionId`
- `fileId`

定位到真实源文件。

---

### 第四步：receiver 收到远端 Offer

调用：

- `m_receiver.onRemoteFileOffer(offer)`

作用：

- 把远端 Offer 存入 `m_remoteOffers`

注意：

这时候 receiver 只是“知道远端有这个文件”，还没有开始下载。

---

### 第五步：receiver 开始下载

调用：

- `m_receiver.beginLatestDownload(...)`

它内部会继续走到：

- `beginDownload(...)`
- `requestNextWindow(...)`

这时候状态机开始决定：

- 当前下载哪个文件
- 从哪个偏移开始
- 一次请求多大窗口

然后返回的动作通常包括：

- `OpenTempFile`
- `SendFileRequest`

---

### 第六步：demo 执行动作

`DemoHarness::processActions(...)` 会看到：

- `OpenTempFile`
  就创建 `.part` 临时文件

- `SendFileRequest`
  就构造 `IncomingFileRequest`
  然后直接调用：
  `m_sender.onRemoteFileRequest(request)`

这里就是“假网络”的关键：

- 正常情况应该走网络
- demo 里直接函数调用替代了网络传输

---

### 第七步：sender 处理 `FileRequest`

调用：

- `FileTransferStateMachine::onRemoteFileRequest(...)`

它会做这些事：

1. 校验 `fileId/requestId/offset/length`
2. 在 `m_localOffers` 中找到目标文件
3. 通过 `IFileTransferDataSource` 读取源文件字节
4. 按 chunk 大小切块
5. 为每个块生成 `SendFileChunk`
6. 如果已经到文件末尾，再生成 `SendFileComplete`

所以 sender 状态机并不直接发数据，它只是说：

> “外层，请帮我发这些 chunk。”

---

### 第八步：demo 把 chunk 喂给 receiver

`processActions(...)` 看到 `SendFileChunk` 后，会构造：

- `IncomingFileChunk`

然后直接调用：

- `m_receiver.onRemoteFileChunk(chunk, tick())`

这相当于：

- 网络收到了一个 chunk
- 但这里不经过 socket，直接内存投递

---

### 第九步：receiver 处理 `FileChunk`

调用：

- `FileTransferStateMachine::onRemoteFileChunk(...)`

它会做这些事：

1. 确认当前确实有活跃下载
2. 校验 `sessionId`
3. 校验 `fileId`
4. 校验 `requestId`
5. 校验 `offset == nextOffset`
6. 校验 `CRC32`
7. 产出 `AppendTempFile`
8. 更新 `nextOffset`
9. 更新 `receivedInWindow`
10. 更新 `deadlineMs`

然后分两种情况：

- 如果整个文件收满了：回到 `requestNextWindow(...)`
- 如果当前窗口收满了：也回到 `requestNextWindow(...)`

也就是说：

`requestNextWindow(...)` 是 receiver 端真正的调度中心。

---

### 第十步：receiver 提交文件

当 `requestNextWindow(...)` 发现：

- `remain <= 0`

说明当前文件已经完整收完。

它会做这些事：

1. 对增量 SHA256 做最终收尾
2. 与 `FileOffer` 里的 `sha256` 比较
3. 如果一致，产出 `CommitTempFile`
4. 把最终路径加入 `m_completedPaths`
5. 看是否还有下一个文件

如果所有文件都结束了，它会产出：

- `PublishCompletedFiles`

---

### 第十一步：demo 最终验证

`run(...)` 最后会：

1. 检查 `m_publishedPaths`
2. 检查输出文件是否存在
3. 重新计算源文件和输出文件的 SHA256
4. 输出结果和 trace

如果都正确，就打印：

```text
Pure C++ file transfer demo succeeded
```

---

## 6. 这套设计最核心的思想

如果你只记住一件事，请记住这一句：

> `FileTransferStateMachine` 不是“执行器”，而是“决策器”。

它负责：

- 接收事件
- 推进状态
- 产出动作

而 `DemoHarness` 负责：

- 执行动作
- 模拟网络
- 实际读写文件

这就是为什么这套纯 C++ 状态机很适合以后接：

- Qt
- 真实 TCP
- 单元测试
- 其他平台适配层

---

## 7. 你现在最值得重点看的函数

如果你想继续深入，我建议先看这 6 个函数：

### demo 层

- `DemoHarness::run(...)`
- `DemoHarness::processActions(...)`

### 状态机层

- `FileTransferStateMachine::beginDownload(...)`
- `FileTransferStateMachine::requestNextWindow(...)`
- `FileTransferStateMachine::sendFileRequestWindow(...)`
- `FileTransferStateMachine::onRemoteFileChunk(...)`

只要这 6 个看懂了，整套 demo 的主干就通了。

---

## 8. 一句话总结

这份 demo 本质上是在做一件事：

**用 `DemoHarness` 模拟 sender/receiver 和网络，用 `FileTransferStateMachine` 专心处理文件传输状态推进，再用 `Action` 把“决策”和“执行”拆开。**

