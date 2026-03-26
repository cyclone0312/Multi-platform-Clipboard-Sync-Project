# 跨平台剪切板同步项目（Qt/C++）MVP 设计说明

## 1. 项目目标理解

目标是在两台机器之间实现剪切板双向同步：

- Windows 10 <-> Linux（KOS/Ubuntu、UOS/Debian）
- MVP 先支持“简单文本”自动同步
- 后续支持“简单富文本”（HTML/RTF）与图片（优先 PNG）
- 后续扩展支持大文件流式传输、手动注入、跨机拖拽

本项目的关键挑战不在单点功能，而在于：

- 跨平台剪切板行为差异（Windows、X11、Wayland）
- 网络层协议可扩展性（MVP 文本 + 未来流式文件）
- 循环同步抑制（本地写入后触发再次发送）
- UI 线程无阻塞设计

---

## 2. MVP 范围界定（当前阶段）

### 2.1 必做（MVP）

- 本地剪切板变更监听（Qt `QClipboard::dataChanged`）
- TCP 基础通信（Server/Client）
- 文本消息收发与反向写入系统剪切板
- 双向同步（Windows -> Linux，Linux -> Windows）

### 2.2 明确不做（MVP 暂缓）

- 大文件真实流式发送逻辑
- 粘贴 Hook 的系统级深度接管
- 拖拽交互协议与 UI

### 2.3 必须预留（架构层）

- 消息类型扩展能力
- 剪切板 MIME/格式标识能力（plain/html/rtf/image）
- 分片/流式会话能力
- 手动注入 API 接口
- 传输会话与任务状态管理

---

## 3. 建议总体架构（Qt C/S）

建议采用“单进程双角色”架构：每个端都可同时作为 Server 和 Client。

- Server：监听本机端口，接收远端消息
- Client：主动连接到配置中的对端
- 同步策略：以消息协议驱动，不依赖平台特定逻辑

这样做的好处：

- 不需要强依赖“主从”部署
- 任意一端重启后都可重连恢复
- 后续多设备扩展更自然

### 3.1 核心模块划分

- AppController
  - 负责生命周期、配置加载、模块组装
- ClipboardMonitor
  - 监听本地剪切板变化并产出标准化事件
- ClipboardWriter
  - 将接收数据写入本地剪切板
- SyncCoordinator
  - 去重、防回环、流控与路由
- TransportServer
  - 基于 `QTcpServer` 接收连接
- TransportClient
  - 基于 `QTcpSocket` 发起连接与发送
- MessageCodec
  - 统一编码/解码 Header + Payload
- SessionManager（预留）
  - 管理流式传输会话（streamId、分片状态、重传）

---

## 4. 通信协议设计（可扩展 Header）

目标：MVP 可传文本，未来可无缝扩展到 1GB 流式文件。

### 4.1 设计原则

- 固定头 + 可变体（TLV 或 JSON/Binary payload）
- 强版本化（兼容未来协议升级）
- 消息类型明确区分文本与流式控制帧
- 支持分片与会话 ID

### 4.2 建议 Header 字段

固定长度 Header（例如 40 或 48 字节）可定义如下：

- magic（4B）
  - 魔数，快速校验协议包
- version（2B）
  - 协议版本，如 `1`
- headerLength（2B）
  - Header 长度，便于后续扩展
- messageType（2B）
  - 消息类型（文本、流开始、流分片、流结束、ACK、错误等）
- flags（2B）
  - 压缩、加密、是否需要 ACK、是否为重发包等
- sessionId（8B）
  - 一次复制动作对应会话 ID（文本也可用）
- sequence（8B）
  - 包序号，支持乱序检测/重传策略
- payloadLength（8B）
  - 当前包负载长度
- checksum（4B）
  - Header+Payload 或仅 Payload 校验
- reserved（可选）
  - 预留字段

### 4.3 建议 messageType 枚举

- `0x0001` TEXT_PLAIN
- `0x0002` TEXT_HTML
- `0x0003` TEXT_RTF
- `0x0004` IMAGE_BITMAP
- `0x0010` STREAM_START
- `0x0011` STREAM_CHUNK
- `0x0012` STREAM_END
- `0x0020` CLIPBOARD_INJECT（手动注入预留）
- `0x00F0` ACK
- `0x00F1` NACK
- `0x00FF` ERROR

### 4.5 富文本与图片负载建议（后续阶段）

- Header 继续使用统一结构，按 `messageType` 区分负载解析逻辑
- 文本类（`TEXT_PLAIN/TEXT_HTML/TEXT_RTF`）：
  - payload 建议为 UTF-8 字节
  - 可在扩展字段或 payload 前缀中带 `mimeType`（如 `text/plain`、`text/html`、`text/rtf`）
- 图片类（`IMAGE_BITMAP`）：
  - payload 建议优先使用 PNG 二进制（跨平台兼容较好，体积可控）
  - 保留 JPEG/BMP 的格式扩展能力
- 同步策略建议：
  - 首阶段仅发送单一“最佳格式”（例如优先 HTML，否则 plain；图片优先 PNG）
  - 后续再升级为“多格式并发”（同一会话发送多种格式，接收端择优落地）

### 4.4 MVP 的实际使用方式

MVP 阶段只启用：

- `TEXT_PLAIN`
- 可选 `ACK`

但解析器必须具备：

- 识别未知 `messageType` 并安全忽略或返回错误
- 按 `headerLength`、`payloadLength` 正确拆包
- 为未来 `STREAM_*` 保留处理入口

---

## 5. 同步链路与防回环策略

### 5.1 基础链路

