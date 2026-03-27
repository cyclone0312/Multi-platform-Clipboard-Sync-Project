# 剪贴板底层原理详解（KOS/Linux + Windows）

## 1. 你当前遇到的问题本质

导师指出“文件和图片对象未做区分”是非常关键的验收点，因为在系统剪贴板里：

- 文本、图片、文件列表是不同数据语义。
- 同一个 Ctrl+C 表面动作，在底层可能对应完全不同的数据格式和传输策略。
- 跨平台同步时，不能把所有对象都当成“字符串”或“同一种二进制流”。

你现在的项目已经有良好骨架，但下一阶段要把“对象模型”补齐。

## 2. Windows 剪贴板底层

## 2.1 核心机制

Windows 剪贴板是系统全局资源，典型调用链：

1. `OpenClipboard(hwnd)` 打开。
2. `EmptyClipboard()` 清空。
3. `SetClipboardData(format, handle)` 设置一种或多种格式。
4. `CloseClipboard()` 关闭。

读取链路：

1. `OpenClipboard`。
2. `IsClipboardFormatAvailable`。
3. `GetClipboardData`。
4. `GlobalLock/GlobalUnlock` 读数据。
5. `CloseClipboard`。

要点：

- 生产者可一次写入多格式（例如同时写 `CF_UNICODETEXT` 和 HTML 格式）。
- 消费者按自己支持的格式择优读取。

## 2.2 文本、图片、文件在 Windows 的常见格式

文本：

- `CF_UNICODETEXT`（最常用）
- 自定义 HTML 格式（注册格式名 `HTML Format`）

图片：

- `CF_BITMAP`（GDI 位图句柄）
- `CF_DIB` / `CF_DIBV5`（设备无关位图）
- 也可通过 COM DataObject 提供 `image/png` 风格数据

文件：

- 最基础是 `CF_HDROP`（文件路径列表）。
- 这只对“本机可访问路径”有效。

高级虚拟文件（跨进程/跨源生成）：

- `CFSTR_FILEDESCRIPTORW` + `CFSTR_FILECONTENTS`
- 依赖 `IDataObject` 延迟提供文件内容。
- 资源管理器粘贴时可按需读取字节流。

结论：

- 图片通常是“内容型对象”（直接有像素数据）。
- 文件通常是“引用型对象”（路径）或“虚拟文件流对象”（按需读）。
- 两者不应粗暴复用同一种传输策略。

## 2.3 延迟渲染与通知机制

延迟渲染：

- 应用可先声明自己支持某格式，真正数据在后续请求时再提供。
- 对大对象（图片、文件）非常重要。

变更通知：

- 现代方式用 `AddClipboardFormatListener`，窗口收到 `WM_CLIPBOARDUPDATE`。
- 可通过 `GetClipboardSequenceNumber` 快速判断剪贴板是否变化。

这对“去重、防回环”很有价值。

## 2.4 权限与兼容性注意点

- 不同完整性级别进程间，访问可能受 UIPI 限制。
- UWP/沙盒场景行为和传统 Win32 不同。
- 某些应用仅暴露私有格式，跨应用兼容有限。

## 3. KOS（Linux）剪贴板底层

KOS 实际上要区分 X11 会话和 Wayland 会话，这决定你的实现策略。

## 3.1 X11：Selection 机制（不是“集中存储”）

X11 核心不是把数据直接存进系统内存，而是“所有权 + 请求应答”：

- 复制时，应用成为 `CLIPBOARD` selection owner。
- 粘贴时，目标应用向 owner 发请求，owner 再提供数据。

常见 selection：

- `PRIMARY`：鼠标选中即复制，中键粘贴。
- `CLIPBOARD`：Ctrl+C / Ctrl+V 语义。

常见 target（MIME/原子）：

- `UTF8_STRING` / `text/plain`
- `text/uri-list`（文件列表）
- `image/png`
- `TARGETS`（查询可用格式）

大数据传输：

- 使用 `INCR` 协议分块传输，不是一次把巨量数据塞进属性。

关键启示：

- X11 天然是“按需读取、可能分块”的模型。
- 这与你项目中的 `FileOffer -> FileRequest -> FileChunk` 思路是同方向的。

## 3.2 Wayland：数据提供由 compositor/协议中介

Wayland 下应用无法像 X11 那样全局监听键盘/窗口内容，能力更受限：

- `wl_data_device` 负责 copy/paste 与 DnD。
- source 声明可提供 MIME 类型。
- target 选择一种 MIME，通过 pipe/fd 拉取数据。

特点：

