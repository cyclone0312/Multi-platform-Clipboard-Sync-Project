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

// 纯逻辑无副作用状态机核心 (Side-Effect Free State Machine Core)
// 该类不会直接涉及网络发送、定时器或者文件 I/O 读写，而是通过传入的事件和状态，
// 产出对应的 `StepResult` (包含了一组 Action 动作集供外层系统执行)。
class FileTransferStateMachine
{
public:
    explicit FileTransferStateMachine(IFileTransferDataSource *dataSource, TransferConfig config = {});

    // Sender (发送端) 端登记“我本地有哪些文件可以被请求” (FileOffer)。
    StepResult registerLocalOffer(const FileOffer &offer);
    // Receiver (接收端) 端缓存远端发出的 FileOffer。
    StepResult onRemoteFileOffer(const FileOffer &offer);
    // Receiver (接收端) 显式通过 Session ID 开始一个对应的下载会话。
    StepResult beginDownload(std::uint64_t sessionId, std::int64_t nowMs, const std::string &downloadRoot);
    // Receiver (接收端) 自动从已缓存的 FileOffer 列表中挑最新的开始下载。
    StepResult beginLatestDownload(std::int64_t nowMs, const std::string &downloadRoot);
    
    // Sender (发送端) 处理 Receiver 发来的滑动窗口网络请求 “请发给我 offset 到 offset+length 的数据块”。
    StepResult onRemoteFileRequest(const IncomingFileRequest &request);
    
    // Receiver (接收端) 处理源源不断的网络数据块 (FileChunk)。
    StepResult onRemoteFileChunk(const IncomingFileChunk &chunk, std::int64_t nowMs);
    StepResult onRemoteFileComplete(const IncomingFileComplete &complete);
    StepResult onRemoteFileAbort(const IncomingFileAbort &abortMessage);
    
    // 由外层系统心跳时钟驱动，判断是否有滑动窗口超时，并决定是重启窗口(Retry)还是最终终止并抛出异常。
    StepResult onTimeout(std::int64_t nowMs);
    
    // 通知状态机网络已建连或恢复。
    StepResult onPeerConnected(std::int64_t nowMs);
    // 通知状态机网络断开，让它挂起超时检测。
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
