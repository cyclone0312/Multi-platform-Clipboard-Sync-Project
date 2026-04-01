#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/file_transfer_actions.h"
#include "core/file_transfer_hash.h"
#include "core/file_transfer_types.h"

namespace file_transfer
{

class IFileTransferDataSource
{
public:
    virtual ~IFileTransferDataSource() = default;

    // sender 侧按需读取源文件内容；状态机本身不直接碰文件系统。
    virtual bool readSourceBytes(const FileMeta &meta,
                                 std::int64_t offset,
                                 std::size_t length,
                                 std::vector<std::uint8_t> *outBytes,
                                 std::string *error) = 0;

    // 生成请求窗口 ID，用来把回来的 FileChunk 和当前窗口关联起来。
    virtual std::string makeRequestId() = 0;
};

class FileTransferStateMachine
{
public:
    explicit FileTransferStateMachine(IFileTransferDataSource *dataSource, TransferConfig config = {});

    // sender 端登记“我本地有哪些文件可以被请求”。
    StepResult registerLocalOffer(const FileOffer &offer);
    // receiver 端缓存远端发来的 offer。
    StepResult onRemoteFileOffer(const FileOffer &offer);
    // receiver 端显式开始一个会话下载。
    StepResult beginDownload(std::uint64_t sessionId, std::int64_t nowMs, const std::string &downloadRoot);
    // receiver 端从已缓存的 offer 里挑最新会话开始下载。
    StepResult beginLatestDownload(std::int64_t nowMs, const std::string &downloadRoot);
    // sender 端处理对方发来的“请给我 offset..offset+length”请求。
    StepResult onRemoteFileRequest(const IncomingFileRequest &request);
    // receiver 端处理收到的一个数据块。
    StepResult onRemoteFileChunk(const IncomingFileChunk &chunk, std::int64_t nowMs);
    StepResult onRemoteFileComplete(const IncomingFileComplete &complete);
    StepResult onRemoteFileAbort(const IncomingFileAbort &abortMessage);
    // 由外层时钟驱动；超时后决定重试还是终止。
    StepResult onTimeout(std::int64_t nowMs);
    StepResult onPeerConnected(std::int64_t nowMs);
    StepResult onPeerDisconnected();

    const std::optional<DownloadState> &activeDownload() const;
    const std::vector<std::string> &completedPaths() const;

private:
    // receiver 端调度器：决定是开新文件、发下一窗、还是提交已完成文件。
    StepResult requestNextWindow(std::int64_t nowMs);
    // 组装一个 FileRequest 窗口，不直接发网络包，而是产出 SendFileRequest 动作。
    StepResult sendFileRequestWindow(const FileMeta &meta, std::int64_t nowMs, bool reuseRequestId);
    StepResult failActiveDownload(const std::string &reason, bool abortTempFile);

    const FileMeta *findLocalFile(std::uint64_t sessionId, const std::string &fileId) const;
    const FileOffer *findLatestRemoteOffer() const;
    std::string buildFinalPath(std::uint64_t sessionId, std::size_t fileIndex, const FileMeta &meta) const;
    std::string buildTempPath(std::uint64_t sessionId, std::size_t fileIndex, const FileMeta &meta) const;
    static std::string sanitizeFileName(const std::string &name);

    TransferConfig m_config;
    IFileTransferDataSource *m_dataSource = nullptr;
    // sender 端可供读取的本地文件索引。
    std::unordered_map<std::uint64_t, FileOffer> m_localOffers;
    // receiver 端已收到但未必开始下载的远端 offer。
    std::unordered_map<std::uint64_t, FileOffer> m_remoteOffers;
    // 当前唯一活跃的下载状态；这个 demo 一次只跑一个会话。
    std::optional<DownloadState> m_activeDownload;
    // 已完成文件路径，全部完成后通过 PublishCompletedFiles 一次性抛给外层。
    std::vector<std::string> m_completedPaths;
    std::string m_downloadRoot;
    std::string m_activeTempPath;
    // receiver 端增量计算 SHA256，文件收完后再和 offer 的 sha256 对比。
    Sha256 m_downloadHash;
    bool m_downloadHashActive = false;
    bool m_peerConnected = true;
};

} // namespace file_transfer
