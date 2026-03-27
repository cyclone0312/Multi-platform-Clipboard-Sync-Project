# 当前项目文件传输实现复盘（底层原理 + 类调用逻辑）

本文基于当前代码实现梳理，不是理想化方案。目标是让你明确：现在已经完成了什么、底层是怎么跑的、离一次性 Ctrl+C/V 还差什么。

## 1. 当前实现在做什么

你现在已经实现的是“跨机文件传输协议骨架”：

- 本地检测到文件复制后，先发 `FileOffer`（元信息）。
- 远端触发请求后，发送端按 `offset + length` 分块回传 `FileChunk`。
- 接收端按窗口写入临时文件，块级 CRC 校验，文件级 SHA256 校验后提交。
- 下载完后把本地临时文件列表写入系统剪贴板。

这是一条正确的主线。

## 2. 关键类职责

## 2.1 ClipboardMonitor

职责：从系统剪贴板读取本地变化并发出事件。

关键行为：

- 监听 `QClipboard::dataChanged`。
- 若 `mimeData.hasUrls()`，提取本地绝对路径，发 `localFilesChanged(paths, listHash)`。
- 否则尝试读取文本，发 `localTextChanged(text, textHash)`。

底层原理点：

- 这是“生产端事件入口”。
- 文件与文本已经做了第一层分流。
- 但图片还未被单独识别处理。

## 2.2 ClipboardWriter

职责：把远端数据写回本地剪贴板，并做防回环指纹记录。

关键行为：

- `writeRemoteText`：写文本并记录文本哈希。
- `writeRemoteFileList`：写 `QMimeData::urls` 并记录文件列表哈希。
- `isRecentlyInjected*`：判断是否是“刚由远端注入”的内容。

底层原理点：

- 防回环依赖“短时间窗口 + 内容哈希”。
- 这是避免 A->B->A 无限回传的核心机制之一。

## 2.3 SyncCoordinator

职责：协议路由 + 状态机 + 文件流调度核心。

关键能力：

- 处理本地事件：
  - 文本走 `sendTextToPeer`。
  - 文件走 `sendFileOfferToPeer`。
- 处理远端消息：
  - `FileOffer` 缓存。
  - `FileRequest` 读取本地文件并回传 `FileChunk`。
  - `FileChunk` 校验并落盘。
  - `FileComplete` / `FileAbort` 状态反馈。
- 下载状态控制：
  - `DownloadState` 保存 `sessionId/fileIndex/offset/requestId`。
  - `requestNextWindow` 与 `sendFileRequestWindow` 控制窗口拉流。

底层原理点：

- 你已经把“控制面（Offer/Request）”和“数据面（Chunk）”分离。
- `QSaveFile` 保证写入原子提交，失败可取消。
- 使用块 CRC + 全文件 SHA256 形成双层完整性校验。

## 2.4 TransportClient / TransportServer

职责：长度前缀帧化传输。

关键行为：

- Client：`4字节长度 + 协议包` 写入 socket。
- Server：按长度前缀拆帧，交给 `MessageCodec` 解码。
- `MessageCodec` 校验 magic/version/headerLength/payloadLength/checksum。

底层原理点：

- TCP 本身是字节流，必须自己拆包/粘包处理。
- 你现在这套“长度前缀 + 固定头 + payload”是标准做法。

## 2.5 PasteTriggerHook + SyncDebugWindow + AppController

职责：触发路径与模块装配。

关键行为：

- `AppController` 连接模块信号槽。
- `SyncDebugWindow` 按钮可以触发 `requestPendingRemoteFilesOnCtrlShiftV`。
- `PasteTriggerHook` 提供：
  - 应用级 `Ctrl+V` 检测（eventFilter）。
  - Windows 全局低级键盘钩子。
  - Linux X11（宏开启时）轮询按键组合。

底层原理点：

- 你已把“粘贴触发”抽象成独立组件，这是后续跨平台落地的关键。
- 但系统级稳定性（尤其 Wayland）仍需专门策略。

## 3. 端到端调用时序

## 3.1 文件复制到 Offer 发送

1. 用户复制文件。
2. `ClipboardMonitor::handleClipboardChanged -> tryEmitClipboardText`。
3. 识别 `hasUrls`，发 `localFilesChanged(paths, listHash)`。
4. `SyncCoordinator::handleLocalFilesChanged` 做防回环判断。
5. `SyncCoordinator::sendFileOfferToPeer`：
   - 采集文件元信息（name/size/mtime/sha256）。
   - 发送 `MessageType::FileOffer`。

