#pragma once

#include <memory>
#include <optional>

#include <QByteArray>
#include <QCryptographicHash>
#include <QHash>
#include <QObject>
#include <QSaveFile>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include "protocol/ProtocolHeader.h"

class ClipboardMonitor;
class ClipboardWriter;
class TransportClient;
class TransportServer;

class SyncCoordinator : public QObject
{
    Q_OBJECT

public:
    explicit SyncCoordinator(ClipboardMonitor *monitor,
                             ClipboardWriter *writer,
                             TransportClient *client,
                             QObject *parent = nullptr);
    ~SyncCoordinator() override;

    // 绑定服务端入站消息到协调器处理逻辑。
    void bindServer(TransportServer *server);
    // 手动写入本地剪贴板并发送到对端。
    bool manualInjectAndSend(const QString &text);

signals:
    // 本地文本通过防回环校验后，准备外发时发出。
    void localTextForwarded(const QString &text);
    // 本地图片通过防回环校验后，准备外发时发出（值为 PNG 字节数）。
    void localImageForwarded(qint64 imageBytes);
    // 本地文件清单通过防回环校验后，准备外发时发出。
    void localFilesForwarded(const QStringList &paths);
    // 收到远端文本消息时发出。
    void remoteTextReceived(const QString &text);
    // 收到远端图片并成功写入本地剪贴板时发出（值为 PNG 字节数）。
    void remoteImageReceived(qint64 imageBytes);
    // 收到远端文件 Offer 时发出（尚未下载）。
    void remoteFileOfferReceived(const QStringList &fileNames);
    // 文件传输状态输出。
    void fileTransferStatus(const QString &status);

public:
    // 粘贴 Hook 的承接入口：触发远端文件按需请求。
    bool requestPendingRemoteFiles();
    // 粘贴热键入口：仅在有远端 Offer 时尝试请求，避免干扰本地普通粘贴。
    bool requestPendingRemoteFilesOnPasteTrigger();
    // Ctrl+Shift+V 快捷键入口：触发请求并输出本次临时目录。
    bool requestPendingRemoteFilesOnCtrlShiftV();

private:
    // 单个文件的元信息（既用于 Offer 广播，也用于传输时校验与定位）。
    struct FileMeta
    {
        // 文件在当前会话内的逻辑编号（字符串形式，便于放入 JSON）。
        QString fileId;
        // 发送端本地绝对路径（仅发送端使用，接收端通常为空）。
        QString path;
        // 文件名（不含目录），用于接收端生成本地临时路径。
        QString name;
        // 文件总字节数。
        qint64 size = 0;
        // 文件最后修改时间（Unix 毫秒时间戳）。
        qint64 mtimeMs = 0;
        // 文件内容 SHA256（十六进制字符串），用于最终完整性校验。
        QString sha256;
    };

    // 一次复制会话对应的“文件 Offer”集合。
    struct FileOffer
    {
        // 会话 ID：同一次复制动作的全局标识。
        quint64 sessionId = 0;
        // 接收端记录该 Offer 到达本地的时间戳（用于选择“最新 Offer”）。
        qint64 receivedAtMs = 0;
        // 本次会话携带的文件元信息列表。
        QVector<FileMeta> files;
    };

    // 接收端下载状态机的运行时状态。
    struct DownloadState
    {
        // 当前下载会话 ID，对应 m_remoteOfferedFiles 里的一个 Offer。
        quint64 sessionId = 0;
        // 当前正在下载第几个文件（索引到 offer.files）。
        int fileIndex = 0;
        // 当前文件“下一块期望偏移”，也是已成功写入的累计字节数。
        qint64 nextOffset = 0;
        // 当前请求窗口计划拉取的总字节数（一次 FileRequest 的长度）。
        qint64 requestedLength = 0;
        // 当前窗口已实际收到并写入的字节数。
        qint64 receivedInWindow = 0;
        // 当前窗口的请求 ID，用于匹配回来的 FileChunk，防止串包。
        QString requestId;
        // 当前文件在接收端临时目录中的本地落盘路径。
        QString localPath;
    };

