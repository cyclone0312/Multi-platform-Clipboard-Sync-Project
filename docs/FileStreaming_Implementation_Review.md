# 文件流式传输改动审查与调用流程说明

## 1. 结论摘要

当前代码已经实现了以下能力：

- 复制文件时发送 FileOffer（只发送元信息，不发送文件大数据）。
- 通过 FileRequest 触发按需拉取，支持 offset + length。
- 发送端分块读取文件并发送 FileChunk。
- 接收端边收边写到本地临时文件，使用 QSaveFile 落盘。
- 每个块做 CRC32 校验；接收端有整文件 SHA256 校验入口。

但目前还不能判定为“已经成功实现跨平台文件流式传输完整目标”，原因是：

- 粘贴 Hook 还未接入系统层，当前是 UI 按钮模拟触发。
- 发送端没有填充文件 SHA256，导致最终整文件校验实际默认失效。
- 流控与断点续传仅有协议字段和窗口请求雏形，缺少可靠重试状态机。
- 剪贴板文件列表写入使用本地路径 URL，不能直接跨平台互认远端路径语义。

## 2. 已实现能力对应关系

### 2.1 协议扩展

- 新增消息类型：FileOffer / FileRequest / FileChunk / FileComplete / FileAbort。
- 位置：src/protocol/ProtocolHeader.h

### 2.2 剪贴板监控与写入

- 监控文件复制：从 mimeData.urls 提取本地文件路径并发出 localFilesChanged。
- 位置：src/clipboard/ClipboardMonitor.h, src/clipboard/ClipboardMonitor.cpp
- 写入文件列表：把本地路径写回剪贴板 URL 列表，并做防回环哈希记录。
- 位置：src/clipboard/ClipboardWriter.h, src/clipboard/ClipboardWriter.cpp

### 2.3 同步协调器

- 新增远端文件 Offer 缓存、本地 Offer 缓存、下载状态机。
- 支持请求窗口 requestNextWindow，支持 chunkSize 与 windowChunks。
- 接收端块级 CRC 校验与顺序校验，边收边写。
- 下载完成后把本地临时路径回写本地剪贴板。
- 位置：src/sync/SyncCoordinator.h, src/sync/SyncCoordinator.cpp

### 2.4 调试触发入口

- 新增按钮“模拟粘贴触发远端文件拉取”，触发 requestPendingRemoteFiles。
- 位置：src/ui/SyncDebugWindow.h, src/ui/SyncDebugWindow.cpp, src/app/AppController.cpp

## 3. 详细调用流程

## 3.1 文件复制阶段（发送 FileOffer，不传大数据）

1. 用户在本机复制文件。
2. ClipboardMonitor::tryEmitClipboardText 检测 mimeData.hasUrls，提取本地路径列表。
3. 发出 localFilesChanged(paths, listHash)。
4. SyncCoordinator::handleLocalFilesChanged 收到事件，做防回环判断。
5. SyncCoordinator::sendFileOfferToPeer 构造文件元信息（fileId/name/size/mtime/sha256）并发送 FileOffer。
6. 对端 SyncCoordinator::handleRemoteFileOffer 缓存远端 Offer，不立即下载。

## 3.2 粘贴触发阶段（当前为模拟触发）

1. 用户在调试窗口点击按钮。
2. SyncDebugWindow 发出 requestRemoteFilesTriggered。
3. AppController 把该信号连接到 SyncCoordinator::requestPendingRemoteFiles。
4. requestPendingRemoteFiles 选取最近一次远端 Offer，初始化 DownloadState。
5. 调用 requestNextWindow 发送首个 FileRequest（fileId/requestId/offset/length/windowChunks）。

## 3.3 流式传输阶段（边读边写）

发送端：

1. handleRemoteFileRequest 解析请求。
2. 打开源文件并 seek(offset)。
3. 按固定块大小读取 chunk，构造 FileChunk（meta + raw chunk）。
4. 每个 chunk 填写 chunkCrc32 与 offset。
5. 达到文件末尾后发送 FileComplete。