## 3.2 收到 Offer 到开始拉流

1. 对端 `handleRemoteMessage` 分派到 `handleRemoteFileOffer`。
2. 缓存到 `m_remoteOfferedFiles[sessionId]`。
3. 当触发粘贴动作（按钮或 Ctrl+Shift+V）时，执行 `requestPendingRemoteFiles*`。
4. 初始化 `DownloadState`，进入 `requestNextWindow`。
5. 发送首个 `FileRequest`（含 `offset/length/windowChunks/requestId`）。

## 3.3 发送端响应 Request

1. `handleRemoteFileRequest` 校验参数。
2. 打开本地源文件并 `seek(offset)`。
3. 循环读取 chunk：
   - 计算 `chunkCrc32`。
   - 打包 `FileChunk(meta + raw bytes)`。
   - 发送。
4. 到达文件末尾发送 `FileComplete`（带 size/sha256）。

## 3.4 接收端写盘与推进

1. `handleRemoteFileChunk` 校验：
   - `fileId/requestId/offset` 连续性。
   - `crc32(chunk)` 一致性。
2. 写入 `QSaveFile`，同步更新 SHA256。
3. 收满窗口继续 `requestNextWindow`。
4. 文件结束后校验整文件 SHA256，`commit()` 提交。
5. 全部文件完成后 `writeRemoteFileList(downloadedPaths)`。

## 4. 目前实现涉及的核心底层原理

1. 事件驱动：Qt 信号槽串联监控、同步、传输、写回。
2. 防回环：最近注入哈希 TTL 抑制。
3. 会话化：`sessionId` 表示一次复制/同步动作。
4. 流式传输：`offset/length` 窗口请求，不一次塞满网络。
5. 完整性：块级 CRC 快速发现损坏，文件级 SHA256终态确认。
6. 原子落盘：`QSaveFile` 避免写半文件污染目标目录。
7. 粘贴触发解耦：把按键检测与传输状态机解耦。

## 5. 你当前版本的主要短板（必须直面）

1. 图片对象仍未进入独立通道。
2. Linux Wayland 下全局粘贴触发能力不可靠。
3. 发送端在线检查较严格，离线时没有可靠发送队列持久化。
4. FileComplete 仅状态提示，未参与强一致收敛逻辑。
5. 失败恢复还不够完整（虽然有窗口超时重试雏形）。

## 6. 为“一次性 Ctrl+C/V 文件传输”准备的改造清单

## 6.1 协议与对象层

1. 保持文件通道与图片通道分离。
2. 为 `FileOffer` 增加更完整元信息：
   - 相对路径（目录复制时）
   - MIME 推断
   - 可选压缩标识
3. 明确 `transferId`（可与 `sessionId`并存），用于恢复时唯一定位。

## 6.2 状态机层

1. 增加下载状态枚举：`Idle/Offered/Requesting/Receiving/Verifying/Committed/Failed`。
2. 把 `FileComplete` 纳入状态推进，而不仅是日志。
3. 支持断线恢复：
   - 重连后按 `nextOffset` 续拉。
   - requestId 失效时重新协商窗口。

## 6.3 触发层

1. 复制动作后自动预热 Offer 缓存。
2. 粘贴触发优先级：
   - 应用内快捷键
   - Windows 全局钩子
   - X11 全局检测
   - Wayland 退化到“应用内触发 + 自动拉取兜底”
3. 粘贴时用户体验：
   - 有 Offer 立即进入拉流
   - 无 Offer 走本地普通粘贴

## 6.4 校验与观测层

1. 记录每个 chunk 的耗时与吞吐。
2. 统计重试次数、超时次数、最终成功率。
3. 失败原因标准化（源文件不可读、CRC失败、SHA失败、写盘失败、连接断开）。

## 7. 你现在可以怎么向导师解释

建议你这样表述：

- 我已经完成“跨平台文件流式同步骨架”，并实现了 Offer/Request/Chunk 的分层协议。
- 我已经实现了块校验、整文件校验、原子写盘和防回环。
- 我承认目前尚未完成图片对象独立链路与 Wayland 下系统级粘贴触发闭环。
- 下一阶段将按“对象分流 + 状态机收敛 + 触发兼容”三条线补齐，目标是一次性 Ctrl+C/V 成品化。

这种表达既诚实，也体现你对底层机制已经建立了工程化理解。
