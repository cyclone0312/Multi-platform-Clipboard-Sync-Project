#include "core/file_transfer_state_machine.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace file_transfer
{
namespace
{

StepResult makeFailureResult(const std::string &message)
{
    StepResult result;
    result.ok = false;
    result.error = message;

    Action logAction;
    logAction.type = ActionType::LogStatus;
    logAction.text = message;
    result.add(std::move(logAction));
    return result;
}

Action makeLog(const std::string &text)
{
    Action action;
    action.type = ActionType::LogStatus;
    action.text = text;
    return action;
}

std::string joinPath(const std::string &left, const std::string &right)
{
    if (left.empty() || left == ".")
    {
        return right;
    }
    if (right.empty())
    {
        return left;
    }

    const char tail = left.back();
    if (tail == '/' || tail == '\\')
    {
        return left + right;
    }
    return left + "/" + right;
}

} // namespace

FileTransferStateMachine::FileTransferStateMachine(IFileTransferDataSource *dataSource, TransferConfig config)
    : m_config(config), m_dataSource(dataSource)
{
}

StepResult FileTransferStateMachine::registerLocalOffer(const FileOffer &offer)
{
    m_localOffers[offer.sessionId] = offer;

    StepResult result;
    std::ostringstream stream;
    stream << "Registered local FileOffer, session=" << offer.sessionId << " files=" << offer.files.size();
    result.add(makeLog(stream.str()));
    return result;
}

StepResult FileTransferStateMachine::onRemoteFileOffer(const FileOffer &offer)
{
    m_remoteOffers[offer.sessionId] = offer;

    StepResult result;
    std::ostringstream stream;
    stream << "Received remote FileOffer, session=" << offer.sessionId << " files=" << offer.files.size();
    result.add(makeLog(stream.str()));
    return result;
}

StepResult FileTransferStateMachine::beginDownload(const std::uint64_t sessionId,
                                                   const std::int64_t nowMs,
                                                   const std::string &downloadRoot)
{
    const auto it = m_remoteOffers.find(sessionId);
    if (it == m_remoteOffers.end())
    {
        return makeFailureResult("Cannot start download: remote session not found");
    }
    if (it->second.files.empty())
    {
        return makeFailureResult("Cannot start download: remote offer contains no files");
    }
    if (m_activeDownload.has_value())
    {
        return makeFailureResult("Cannot start download: another transfer is already active");
    }

    m_downloadRoot = downloadRoot;
    m_completedPaths.clear();
    m_activeTempPath.clear();
    m_downloadHash.reset();
    m_downloadHashActive = false;

    DownloadState state;
    state.sessionId = sessionId;
    m_activeDownload = state;

    StepResult result;
    std::ostringstream stream;
    stream << "Begin download, session=" << sessionId << " files=" << it->second.files.size();
    result.add(makeLog(stream.str()));
    // 启动下载后立刻交给调度器决定第一步动作，通常会先 OpenTempFile + SendFileRequest。
    result.merge(requestNextWindow(nowMs));
    return result;
}

StepResult FileTransferStateMachine::beginLatestDownload(const std::int64_t nowMs, const std::string &downloadRoot)
{
    const FileOffer *offer = findLatestRemoteOffer();
    if (offer == nullptr)
    {
        return makeFailureResult("Cannot start download: no remote FileOffer available");
    }
    return beginDownload(offer->sessionId, nowMs, downloadRoot);
}

StepResult FileTransferStateMachine::onRemoteFileRequest(const IncomingFileRequest &request)
{
    if (request.fileId.empty() || request.requestId.empty() || request.offset < 0 || request.length <= 0)
    {
        return makeFailureResult("Reject FileRequest: invalid request fields");
    }

    const FileMeta *target = findLocalFile(request.sessionId, request.fileId);
    if (target == nullptr)
    {
        StepResult result;
        result.ok = false;
        result.error = "source file not found";

        Action abortAction;
        abortAction.type = ActionType::SendFileAbort;
        abortAction.sessionId = request.sessionId;
        abortAction.fileId = request.fileId;
        abortAction.requestId = request.requestId;
        abortAction.reason = "source file not found";
        result.add(std::move(abortAction));
        result.add(makeLog("Reject FileRequest: source file not found"));
        return result;
    }

    if (request.offset >= target->size)
    {
        StepResult result;
        result.ok = false;
        result.error = "request offset beyond end of file";

        Action abortAction;
        abortAction.type = ActionType::SendFileAbort;
        abortAction.sessionId = request.sessionId;
        abortAction.fileId = request.fileId;
        abortAction.requestId = request.requestId;
        abortAction.reason = "request offset beyond end of file";
        result.add(std::move(abortAction));
        result.add(makeLog("Reject FileRequest: offset beyond end of file"));
        return result;
    }

    if (m_dataSource == nullptr)
    {
        return makeFailureResult("Reject FileRequest: no data source is available");
    }

    StepResult result;
    const std::int64_t maxToSend = std::min<std::int64_t>(request.length, target->size - request.offset);
    std::int64_t sent = 0;

    // Sender (发送端) 处理请求窗口 (Window)：
    // 接收端会发来例如请求读取 3MB 数据的窗口请求 [offset, offset+length) 。
    // 发送端的状态机基于事先配置的 chunk size (分块大小)，把这长段请求切分为多个小的 SendFileChunk 动作，
    // 每个动作包含实际数据字节和 CRC 校验，发往网络模块。
    // 这避免了单次产生超大网络包（容易引发丢包），也使应用层有能力实现自主流控和包速率控制。
    while (sent < maxToSend)
    {
        const std::size_t chunkSize = static_cast<std::size_t>(
            std::min<std::int64_t>(static_cast<std::int64_t>(m_config.chunkSizeBytes), maxToSend - sent));

        std::vector<std::uint8_t> bytes;
        std::string error;
        if (!m_dataSource->readSourceBytes(*target, request.offset + sent, chunkSize, &bytes, &error))
        {
            result.ok = false;
            result.error = error.empty() ? "source file unavailable" : error;

            Action abortAction;
            abortAction.type = ActionType::SendFileAbort;
            abortAction.sessionId = request.sessionId;
            abortAction.fileId = request.fileId;
            abortAction.requestId = request.requestId;
            abortAction.reason = result.error;
            result.add(std::move(abortAction));
            result.add(makeLog("Abort FileRequest: failed to read source bytes"));
            return result;
        }

        if (bytes.empty())
        {
            break;
        }

        const std::int64_t chunkOffset = request.offset + sent;
        sent += static_cast<std::int64_t>(bytes.size());

        Action chunkAction;
        chunkAction.type = ActionType::SendFileChunk;
        chunkAction.sessionId = request.sessionId;
        chunkAction.fileId = request.fileId;
        chunkAction.requestId = request.requestId;
        chunkAction.offset = chunkOffset;
        chunkAction.chunkCrc32 = crc32(bytes);
        chunkAction.bytes = std::move(bytes);
        result.add(std::move(chunkAction));
    }

    if (request.offset + sent >= target->size)
    {
        // 发到文件末尾时补一个完成帧，让 receiver 知道发送端已经到尾部。
        Action completeAction;
        completeAction.type = ActionType::SendFileComplete;
        completeAction.sessionId = request.sessionId;
        completeAction.fileId = request.fileId;
        completeAction.requestId = request.requestId;
        completeAction.length = target->size;
        completeAction.sha256 = target->sha256;
        result.add(std::move(completeAction));
    }

    std::ostringstream stream;
    stream << "Handled FileRequest, session=" << request.sessionId
           << " fileId=" << request.fileId
           << " offset=" << request.offset
           << " length=" << maxToSend;
    result.add(makeLog(stream.str()));
    return result;
}

StepResult FileTransferStateMachine::onRemoteFileChunk(const IncomingFileChunk &chunk, const std::int64_t nowMs)
{
    if (!m_activeDownload.has_value())
    {
        return makeFailureResult("Ignore FileChunk: no active download");
    }

    DownloadState &state = *m_activeDownload;
    const auto offerIt = m_remoteOffers.find(state.sessionId);
    if (state.sessionId != chunk.sessionId || offerIt == m_remoteOffers.end())
    {
        return makeFailureResult("Ignore FileChunk: session mismatch");
    }

    const FileOffer &offer = offerIt->second;
    if (state.fileIndex >= offer.files.size())
    {
        return makeFailureResult("Ignore FileChunk: file index is out of range");
    }

    const FileMeta &meta = offer.files[state.fileIndex];
    // ==== 严格的数据块匹配与乱序丢弃逻辑 ====
    // 只有当 Chunk 的各项属性：所属文件 ID、对应当前窗口唯一的 requestId，
    // 以及严格符合当前期待的偏移量 (nextOffset) 完全一致时，该数据块才会被接收。
    // 这种机制天然免疫以下网络问题：
    // 1. 网络延迟导致过期的、重试时产生的老数据块造成的脏数据 (Stale Chunks)。
    // 2. UDP 等不可靠传输层造成的包乱序、重复和串包。
    if (chunk.fileId != meta.fileId || chunk.requestId != state.requestId || chunk.offset != state.nextOffset)
    {
        return failActiveDownload("Abort download: FileChunk order mismatch", true);
    }
    if (crc32(chunk.bytes) != chunk.chunkCrc32)
    {
        return failActiveDownload("Abort download: FileChunk CRC mismatch", true);
    }
    if (!m_downloadHashActive || m_activeTempPath.empty())
    {
        return failActiveDownload("Abort download: temp file is not prepared", false);
    }

    StepResult result;

    Action appendAction;
    appendAction.type = ActionType::AppendTempFile;
    appendAction.sessionId = chunk.sessionId;
    appendAction.fileId = chunk.fileId;
    appendAction.requestId = chunk.requestId;
    appendAction.path = m_activeTempPath;
    appendAction.offset = chunk.offset;
    appendAction.bytes = chunk.bytes;
    result.add(std::move(appendAction));

    m_downloadHash.update(chunk.bytes);
    state.nextOffset += static_cast<std::int64_t>(chunk.bytes.size());
    state.receivedInWindow += static_cast<std::int64_t>(chunk.bytes.size());
    state.retryCount = 0;
    state.deadlineMs = nowMs + m_config.requestTimeoutMs;
    state.pausedByDisconnect = false;

    Action timerAction;
    timerAction.type = ActionType::StartTimer;
    timerAction.timeoutMs = m_config.requestTimeoutMs;
    result.add(std::move(timerAction));

    if (state.nextOffset >= meta.size)
    {
        // 当前文件收满后，不直接写死处理下一个，而是递归进入本状态机的核心调度器 requestNextWindow，
        // 由它统一决定是“提交当前文件（Commit），还是规划再请求下一个文件”。
        result.merge(requestNextWindow(nowMs));
        return result;
    }

    if (state.receivedInWindow >= state.requestedLength)
    {
        // 当前窗口收满后，再发下一窗请求。
        result.merge(requestNextWindow(nowMs));
    }
    return result;
}

StepResult FileTransferStateMachine::onRemoteFileComplete(const IncomingFileComplete &complete)
{
    StepResult result;
    std::ostringstream stream;
    stream << "Received FileComplete, session=" << complete.sessionId
           << " fileId=" << complete.fileId
           << " size=" << complete.size;
    result.add(makeLog(stream.str()));
    return result;
}

StepResult FileTransferStateMachine::onRemoteFileAbort(const IncomingFileAbort &abortMessage)
{
    return failActiveDownload("Remote aborted transfer: " + abortMessage.reason, true);
}

StepResult FileTransferStateMachine::onTimeout(const std::int64_t nowMs)
{
    if (!m_activeDownload.has_value())
    {
        return {};
    }

    DownloadState &state = *m_activeDownload;
    if (state.pausedByDisconnect || !m_peerConnected)
    {
        StepResult result;
        state.pausedByDisconnect = true;
        result.add(makeLog("Transfer paused: peer is disconnected"));
        return result;
    }

    if (nowMs < state.deadlineMs)
    {
        return {};
    }

    const auto offerIt = m_remoteOffers.find(state.sessionId);
    if (offerIt == m_remoteOffers.end() || state.fileIndex >= offerIt->second.files.size())
    {
        return makeFailureResult("Ignore timeout: active download is no longer valid");
    }

    if (state.retryCount >= m_config.maxRequestWindowRetries)
    {
        return failActiveDownload("Abort download: request window retry limit reached", true);
    }

    ++state.retryCount;

    StepResult result;
    std::ostringstream stream;
    stream << "Retry FileRequest window, attempt=" << state.retryCount;
    result.add(makeLog(stream.str()));
    result.merge(sendFileRequestWindow(offerIt->second.files[state.fileIndex], nowMs, true));
    return result;
}

StepResult FileTransferStateMachine::onPeerConnected(const std::int64_t nowMs)
{
    m_peerConnected = true;
    if (!m_activeDownload.has_value() || !m_activeDownload->pausedByDisconnect)
    {
        return {};
    }

    m_activeDownload->pausedByDisconnect = false;

    StepResult result;
    result.add(makeLog("Peer reconnected, resume active download"));
    result.merge(requestNextWindow(nowMs));
    return result;
}

StepResult FileTransferStateMachine::onPeerDisconnected()
{
    m_peerConnected = false;

    StepResult result;
    if (m_activeDownload.has_value())
    {
        m_activeDownload->pausedByDisconnect = true;

        Action stopAction;
        stopAction.type = ActionType::StopTimer;
        result.add(std::move(stopAction));
        result.add(makeLog("Peer disconnected, pause active download"));
    }
    return result;
}

const std::optional<DownloadState> &FileTransferStateMachine::activeDownload() const
{
    return m_activeDownload;
}

const std::vector<std::string> &FileTransferStateMachine::completedPaths() const
{
    return m_completedPaths;
}

StepResult FileTransferStateMachine::requestNextWindow(const std::int64_t nowMs)
{
    if (!m_activeDownload.has_value())
    {
        return makeFailureResult("Cannot advance: no active download");
    }

    DownloadState &state = *m_activeDownload;
    const auto offerIt = m_remoteOffers.find(state.sessionId);
    if (offerIt == m_remoteOffers.end())
    {
        return failActiveDownload("Abort download: remote FileOffer is missing", true);
    }

    const FileOffer &offer = offerIt->second;
    StepResult result;

    if (state.fileIndex >= offer.files.size())
    {
        // 整个会话结束：停止计时器，把完成路径一次性告诉外层。
        Action stopAction;
        stopAction.type = ActionType::StopTimer;
        result.add(std::move(stopAction));

        Action publishAction;
        publishAction.type = ActionType::PublishCompletedFiles;
        publishAction.sessionId = state.sessionId;
        publishAction.paths = m_completedPaths;
        result.add(std::move(publishAction));

        std::ostringstream stream;
        stream << "Download session completed, session=" << state.sessionId << " files=" << m_completedPaths.size();
        result.add(makeLog(stream.str()));

        m_remoteOffers.erase(state.sessionId);
        m_activeDownload.reset();
        m_activeTempPath.clear();
        m_downloadHash.reset();
        m_downloadHashActive = false;
        return result;
    }

    const FileMeta &meta = offer.files[state.fileIndex];
    if (m_activeTempPath.empty())
    {
        // ==== 单个文件下载初始化阶段 ====
        // 当刚刚进入一个新文件时，分配“最终落盘路径”和“后缀加上 .part 的临时下载路径”。
        // 通过产生 OpenTempFile 的动作指令，让外层的 Actuator/执行器 去底层操作系统里把文件 IO 句柄打开，并准备就绪。
        state.localPath = buildFinalPath(state.sessionId, state.fileIndex, meta);
        m_activeTempPath = buildTempPath(state.sessionId, state.fileIndex, meta);
        state.nextOffset = 0;
        state.requestedLength = 0;
        state.receivedInWindow = 0;
        state.requestId.clear();
        state.retryCount = 0;
        state.deadlineMs = 0;
        m_downloadHash.reset();
        m_downloadHashActive = true;

        Action openAction;
        openAction.type = ActionType::OpenTempFile;
        openAction.sessionId = state.sessionId;
        openAction.fileIndex = state.fileIndex;
        openAction.path = m_activeTempPath;
        openAction.targetPath = state.localPath;
        result.add(std::move(openAction));
    }

    const std::int64_t remain = meta.size - state.nextOffset;
    if (remain <= 0)
    {
        if (meta.sha256.empty())
        {
            result.merge(failActiveDownload("Abort download: missing SHA256 for completed file", true));
            return result;
        }

        const std::string finalSha = m_downloadHash.finalHex();
        m_downloadHashActive = false;
        if (finalSha != meta.sha256)
        {
            result.merge(failActiveDownload("Abort download: SHA256 mismatch for completed file", true));
            return result;
        }

        // 全部接收到并确认文件完整且 SHA256 可信。
        // 通知外层系统（Actuator），将之前不断追加的 .part 临时文件，移动或重命名并替换为真正的目标路径文件 (Target Path)。
        Action commitAction;
        commitAction.type = ActionType::CommitTempFile;
        commitAction.sessionId = state.sessionId;
        commitAction.fileIndex = state.fileIndex;
        commitAction.path = m_activeTempPath;
        commitAction.targetPath = state.localPath;
        commitAction.sha256 = finalSha;
        result.add(std::move(commitAction));

        std::ostringstream stream;
        stream << "Completed file download, name=" << meta.name;
        result.add(makeLog(stream.str()));

        m_completedPaths.push_back(state.localPath);
        ++state.fileIndex;
        state.nextOffset = 0;
        state.requestedLength = 0;
        state.receivedInWindow = 0;
        state.requestId.clear();
        state.retryCount = 0;
        state.deadlineMs = 0;
        state.pausedByDisconnect = false;
        m_activeTempPath.clear();
        m_downloadHash.reset();

        // 提交完一个文件后，继续调度下一文件或结束会话。
        result.merge(requestNextWindow(nowMs));
        return result;
    }

    result.merge(sendFileRequestWindow(meta, nowMs, false));
    return result;
}

StepResult FileTransferStateMachine::sendFileRequestWindow(const FileMeta &meta,
                                                          const std::int64_t nowMs,
                                                          const bool reuseRequestId)
{
    if (!m_activeDownload.has_value())
    {
        return makeFailureResult("Cannot send FileRequest: no active download");
    }

    DownloadState &state = *m_activeDownload;
    const std::int64_t remain = meta.size - state.nextOffset;
    if (remain <= 0)
    {
        return {};
    }

    if (!reuseRequestId || state.requestId.empty())
    {
        if (m_dataSource == nullptr)
        {
            return makeFailureResult("Cannot generate FileRequest: no data source is available");
        }

        state.requestId = m_dataSource->makeRequestId();
        state.requestedLength =
            std::min<std::int64_t>(remain, static_cast<std::int64_t>(m_config.chunkSizeBytes * m_config.windowChunks));
        state.receivedInWindow = 0;
        state.retryCount = 0;
    }

    // deadline 由外层 onTimeout(nowMs) 检查，这里只负责更新时间点。
    state.deadlineMs = nowMs + m_config.requestTimeoutMs;

    StepResult result;

    Action requestAction;
    requestAction.type = ActionType::SendFileRequest;
    requestAction.sessionId = state.sessionId;
    requestAction.fileIndex = state.fileIndex;
    requestAction.fileId = meta.fileId;
    requestAction.requestId = state.requestId;
    requestAction.offset = state.nextOffset;
    requestAction.length = state.requestedLength;
    result.add(std::move(requestAction));

    Action timerAction;
    timerAction.type = ActionType::StartTimer;
    timerAction.timeoutMs = m_config.requestTimeoutMs;
    result.add(std::move(timerAction));

    std::ostringstream stream;
    stream << "Request window, file=" << meta.name
           << " offset=" << state.nextOffset
           << " length=" << state.requestedLength
           << " retry=" << state.retryCount;
    result.add(makeLog(stream.str()));
    return result;
}

StepResult FileTransferStateMachine::failActiveDownload(const std::string &reason, const bool abortTempFile)
{
    StepResult result;
    result.ok = false;
    result.error = reason;

    Action stopAction;
    stopAction.type = ActionType::StopTimer;
    result.add(std::move(stopAction));

    if (abortTempFile && !m_activeTempPath.empty())
    {
        Action abortAction;
        abortAction.type = ActionType::AbortTempFile;
        abortAction.path = m_activeTempPath;
        result.add(std::move(abortAction));
    }

    result.add(makeLog(reason));
    m_activeDownload.reset();
    m_activeTempPath.clear();
    m_downloadHash.reset();
    m_downloadHashActive = false;
    return result;
}

const FileMeta *FileTransferStateMachine::findLocalFile(const std::uint64_t sessionId, const std::string &fileId) const
{
    const auto offerIt = m_localOffers.find(sessionId);
    if (offerIt == m_localOffers.end())
    {
        return nullptr;
    }

    for (const FileMeta &meta : offerIt->second.files)
    {
        if (meta.fileId == fileId)
        {
            return &meta;
        }
    }
    return nullptr;
}

const FileOffer *FileTransferStateMachine::findLatestRemoteOffer() const
{
    const FileOffer *latest = nullptr;
    for (const auto &entry : m_remoteOffers)
    {
        if (latest == nullptr || entry.second.receivedAtMs > latest->receivedAtMs)
        {
            latest = &entry.second;
        }
    }
    return latest;
}

std::string FileTransferStateMachine::buildFinalPath(const std::uint64_t sessionId,
                                                     const std::size_t fileIndex,
                                                     const FileMeta &meta) const
{
    const std::string root = m_downloadRoot.empty() ? std::string(".") : m_downloadRoot;
    const std::string sessionDir = joinPath(root, std::to_string(sessionId));
    const std::string safeName = sanitizeFileName(meta.name.empty() ? ("file_" + std::to_string(fileIndex)) : meta.name);
    return joinPath(sessionDir, safeName);
}

std::string FileTransferStateMachine::buildTempPath(const std::uint64_t sessionId,
                                                    const std::size_t fileIndex,
                                                    const FileMeta &meta) const
{
    return buildFinalPath(sessionId, fileIndex, meta) + ".part";
}

std::string FileTransferStateMachine::sanitizeFileName(const std::string &name)
{
    std::string safe = name;
    for (char &ch : safe)
    {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
        {
            ch = '_';
        }
    }

    safe.erase(std::remove_if(safe.begin(), safe.end(), [](const unsigned char ch)
                              { return std::iscntrl(ch) != 0; }),
               safe.end());

    if (safe.empty())
    {
        return "unnamed_file";
    }
    return safe;
}

} // namespace file_transfer