接收端：

1. handleRemoteFileChunk 解析 chunk payload。
2. 校验 fileId/requestId/offset 连续性。
3. 校验 chunkCrc32。
4. 通过 QSaveFile::write 写本地临时文件，并累积 SHA256。
5. 当前窗口收满后继续 requestNextWindow。
6. 文件完成后 commit 文件并推进到下一个文件。
7. 全部文件完成后将下载得到的本地路径列表写入本地剪贴板。

## 4. 关键风险与缺口（按严重度）

### 严重

1. 未实现系统级粘贴 Hook，目标 5 还未闭环
- 证据：src/ui/SyncDebugWindow.cpp 中按钮文案与触发逻辑是“模拟粘贴触发远端文件拉取”。
- 影响：无法在 Windows/KOS/UOS 的真实 Ctrl+V 路径中自动触发 FileRequest。

2. 整文件 SHA256 校验路径不完整
- 证据：SyncCoordinator::sendFileOfferToPeer 中 meta.sha256 未计算即发送；接收端仅在 meta.sha256 非空才校验。
- 影响：finalSha 可能长期不生效，无法满足强完整性要求。

3. 发送端传输成功判定依赖 QTcpSocket::write 返回一次写满
- 证据：TransportClient::sendMessage 直接判断 written == frame.size。
- 影响：大帧或拥塞时可能部分写入，导致误判失败与潜在消息丢失。

### 高

4. 断点续传还未形成完整状态机
- 当前虽有 offset + length，但缺少断线后会话恢复、重连后重新协商、窗口内缺块重传。
- 影响：网络波动下会中断，无法达到“可恢复”目标。

5. 文件路径跨平台语义仍需抽象
- 当前 FileOffer 实际由发送端本地路径推导元信息，接收端落地后写回本地路径。
- 影响：在跨平台场景中，路径只能作为本地临时结果，不应做跨端路径语义假设。

### 中

6. 接收端对 FileComplete 的处理仅记录状态
- 证据：handleRemoteFileComplete 仅 emit 日志，不驱动状态机收敛。
- 影响：极端情况下难以利用完成帧做一致性修正。

7. 下载状态与缓存缺少 TTL/容量控制
- 远端 Offer、本地 Offer 缓存长期存在时会占用内存。

## 5. 推荐补齐路线

1. 接入系统粘贴 Hook 层
- Windows：先实现进程内 Ctrl+V 触发，再迭代到资源管理器可用的延迟数据提供模型。
- Linux：分别针对 X11 与 Wayland 设计触发策略。

2. 在发送端补全 SHA256
- 复制阶段计算或延迟计算并回填 Offer；至少在 FileComplete 中带真实 sha256。

3. 改造发送队列
- TransportClient 增加待发队列 + bytesWritten 驱动，保证完整帧发送。

4. 完善续传与重传
- 引入 transferId + requestId 的持久状态，断线后按 offset 恢复请求。

5. 增加超时与重试策略
- 每个请求窗口设置超时，超时后重发 FileRequest 或发送 FileAbort。

## 6. 你当前版本是否“已经实现成功”

可以认为：

- 已实现“文件流式传输核心骨架”，可用于开发联调与功能演示。

不能认为：

- 已实现“跨平台生产可用的文件流式传输完整方案”。

核心差距：

- 系统粘贴 Hook 未落地。
- 可靠传输（完整写入、续传、重试）未闭环。
- 完整性校验链路未闭环（发送端 sha256）。

## 7. 建议测试清单

- 单文件：1KB、1MB、1GB。
- 多文件：100 个小文件 + 1 个大文件混合。
- 异常：中途断网、进程重启、磁盘空间不足。
- 一致性：CRC 错误注入、乱序注入、重复 chunk 注入。
- 跨平台：Windows -> Linux，Linux -> Windows，验证 Ctrl+C/Ctrl+V 实际行为路径。
