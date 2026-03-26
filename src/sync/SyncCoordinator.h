#pragma once

#include <memory>
#include <optional>

#include <QCryptographicHash>
#include <QHash>
#include <QObject>
#include <QSaveFile>
#include <QStringList>
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
    // 本地文件清单通过防回环校验后，准备外发时发出。
    void localFilesForwarded(const QStringList &paths);
    // 收到远端文本消息时发出。
    void remoteTextReceived(const QString &text);
    // 收到远端文件 Offer 时发出（尚未下载）。
    void remoteFileOfferReceived(const QStringList &fileNames);
    // 文件传输状态输出。
    void fileTransferStatus(const QString &status);

private:
    // 按统一协议编码并发送文本到对端。
    bool sendTextToPeer(const QString &text, quint64 sessionId);
    // 发送文件 Offer（仅元信息）。
    bool sendFileOfferToPeer(const QStringList &paths, quint64 sessionId);
    // 启动一个文件下载请求窗口。
    bool requestNextWindow();
    // 处理本地剪贴板更新，并在满足条件时转发到对端。
    void handleLocalTextChanged(const QString &text, quint32 textHash);
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

public:
    // 粘贴 Hook 的承接入口：触发远端文件按需请求。
    bool requestPendingRemoteFiles();

    // 可选依赖：监听器可通过配置关闭。
    ClipboardMonitor *m_monitor = nullptr;
    // 负责远端内容写入与防回环判定。
    ClipboardWriter *m_writer = nullptr;
    // 本地到远端转发所使用的发送通道。
    TransportClient *m_client = nullptr;

    struct FileMeta
    {
        QString fileId;
        QString path;
        QString name;
        qint64 size = 0;
        qint64 mtimeMs = 0;
        QString sha256;
    };

    struct FileOffer
    {
        quint64 sessionId = 0;
        qint64 receivedAtMs = 0;
        QVector<FileMeta> files;
    };

    struct DownloadState
    {
        quint64 sessionId = 0;
        int fileIndex = 0;
        qint64 nextOffset = 0;
        qint64 requestedLength = 0;
        qint64 receivedInWindow = 0;
        QString requestId;
        QString localPath;
    };

    QHash<quint64, FileOffer> m_localOfferedFiles;
    QHash<quint64, FileOffer> m_remoteOfferedFiles;
    std::optional<DownloadState> m_activeDownload;
    std::unique_ptr<QSaveFile> m_downloadFile;
    std::unique_ptr<QCryptographicHash> m_downloadHash;
    QStringList m_lastDownloadedPaths;

    int m_chunkSizeBytes = 512 * 1024;
    int m_windowChunks = 16;
};