- 本地复制触发剪切板变更
- `ClipboardMonitor` 产生事件
- `SyncCoordinator` 判定是否需要外发
- `TransportClient` 发送 TCP 消息
- 远端收到后写入本地剪切板

### 5.2 防回环（非常关键）

远端写入本地剪切板会再次触发 `dataChanged`，必须抑制循环。

建议方案：

- 每条同步消息附带 `sessionId` + `originNodeId`
- 本端在写剪切板前记录“最近一次远端注入指纹”（时间窗 + 哈希）
- `ClipboardMonitor` 触发后优先与最近注入记录比对
- 命中则不再外发

这样可避免 A->B->A 的无限环路。

---

## 6. 不阻塞 UI 的开发逻辑（先设计，不写代码）

### 6.1 剪切板监听逻辑

- `QClipboard::dataChanged` 信号在主线程触发
- 主线程只做轻量逻辑：读取文本、构建消息对象、投递发送请求
- 不在槽函数中做耗时网络发送

### 6.2 TCP 发送逻辑

建议两种方案，优先方案 A：

- 方案 A（推荐）：
  - 创建独立网络线程（`QThread`）
  - `QTcpSocket` 及发送队列驻留在线程内
  - 主线程通过 queued signal 发送“待发消息”
- 方案 B（简化版）：
  - `QTcpSocket` 在主线程
  - 通过缓冲队列 + 异步 `bytesWritten` 驱动分段写
  - 适合 MVP 小数据，但后续扩展性略弱

MVP 可以先 A 或 B，若你准备快速迭代到大文件，建议直接用 A。

---

## 7. 项目结构建议

推荐目录（单仓库）：

- `CMakeLists.txt`
- `src/main.cpp`
- `src/app/AppController.*`
- `src/clipboard/ClipboardMonitor.*`
- `src/clipboard/ClipboardWriter.*`
- `src/sync/SyncCoordinator.*`
- `src/transport/TransportServer.*`
- `src/transport/TransportClient.*`
- `src/protocol/ProtocolHeader.*`
- `src/protocol/MessageCodec.*`
- `src/session/SessionManager.*`（可先空实现）
- `src/common/Config.*`
- `src/common/Logger.*`
- `tests/`（后续单元测试）
- `docs/`（协议文档、状态机文档）

---

## 8. CMake / Pro 配置建议

优先建议使用 CMake（跨平台维护更好），并兼容 Qt5/Qt6 的最小差异。

### 8.1 CMake 关键点

- `CMAKE_CXX_STANDARD 17`
- 打开 `AUTOMOC/AUTORCC/AUTOUIC`
- 链接 Qt 模块：
  - `Core`
  - `Gui`（`QClipboard` 需要）
  - `Network`
  - 如后续 GUI：`Widgets`
- 平台编译选项：
  - Windows：`ws2_32` 一般由 Qt 处理，可按需补充
  - Linux：确保 X11/Wayland 运行时依赖

### 8.2 qmake `.pro`（备选）

- `QT += core gui network`
- `CONFIG += c++17`
- `TEMPLATE = app`
- 按模块组织 `HEADERS/SOURCES`

建议：新项目优先 CMake，若团队已有 qmake 资产再考虑 `.pro`。

---

## 9. KOS / UOS 编译环境准备（基础包）

由于两者底层接近 Ubuntu / Debian，可按如下最小集合准备。

### 9.1 通用构建工具

- `build-essential`
- `cmake`
- `ninja-build`
- `pkg-config`
- `git`

### 9.2 Qt 开发包（Qt6 优先）

- `qt6-base-dev`
- `qt6-base-dev-tools`
- `qt6-tools-dev`
- `qt6-tools-dev-tools`

如发行版暂不稳定支持 Qt6，可退回 Qt5：

- `qtbase5-dev`
- `qttools5-dev`
- `qttools5-dev-tools`

### 9.3 Linux 图形与剪切板相关依赖

- `libx11-dev`
- `libxfixes-dev`
- `libxcb1-dev`
- `libxcb-xfixes0-dev`
- Wayland 环境建议补充：
  - `qt6-wayland`
  - `wl-clipboard`
- X11 调试辅助可选：
  - `xclip`

说明：KOS/UOS 可能使用自有仓库命名，若包名略有差异，可通过 `apt search` 对应查找。

---

## 10. 后续开发路线（建议）

### 第 1 步：跑通最小链路

- 仅文本 + 单连接 + 无 UI
- 完成 A->B 与 B->A 文本同步
- 完成回环抑制

### 第 2 步：协议稳定化

- 固化 Header
- 增加 ACK/NACK 和错误码
- 完成连接重试与断线重连

### 第 3 步：扩展剪切板数据类型

- 增加 `TEXT_HTML/TEXT_RTF` 收发与写入
- 增加 `IMAGE_BITMAP` 收发与写入（先支持 PNG）
- 完成跨平台格式兼容验证（Windows/X11/Wayland）

### 第 4 步：为大文件做骨架

- `STREAM_START/CHUNK/END` 状态机
- SessionManager 落地
- 分片写入与背压策略

### 第 5 步：注入 API 与拖拽预研

- 提供本地 API 注入入口
- 评估跨机拖拽的交互协议与安全边界

---

## 11. 当前建议结论

当前最优策略是：

- 先把 MVP 文本链路做“稳定、可恢复、可扩展”
- 从第一天就引入统一 Header 与消息类型
- 网络发送与剪切板监听逻辑解耦，避免 UI 阻塞
- 明确防回环机制，否则双向同步无法稳定运行

这套设计可以在不推翻 MVP 代码的情况下平滑升级到 1GB 文件流式传输与拖拽同步。