    // 按统一协议编码并发送文本到对端。
    bool sendTextToPeer(const QString &text, quint64 sessionId);
    // 按统一协议编码并发送图片（PNG）到对端。
    bool sendImageToPeer(const QByteArray &pngBytes, quint64 sessionId);
    // 发送文件 Offer（仅元信息）。
    bool sendFileOfferToPeer(const QStringList &paths, quint64 sessionId);
    // 启动一个文件下载请求窗口。
    bool requestNextWindow();
    // 发送文件请求窗口，支持重发同一 requestId。
    bool sendFileRequestWindow(const FileMeta &meta, bool reuseRequestId);
    // 处理本地剪贴板更新，并在满足条件时转发到对端。
    void handleLocalTextChanged(const QString &text, quint32 textHash);
    // 处理本地图片复制事件。
    void handleLocalImageChanged(const QByteArray &pngBytes, quint32 imageHash);
    // 处理本地文件复制事件。
    void handleLocalFilesChanged(const QStringList &paths, quint32 listHash);
    // 处理远端消息并写入本地剪贴板。
    void handleRemoteMessage(const protocol::ClipboardMessage &message);
    // 处理远端 FileOffer。
    void handleRemoteFileOffer(const protocol::ClipboardMessage &message);
    // 处理远端 FileRequest。
    void handleRemoteFileRequest(const protocol::ClipboardMessage &message);
    // 处理远端 FileChunk。
    void handleRemoteFileChunk(const protocol::ClipboardMessage &message);
    // 处理远端 FileComplete。
    void handleRemoteFileComplete(const protocol::ClipboardMessage &message);
    // 处理远端 FileAbort。
    void handleRemoteFileAbort(const protocol::ClipboardMessage &message);
    // 当前请求窗口超时回调。
    void handleRequestWindowTimeout();
    // 对端连接恢复后尝试续传。
    void handlePeerConnected();
    // 对端断开时暂停当前下载。
    void handlePeerDisconnected();

    // 可选依赖：监听器可通过配置关闭。
    ClipboardMonitor *m_monitor = nullptr;
    // 负责远端内容写入与防回环判定。
    ClipboardWriter *m_writer = nullptr;
    // 本地到远端转发所使用的发送通道。
    TransportClient *m_client = nullptr;

    // 本端已发布给对端的 Offer 缓存（作为对端 FileRequest 的数据源索引）。
    QHash<quint64, FileOffer> m_localOfferedFiles;
    // 对端发来的 Offer 缓存（等待 Ctrl+V/Ctrl+Shift+V 或自动触发拉取）。
    QHash<quint64, FileOffer> m_remoteOfferedFiles;
    // 当前唯一活跃下载任务；当前实现同一时刻只允许一个下载会话。
    std::optional<DownloadState> m_activeDownload;
    // 正在写入中的目标文件，使用 QSaveFile 保证提交原子性。
    std::unique_ptr<QSaveFile> m_downloadFile;
    // 对正在接收文件的增量哈希器，用于最终 SHA256 对比。
    std::unique_ptr<QCryptographicHash> m_downloadHash;
    // 当前会话已下载成功的本地路径列表，完成后整体写入本地剪贴板。
    QStringList m_lastDownloadedPaths;

    // 单个文件分块大小（默认 512KB）。
    int m_chunkSizeBytes = 512 * 1024;
    // 一个请求窗口内最多期望接收的分块数。
    int m_windowChunks = 16;
    // 请求窗口超时时间；超时后触发重试或暂停。
    int m_requestWindowTimeoutMs = 8000;
    // 单个请求窗口允许的最大重试次数。
    int m_maxRequestWindowRetries = 3;
    // 当前窗口已重试次数。
    int m_currentWindowRetryCount = 0;
    // 连接断开时标记“暂停中”，重连后由协调器继续请求。
    bool m_downloadPausedByDisconnect = false;
    // 监督 FileRequest -> FileChunk 的窗口超时计时器。
    QTimer m_requestWindowTimer;
};