- 安全模型更严格，很多“全局监听剪贴板/按键”的传统做法不可用。
- 需要桌面门户（`xdg-desktop-portal`）或 compositor 支持做跨应用能力扩展。

对你后续的影响：

- Ctrl+V 全局 Hook 在 Wayland 往往不可行或不稳定。
- 需要“应用内触发 + 协议自动触发 + 超时兜底”组合策略，而不是只依赖全局热键。

## 3.3 Qt 在 Linux 的抽象

Qt 通过 `QClipboard` 统一接口，但后端行为会因平台插件不同而变化：

- XCB（X11）后端可对应 selection 机制。
- Wayland 后端受 compositor 与协议约束。

因此同样一段 Qt 代码，在不同桌面会话下可观察行为可能不同。

## 4. 为什么图片与文件不能采用同一种传输方式

## 4.1 对象语义不同

图片：

- 粘贴目标通常要“内容本体”（像素）。
- 适合在网络中直接传 `image/png` 字节。

文件：

- 粘贴目标通常要“文件实体”。
- 路径只是本机引用，跨机器无意义。
- 需要“先传元信息，再按需拉取文件内容，再落地本地临时路径/虚拟文件对象”。

## 4.2 体量与交互不同

- 图片常见几十 KB 到几 MB，一次传输可接受。
- 文件可能 GB 级，必须走流式/分块/重传。

## 4.3 平台消费方式不同

- Windows 图片消费可能偏向 `CF_DIB/CF_BITMAP` 或 PNG。
- Windows 文件消费既可 `CF_HDROP`，也可能要求虚拟文件接口。
- Linux 侧常见 `image/png` 与 `text/uri-list`，并且 X11/Wayland机制不同。

结论：

- “图片通道”和“文件通道”应分离。
- 二者可复用传输层，但消息类型、校验、触发逻辑应分别设计。

## 5. 面向你项目的落地建议（为一次性 Ctrl+C/V 做准备）

## 5.1 建立统一对象模型

建议把剪贴板对象先抽象成：

- `TextObject`
- `ImageObject`
- `FileListObject`

每类都要有：

- 采集器（Monitor）
- 协议映射（MessageType + payload schema）
- 写入器（Writer）
- 防回环指纹策略

## 5.2 协议层明确分流

建议消息类型最少包含：

- `TextPlain`
- `ImageBitmap`（建议 payload 用 PNG）
- `FileOffer/FileRequest/FileChunk/FileComplete/FileAbort`

同时保留：

- `sessionId`：一次 Ctrl+C 动作统一会话。
- `sequence`：分块顺序。
- 完整性校验：块级 CRC + 文件级 SHA256。

## 5.3 “一次性 Ctrl+C/V”控制面设计

目标是用户只按一次 Ctrl+C、一次 Ctrl+V。

推荐流程：

1. 复制端 Ctrl+C 后立即发送元数据（Text/Image/FileOffer）。
2. 接收端缓存 Offer 并标记“最近会话可粘贴”。
3. 用户 Ctrl+V 触发：
   - 文本/图片：若 payload 已完整可直接写入剪贴板。
   - 文件：立刻发 `FileRequest` 拉流，完成后写本地文件列表再触发粘贴落地。
4. 全程带超时和失败提示，避免用户误认为“已粘贴成功”。

## 5.4 跨平台兼容优先级建议

第一优先：

- 文本：`text/plain` + `CF_UNICODETEXT`
- 图片：`image/png` + Windows 侧补充可消费格式
- 文件：你现在这套 Offer/Request/Chunk 模型继续强化

第二优先：

- HTML/RTF 富文本
- Windows 虚拟文件（`CFSTR_FILEDESCRIPTORW/CFSTR_FILECONTENTS`）

## 6. 常见误区清单

- 误区 1：把文件当文本路径同步就等于“文件同步”。
- 误区 2：把图片也当文件传，依赖落地路径再粘贴。
- 误区 3：只在应用内快捷键有效，就当作系统级 Ctrl+V 已完成。
- 误区 4：只做块校验，不做整文件校验。
- 误区 5：忽略 Wayland 的安全模型限制。

## 7. 下一步学习建议（最短路径）

1. 先补“剪贴板对象分层认知”：文本/图片/文件。
2. 再补“Windows DataObject 虚拟文件模型”。
3. 同时补“X11 Selection + INCR”和“Wayland data-control 限制”。
4. 最后把你现有文件流状态机完善成可重试、可恢复、可观测。

做到这四步，你的项目就会从“骨架可运行”升级到“有跨平台成品潜力”。
