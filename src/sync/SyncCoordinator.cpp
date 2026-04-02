#include "sync/SyncCoordinator.h"

#include <QtEndian>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QDebug>
#include <QRandomGenerator>
#include <QUuid>

#include <limits>

#include "clipboard/ClipboardMonitor.h"
#include "clipboard/ClipboardWriter.h"
#include "transport/TransportClient.h"
#include "transport/TransportServer.h"

namespace
{
    QByteArray encodeJson(const QJsonObject &json)
    {
        return QJsonDocument(json).toJson(QJsonDocument::Compact);
    }

    bool decodeJson(const QByteArray &bytes, QJsonObject *out)
    {
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject())
        {
            return false;
        }

        *out = doc.object();
        return true;
    }

    quint32 crc32(const QByteArray &bytes)
    {
        quint32 crc = 0xFFFFFFFFu;
        for (const uchar byte : bytes)
        {
            crc ^= byte;
            for (int i = 0; i < 8; ++i)
            {
                const quint32 mask = static_cast<quint32>(-(crc & 1u));
                crc = (crc >> 1) ^ (0xEDB88320u & mask);
            }
        }
        return ~crc;
    }

    QByteArray buildChunkPayload(const QJsonObject &meta, const QByteArray &chunk)
    {
        const QByteArray metaBytes = encodeJson(meta);
        QByteArray payload;
        payload.resize(4);
        qToLittleEndian<quint32>(static_cast<quint32>(metaBytes.size()), reinterpret_cast<uchar *>(payload.data()));
        payload.append(metaBytes);
        payload.append(chunk);
        return payload;
    }

    bool parseChunkPayload(const QByteArray &payload, QJsonObject *meta, QByteArray *chunk)
    {
        if (payload.size() < 4)
        {
            return false;
        }

        const quint32 metaSize = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(payload.constData()));
        if (payload.size() < static_cast<int>(4 + metaSize))
        {
            return false;
        }

        const QByteArray metaBytes = payload.mid(4, static_cast<int>(metaSize));
        if (!decodeJson(metaBytes, meta))
        {
            return false;
        }

        *chunk = payload.mid(static_cast<int>(4 + metaSize));
        return true;
    }

    QString tempDownloadRoot()
    {
        return QDir::tempPath() + QStringLiteral("/clipboard_sync_downloads");
    }

    QString buildDownloadPath(quint64 sessionId, const QString &fileName)
    {
        QDir root(tempDownloadRoot());
        root.mkpath(QStringLiteral("."));

        const QString sessionDirName = QString::number(sessionId);
        root.mkpath(sessionDirName);
        QDir sessionDir(root.filePath(sessionDirName));

        QString candidate = sessionDir.filePath(fileName);
        if (!QFileInfo::exists(candidate))
        {
            return candidate;
        }

        const QFileInfo fi(fileName);
        const QString base = fi.completeBaseName().isEmpty() ? fi.fileName() : fi.completeBaseName();
        const QString ext = fi.completeSuffix();
        for (int i = 1; i <= 9999; ++i)
        {
            const QString numberedName = ext.isEmpty()
                                             ? QStringLiteral("%1_%2").arg(base).arg(i)
                                             : QStringLiteral("%1_%2.%3").arg(base).arg(i).arg(ext);
            candidate = sessionDir.filePath(numberedName);
            if (!QFileInfo::exists(candidate))
            {
                return candidate;
            }
        }

        return sessionDir.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral("_") + fileName);
    }

    QString computeFileSha256Hex(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return QString();
        }

        QCryptographicHash hash(QCryptographicHash::Sha256);
        QByteArray chunk;
        chunk.resize(1024 * 1024);
        while (true)
        {
            const qint64 n = file.read(chunk.data(), chunk.size());
            if (n < 0)
            {
                return QString();
            }
            if (n == 0)
            {
                break;
            }

            hash.addData(chunk.constData(), n);
        }

        return QString::fromLatin1(hash.result().toHex());
    }
}

SyncCoordinator::SyncCoordinator(ClipboardMonitor *monitor,
                                 ClipboardWriter *writer,
                                 TransportClient *client,
                                 QObject *parent)
    : QObject(parent), m_monitor(monitor), m_writer(writer), m_client(client)
{
    m_requestWindowTimer.setSingleShot(true);
    m_requestWindowTimer.setInterval(m_requestWindowTimeoutMs);
    QObject::connect(&m_requestWindowTimer, &QTimer::timeout, this, &SyncCoordinator::handleRequestWindowTimeout);

    if (m_monitor)
    {
        QObject::connect(m_monitor,
                         &ClipboardMonitor::localTextChanged,
                         this,
                         &SyncCoordinator::handleLocalTextChanged);
        QObject::connect(m_monitor,
                         &ClipboardMonitor::localImageChanged,
                         this,
                         &SyncCoordinator::handleLocalImageChanged);
        QObject::connect(m_monitor,
                         &ClipboardMonitor::localFilesChanged,
                         this,
                         &SyncCoordinator::handleLocalFilesChanged);
    }

    if (m_client)
    {
        QObject::connect(m_client, &TransportClient::peerConnected, this, &SyncCoordinator::handlePeerConnected);
        QObject::connect(m_client, &TransportClient::peerDisconnected, this, &SyncCoordinator::handlePeerDisconnected);
    }
}

SyncCoordinator::~SyncCoordinator() = default;

void SyncCoordinator::bindServer(TransportServer *server)
{
    QObject::connect(server,
                     &TransportServer::messageReceived,
                     this,
                     &SyncCoordinator::handleRemoteMessage);
}

bool SyncCoordinator::manualInjectAndSend(const QString &text)
{
    if (text.isEmpty())
    {
        return false;
    }

    const quint64 sessionId = QRandomGenerator::global()->generate64();

    if (!m_writer->writeRemoteText(text, sessionId))
    {
        qWarning() << "manual inject failed to write local clipboard";
        return false;
    }

    emit localTextForwarded(text);
    return sendTextToPeer(text, sessionId);
}

bool SyncCoordinator::manualSendFiles(const QStringList &paths)
{
    if (paths.isEmpty())
    {
        return false;
    }

    // 把“拖入窗口”视作一次新的复制会话，这样可以直接复用现有的
    // FileOffer / FileRequest / FileChunk 传输链路，而不用再单独设计协议。
    const quint64 sessionId = QRandomGenerator::global()->generate64();
    emit localFilesForwarded(paths);
    if (!sendFileOfferToPeer(paths, sessionId, true))
    {
        emit fileTransferStatus(QStringLiteral("手动拖入的文件未能发送到对端"));
        qWarning() << "manual file offer forward failed";
        return false;
    }

    return true;
}

bool SyncCoordinator::requestPendingRemoteFiles()
{
    return startPendingRemoteFilesRequest(false, true);
}

bool SyncCoordinator::startPendingRemoteFilesRequestForSession(const quint64 sessionId,
                                                              const bool replayPasteAfterDownload,
                                                              const bool publishClipboardAfterDownload)
{
    if (m_activeDownload.has_value())
    {
        emit fileTransferStatus(QStringLiteral("已有文件传输任务在进行中"));
        return false;
    }

    if (!m_remoteOfferedFiles.contains(sessionId))
    {
        emit fileTransferStatus(QStringLiteral("当前没有可请求的远端文件 Offer"));
        return false;
    }

    const FileOffer offer = m_remoteOfferedFiles.value(sessionId);
    if (offer.files.isEmpty())
    {
        emit fileTransferStatus(QStringLiteral("远端 Offer 不包含可下载文件"));
        return false;
    }

    m_lastDownloadedPaths.clear();
    DownloadState state;
    state.sessionId = sessionId;
    state.fileIndex = 0;
    state.nextOffset = 0;
    m_activeDownload = state;
    m_replayPasteAfterCurrentDownload = replayPasteAfterDownload;
    m_publishClipboardAfterCurrentDownload = publishClipboardAfterDownload;
    m_currentWindowRetryCount = 0;
    m_downloadPausedByDisconnect = false;
    m_requestWindowTimer.stop();

    emit fileTransferStatus(QStringLiteral("开始请求远端文件，总数: %1").arg(offer.files.size()));
    return requestNextWindow();
}

// 开始请求远端文件，触发下载流程。这个函数在多个入口被调用，包括粘贴热键和 Ctrl+Shift+V 快捷键。
// 它会检查当前是否有活跃的下载任务，如果没有，就从 m_remoteOfferedFiles 里选出最新的一个 Offer，初始化下载状态，
// 并调用 requestNextWindow() 向对端发送第一个文件请求窗口。
bool SyncCoordinator::startPendingRemoteFilesRequest(bool replayPasteAfterDownload, bool publishClipboardAfterDownload)
{
    if (m_activeDownload.has_value())
    {
        emit fileTransferStatus(QStringLiteral("已有文件传输任务在进行中"));
        return false;
    }

    if (m_remoteOfferedFiles.isEmpty())
    {
        emit fileTransferStatus(QStringLiteral("当前没有可请求的远端文件 Offer"));
        return false;
    }

    quint64 latestSessionId = 0;
    qint64 latestTs = std::numeric_limits<qint64>::min();
    for (auto it = m_remoteOfferedFiles.cbegin(); it != m_remoteOfferedFiles.cend(); ++it)
    {
        if (it.value().receivedAtMs > latestTs)
        {
            latestTs = it.value().receivedAtMs;
            latestSessionId = it.key();
        }
    }

    const FileOffer offer = m_remoteOfferedFiles.value(latestSessionId);
    if (offer.files.isEmpty())
    {
        emit fileTransferStatus(QStringLiteral("远端 Offer 不包含可下载文件"));
        return false;
    }

    return startPendingRemoteFilesRequestForSession(latestSessionId,
                                                    replayPasteAfterDownload,
                                                    publishClipboardAfterDownload);
}

bool SyncCoordinator::requestPendingRemoteFilesOnPasteTrigger()
{
    if (!shouldInterceptPasteTrigger())
    {
        return false;
    }

    return startPendingRemoteFilesRequest(true, true);
}

bool SyncCoordinator::requestPendingRemoteFilesOnCtrlShiftV()
{
    if (m_activeDownload.has_value())
    {
        emit fileTransferStatus(QStringLiteral("已有文件传输任务在进行中"));
        return false;
    }

    if (m_remoteOfferedFiles.isEmpty())
    {
        emit fileTransferStatus(QStringLiteral("Ctrl+Shift+V: 当前没有可请求的远端文件 Offer"));
        return false;
    }

    quint64 latestSessionId = 0;
    qint64 latestTs = std::numeric_limits<qint64>::min();
    for (auto it = m_remoteOfferedFiles.cbegin(); it != m_remoteOfferedFiles.cend(); ++it)
    {
        if (it.value().receivedAtMs > latestTs)
        {
            latestTs = it.value().receivedAtMs;
            latestSessionId = it.key();
        }
    }

    const QString tempSessionDir = QDir::toNativeSeparators(QDir(tempDownloadRoot()).filePath(QString::number(latestSessionId)));
    emit fileTransferStatus(QStringLiteral("Ctrl+Shift+V: 远端文件将暂存到 %1").arg(tempSessionDir));
    return startPendingRemoteFilesRequest(false, true);
}

void SyncCoordinator::scheduleNextPendingRemoteOfferRequest()
{
    if (m_activeDownload.has_value() || m_remoteOfferedFiles.isEmpty())
    {
        return;
    }

    quint64 latestSessionId = 0;
    qint64 latestTs = std::numeric_limits<qint64>::min();
    for (auto it = m_remoteOfferedFiles.cbegin(); it != m_remoteOfferedFiles.cend(); ++it)
    {
        if (!it.value().autoPrefetchOnReceive)
        {
            continue;
        }

        if (it.value().receivedAtMs > latestTs)
        {
            latestTs = it.value().receivedAtMs;
            latestSessionId = it.key();
        }
    }

    if (latestSessionId == 0)
    {
        return;
    }

    QMetaObject::invokeMethod(this, [this, latestSessionId]()
                              { startPendingRemoteFilesRequestForSession(latestSessionId, false, false); }, Qt::QueuedConnection);
}

bool SyncCoordinator::shouldInterceptPasteTrigger() const
{
    return !m_activeDownload.has_value() && !m_remoteOfferedFiles.isEmpty();
}

bool SyncCoordinator::sendTextToPeer(const QString &text, quint64 sessionId)
{
    protocol::ClipboardMessage message;
    message.type = protocol::MessageType::TextPlain;
    message.flags = 0;
    message.sessionId = sessionId;
    message.sequence = 1;
    message.payload = text.toUtf8();

    qInfo() << "sending text bytes:" << message.payload.size() << "session:" << message.sessionId;

    if (!m_client->sendMessage(message))
    {
        qWarning() << "send text failed (peer may be offline)";
        return false;
    }

    return true;
}

bool SyncCoordinator::sendImageToPeer(const QByteArray &pngBytes, quint64 sessionId)
{
    if (pngBytes.isEmpty())
    {
        return false;
    }

    protocol::ClipboardMessage message;
    message.type = protocol::MessageType::ImageBitmap;
    message.flags = 0;
    message.sessionId = sessionId;
    message.sequence = 1;
    message.payload = pngBytes;

    qInfo() << "sending image bytes:" << message.payload.size() << "session:" << message.sessionId;

    if (!m_client->sendMessage(message))
    {
        qWarning() << "send image failed (peer may be offline)";
        return false;
    }

    return true;
}

bool SyncCoordinator::sendFileOfferToPeer(const QStringList &paths, quint64 sessionId, const bool autoPrefetchOnReceive)
{
    FileOffer offer;
    offer.sessionId = sessionId;
    offer.autoPrefetchOnReceive = autoPrefetchOnReceive;

    QJsonArray filesJson;
    int index = 0;
    for (const QString &path : paths)
    {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile())
        {
            continue;
        }

        FileMeta meta;
        meta.fileId = QString::number(index++);
        meta.path = info.absoluteFilePath();
        meta.name = info.fileName();
        meta.size = info.size();
        meta.mtimeMs = info.lastModified().toMSecsSinceEpoch();
        // Offer 阶段先计算整文件 SHA256，供接收端最终提交前做强一致校验。
        meta.sha256 = computeFileSha256Hex(meta.path);
        if (meta.sha256.isEmpty())
        {
            qWarning() << "skip file offer because SHA256 failed:" << meta.path;
            continue;
        }

        QJsonObject fileObj;
        fileObj.insert(QStringLiteral("fileId"), meta.fileId);
        fileObj.insert(QStringLiteral("name"), meta.name);
        fileObj.insert(QStringLiteral("size"), static_cast<double>(meta.size));
        fileObj.insert(QStringLiteral("mtimeMs"), static_cast<double>(meta.mtimeMs));
        fileObj.insert(QStringLiteral("sha256"), meta.sha256);
        filesJson.push_back(fileObj);
        offer.files.push_back(meta);
    }

    if (offer.files.isEmpty())
    {
        return false;
    }

    m_localOfferedFiles.insert(sessionId, offer);

    protocol::ClipboardMessage message;
    message.type = protocol::MessageType::FileOffer;
    message.flags = 0;
    message.sessionId = sessionId;
    message.sequence = 1;

    QJsonObject root;
    root.insert(QStringLiteral("autoPrefetch"), autoPrefetchOnReceive);
    root.insert(QStringLiteral("files"), filesJson);
    message.payload = encodeJson(root);

    return m_client->sendMessage(message);
}

// 它是“调度器/状态机核心”，负责判断当前阶段该做什么
bool SyncCoordinator::requestNextWindow()
{
    // 保护条件
    if (!m_activeDownload.has_value())
    {
        return false;
    }

    DownloadState &state = m_activeDownload.value();
    // 会话有效性检查
    if (!m_remoteOfferedFiles.contains(state.sessionId))
    {
        emit fileTransferStatus(QStringLiteral("下载任务找不到远端 Offer，会话失效"));
        m_requestWindowTimer.stop();
        m_replayPasteAfterCurrentDownload = false;
        m_activeDownload.reset();
        m_downloadFile.reset();
        m_downloadHash.reset();
        return false;
    }

    const FileOffer &offer = m_remoteOfferedFiles[state.sessionId];
    // 是否已完成全部文件  如果 fileIndex 已到末尾，说明整个会话下载完成
    if (state.fileIndex >= offer.files.size())
    {
        const QStringList downloadedPaths = m_lastDownloadedPaths;
        const bool hasDownloadedFiles = !downloadedPaths.isEmpty();
        bool clipboardReady = false;
        if (hasDownloadedFiles && m_publishClipboardAfterCurrentDownload)
        {
            // 将远端文件列表写入本地剪贴板，并记录防回环指纹。
            clipboardReady = m_writer->writeRemoteFileList(downloadedPaths, state.sessionId);
        }
        if (hasDownloadedFiles)
        {
            // 窗口列表里只放已经落到本地磁盘的文件，这样用户从列表拖出时
            // 不会再依赖网络中的下载过程，拖拽体验也更稳定。
            emit remoteFilesDownloaded(downloadedPaths);
        }
        if (clipboardReady)
        {
            emit fileTransferStatus(QStringLiteral("文件下载完成，已写入本地剪贴板，文件数: %1").arg(m_lastDownloadedPaths.size()));
        }
        else
        {
            emit fileTransferStatus(QStringLiteral("文件下载完成，但写入本地剪贴板失败，文件数: %1").arg(m_lastDownloadedPaths.size()));
        }
        if (hasDownloadedFiles && !m_publishClipboardAfterCurrentDownload)
        {
            emit fileTransferStatus(QStringLiteral("文件下载完成，已加入窗口列表，可直接拖出，文件数: %1").arg(downloadedPaths.size()));
        }
        const bool shouldReplayPaste = m_replayPasteAfterCurrentDownload && clipboardReady;
        if (m_replayPasteAfterCurrentDownload && !clipboardReady)
        {
            emit fileTransferStatus(QStringLiteral("已跳过自动补发 Ctrl+V：本地剪贴板文件列表未就绪"));
        }
        const bool finishedOfferWantsAutoPrefetch = offer.autoPrefetchOnReceive;
        m_remoteOfferedFiles.remove(state.sessionId);
        m_requestWindowTimer.stop();
        m_replayPasteAfterCurrentDownload = false;
        m_publishClipboardAfterCurrentDownload = true;
        m_activeDownload.reset();
        m_downloadFile.reset();
        m_downloadHash.reset();

        if (shouldReplayPaste)
        {
            emit autoPasteReplayRequested();
        }
        if (finishedOfferWantsAutoPrefetch && !m_remoteOfferedFiles.isEmpty())
        {
            scheduleNextPendingRemoteOfferRequest();
        }
        return true;
    }
    // 当前文件初始化
    // 如果 m_downloadFile 还没创建，说明当前文件首次进入
    const FileMeta &meta = offer.files[state.fileIndex];
    if (!m_downloadFile)
    {
        const QString localPath = buildDownloadPath(state.sessionId, meta.name);
        state.localPath = localPath;
        m_downloadFile = std::make_unique<QSaveFile>(localPath);
        // 写入文件
        if (!m_downloadFile->open(QIODevice::WriteOnly))
        {
            emit fileTransferStatus(QStringLiteral("无法创建本地文件: %1").arg(localPath));
            m_requestWindowTimer.stop();
            m_replayPasteAfterCurrentDownload = false;
            m_activeDownload.reset();
            m_downloadFile.reset();
            return false;
        }

        m_downloadHash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
        state.nextOffset = 0;
    }

    const qint64 remain = meta.size - state.nextOffset;
    // 还剩多少字节没收完   当前文件总大小（字节） - 收端下一次期望写入的偏移量

    if (remain <= 0)
    {
        if (meta.sha256.isEmpty())
        {
            emit fileTransferStatus(QStringLiteral("缺少远端 SHA256，拒绝提交文件: %1").arg(meta.name));
            m_downloadFile->cancelWriting();
            m_requestWindowTimer.stop();
            m_replayPasteAfterCurrentDownload = false;
            m_activeDownload.reset();
            m_downloadFile.reset();
            m_downloadHash.reset();
            return false;
        }

        // 接收端把增量哈希结果与 Offer 中的 SHA256 对比，不一致则拒绝 commit。
        const QString finalSha = QString::fromLatin1(m_downloadHash->result().toHex());
        if (finalSha.compare(meta.sha256, Qt::CaseInsensitive) != 0)
        {
            emit fileTransferStatus(QStringLiteral("文件校验失败: %1").arg(meta.name));
            m_downloadFile->cancelWriting();
            m_requestWindowTimer.stop();
            m_replayPasteAfterCurrentDownload = false;
            m_activeDownload.reset();
            m_downloadFile.reset();
            m_downloadHash.reset();
            return false;
        }

        if (!m_downloadFile->commit())
        {
            emit fileTransferStatus(QStringLiteral("本地文件提交失败: %1").arg(meta.name));
            m_requestWindowTimer.stop();
            m_replayPasteAfterCurrentDownload = false;
            m_activeDownload.reset();
            m_downloadFile.reset();
            m_downloadHash.reset();
            return false;
        }

        m_lastDownloadedPaths.push_back(state.localPath);
        emit fileTransferStatus(QStringLiteral("文件下载完成: %1").arg(meta.name));
        ++state.fileIndex;
        state.nextOffset = 0;
        state.requestedLength = 0;
        state.receivedInWindow = 0;
        state.requestId.clear();
        m_downloadFile.reset();
        m_downloadHash.reset();
        return requestNextWindow();
    }

    // 当还没收完时，它调用 sendFileRequestWindow(...) 发下一轮请求窗口
    return sendFileRequestWindow(meta, false);
}

// 根据当前已下载的进度，计算出下一段需要请求的数据范围，
// 并向远程端发送一个“请给我这部分数据”的指令，同时定好闹钟等待回信
// 理解为：从文件第 offset 字节开始，再给我 length 字节，按窗口发；如果超时我会拿同一请求单号重催一次
bool SyncCoordinator::sendFileRequestWindow(const FileMeta &meta, bool reuseRequestId)
{
    // 第一件事是检查 m_activeDownload。没有活动任务就直接返回 false
    if (!m_activeDownload.has_value())
    {
        return false;
    }

    DownloadState &state = m_activeDownload.value();
    // 计算还剩多少没下完
    const qint64 remain = meta.size - state.nextOffset;
    if (remain <= 0)
    {
        return false;
    }
    // 决定是否生成新的 requestId  条件是“不是重用”或者“当前 requestId 为空
    if (!reuseRequestId || state.requestId.isEmpty())
    {
        // 生成新 requestId
        state.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        // 计算本窗口请求长度 requestedLength
        state.requestedLength = qMin<qint64>(remain, static_cast<qint64>(m_chunkSizeBytes) * m_windowChunks);
        // 清零 receivedInWindow 和重试计数
        state.receivedInWindow = 0;
        m_currentWindowRetryCount = 0;
    }

    // 组装 FileRequest 的 JSON 负载  这就是对端读取源文件时的参数
    QJsonObject req;
    req.insert(QStringLiteral("fileId"), meta.fileId);
    req.insert(QStringLiteral("requestId"), state.requestId);
    req.insert(QStringLiteral("offset"), static_cast<double>(state.nextOffset));
    req.insert(QStringLiteral("length"), static_cast<double>(state.requestedLength));
    req.insert(QStringLiteral("windowChunks"), m_windowChunks);

    // 组装协议消息并发送
    protocol::ClipboardMessage message;
    message.type = protocol::MessageType::FileRequest;
    message.flags = 0;
    message.sessionId = state.sessionId;
    message.sequence = static_cast<quint64>(state.nextOffset / qMax(1, m_chunkSizeBytes) + 1);
    message.payload = encodeJson(req);

    if (!m_client->sendMessage(message))
    {
        return false;
    }

    // 发送成功后启动超时计时器并打状态日志
    m_requestWindowTimer.start();
    emit fileTransferStatus(QStringLiteral("请求文件窗口: %1 offset=%2 length=%3 retry=%4")
                                .arg(meta.name)
                                .arg(state.nextOffset)
                                .arg(state.requestedLength)
                                .arg(m_currentWindowRetryCount));
    return true;
}

void SyncCoordinator::handleLocalTextChanged(const QString &text, quint32 textHash)
{
    if (m_writer->isRecentlyInjected(textHash))
    {
        qInfo() << "skip local echo text hash:" << textHash;
        return;
    }

    emit localTextForwarded(text);

    const quint64 sessionId = QRandomGenerator::global()->generate64();
    if (!sendTextToPeer(text, sessionId))
    {
        qWarning() << "local clipboard forward failed";
    }
}

void SyncCoordinator::handleLocalImageChanged(const QByteArray &pngBytes, quint32 imageHash)
{
    if (m_writer->isRecentlyInjectedImage(imageHash))
    {
        qInfo() << "skip local echo image hash:" << imageHash;
        return;
    }

    const quint64 sessionId = QRandomGenerator::global()->generate64();
    emit localImageForwarded(static_cast<qint64>(pngBytes.size()));
    if (!sendImageToPeer(pngBytes, sessionId))
    {
        qWarning() << "local image forward failed";
    }
}

void SyncCoordinator::handleLocalFilesChanged(const QStringList &paths, quint32 listHash)
{
    if (m_writer->isRecentlyInjectedFileList(listHash))
    {
        qInfo() << "skip local echo file list hash:" << listHash;
        return;
    }

    const quint64 sessionId = QRandomGenerator::global()->generate64();
    emit localFilesForwarded(paths);
    if (!sendFileOfferToPeer(paths, sessionId, false))
    {
        qWarning() << "local file offer forward failed";
    }
}

void SyncCoordinator::handleRemoteMessage(const protocol::ClipboardMessage &message)
{
    switch (message.type)
    {
    case protocol::MessageType::TextPlain:
    {
        const QString text = QString::fromUtf8(message.payload);
        if (text.isEmpty())
        {
            return;
        }

        emit remoteTextReceived(text);

        qInfo() << "remote message received, bytes:" << message.payload.size() << "session:" << message.sessionId;

        if (m_writer->writeRemoteText(text, message.sessionId))
        {
            qInfo() << "remote clipboard applied";
        }
        else
        {
            qWarning() << "remote clipboard apply failed";
        }
        return;
    }
    case protocol::MessageType::ImageBitmap:
    {
        if (message.payload.isEmpty())
        {
            return;
        }

        qInfo() << "remote image received, bytes:" << message.payload.size() << "session:" << message.sessionId;

        if (m_writer->writeRemoteImage(message.payload, message.sessionId))
        {
            qInfo() << "remote image applied";
            emit remoteImageReceived(static_cast<qint64>(message.payload.size()));
        }
        else
        {
            qWarning() << "remote image apply failed";
        }
        return;
    }
    case protocol::MessageType::FileOffer:
        handleRemoteFileOffer(message); // 接收端 收到文件清单，记录在 m_remoteOfferedFiles 里，并发信号通知 UI 更新；等待用户触发下载请求后再真正进入下载流程
        return;
    case protocol::MessageType::FileRequest:
        handleRemoteFileRequest(message); // 发送端 收到Request后回多个 FileChunk
        return;
    case protocol::MessageType::FileChunk:
        handleRemoteFileChunk(message); // 接收端  把收到的文件块按顺序、带校验地写进本地临时文件，并推动下载状态继续前进
        return;
    case protocol::MessageType::FileComplete:
        handleRemoteFileComplete(message);
        return;
    case protocol::MessageType::FileAbort:
        handleRemoteFileAbort(message);
        return;
    default:
        return;
    }
}

// 记录文件清单，发出信号通知 UI 更新，准备好后续的下载请求
void SyncCoordinator::handleRemoteFileOffer(const protocol::ClipboardMessage &message)
{
    QJsonObject root;
    if (!decodeJson(message.payload, &root))
    {
        emit fileTransferStatus(QStringLiteral("收到无效的 FileOffer 负载"));
        return;
    }

    const QJsonArray files = root.value(QStringLiteral("files")).toArray();
    if (files.isEmpty())
    {
        return;
    }

    FileOffer offer;
    offer.sessionId = message.sessionId;
    offer.receivedAtMs = QDateTime::currentMSecsSinceEpoch();
    offer.autoPrefetchOnReceive = root.value(QStringLiteral("autoPrefetch")).toBool(false);
    QStringList names;
    for (const QJsonValue &value : files)
    {
        const QJsonObject obj = value.toObject();
        FileMeta meta;
        meta.fileId = obj.value(QStringLiteral("fileId")).toString();
        meta.name = obj.value(QStringLiteral("name")).toString();
        meta.size = static_cast<qint64>(obj.value(QStringLiteral("size")).toDouble());
        meta.mtimeMs = static_cast<qint64>(obj.value(QStringLiteral("mtimeMs")).toDouble());
        meta.sha256 = obj.value(QStringLiteral("sha256")).toString();
        offer.files.push_back(meta);
        names.push_back(meta.name);
    }

    // 只是“登记在册”，还没真正开始下载文件内容。
    m_remoteOfferedFiles.insert(message.sessionId, offer);
    emit remoteFileOfferReceived(names);
    emit fileTransferStatus(QStringLiteral("收到远端文件 Offer，session=%1 文件数=%2").arg(message.sessionId).arg(offer.files.size()));

    if (offer.autoPrefetchOnReceive && !m_activeDownload.has_value())
    {
        scheduleNextPendingRemoteOfferRequest();
    }
}

// 有人来向我要文件内容了，我现在按请求范围去读本地文件，然后切成多个 Chunk 发回去
void SyncCoordinator::handleRemoteFileRequest(const protocol::ClipboardMessage &message)
{
    QJsonObject req;
    if (!decodeJson(message.payload, &req))
    {
        return;
    }

    // 解析出 fileId/requestId/offset/length
    const QString fileId = req.value(QStringLiteral("fileId")).toString();
    const QString requestId = req.value(QStringLiteral("requestId")).toString();
    const qint64 offset = static_cast<qint64>(req.value(QStringLiteral("offset")).toDouble());
    const qint64 length = static_cast<qint64>(req.value(QStringLiteral("length")).toDouble());

    // 参数不合法或会话不存在时直接丢弃，避免生成无效响应流量。
    if (!m_localOfferedFiles.contains(message.sessionId) || fileId.isEmpty() || requestId.isEmpty() || offset < 0 || length <= 0)
    {
        return;
    }
    // 在本地 m_localOfferedFiles 里找到对应文件
    const FileOffer &offer = m_localOfferedFiles[message.sessionId];
    std::optional<FileMeta> target;
    for (const FileMeta &meta : offer.files)
    {
        if (meta.fileId == fileId)
        {
            target = meta;
            break;
        }
    }
    if (!target.has_value())
    {
        return;
    }

    QFile file(target->path);
    // 用只读方式打开，并跳到对端指定的偏移位置
    if (!file.open(QIODevice::ReadOnly) || !file.seek(offset))
    {
        protocol::ClipboardMessage abortMsg;
        abortMsg.type = protocol::MessageType::FileAbort;
        abortMsg.sessionId = message.sessionId;
        abortMsg.sequence = static_cast<quint64>(offset);
        QJsonObject abortPayload;
        abortPayload.insert(QStringLiteral("fileId"), fileId);
        abortPayload.insert(QStringLiteral("requestId"), requestId);
        abortPayload.insert(QStringLiteral("reason"), QStringLiteral("source file unavailable"));
        abortMsg.payload = encodeJson(abortPayload);
        m_client->sendMessage(abortMsg);
        return;
    }

    // 按块读取,每块都带上 offset 和 CRC32，接收端可以据此验序和验块，丢块时也方便重试。
    qint64 sent = 0;
    const qint64 maxToSend = qMin(length, target->size - offset);
    // 按请求窗口上限与 chunk 大小切片回传；每个 chunk 带 offset + CRC 便于接收端验序与验块。
    while (sent < maxToSend)
    {
        const qint64 thisSize = qMin<qint64>(m_chunkSizeBytes, maxToSend - sent);
        const QByteArray chunk = file.read(thisSize);
        if (chunk.isEmpty())
        {
            break;
        }

        const qint64 chunkOffset = offset + sent;
        QJsonObject meta;
        meta.insert(QStringLiteral("fileId"), fileId);
        meta.insert(QStringLiteral("requestId"), requestId);
        meta.insert(QStringLiteral("offset"), static_cast<double>(chunkOffset));
        meta.insert(QStringLiteral("chunkSize"), chunk.size());
        meta.insert(QStringLiteral("chunkCrc32"), static_cast<double>(crc32(chunk)));
        meta.insert(QStringLiteral("totalSize"), static_cast<double>(target->size));

        protocol::ClipboardMessage chunkMsg;
        chunkMsg.type = protocol::MessageType::FileChunk;
        chunkMsg.sessionId = message.sessionId;
        chunkMsg.sequence = static_cast<quint64>(chunkOffset / qMax(1, m_chunkSizeBytes) + 1);
        chunkMsg.payload = buildChunkPayload(meta, chunk);

        // FileChunk 是由“拥有原始文件的一端”产生的 产生时机是“收到对端 FileRequest 后，按要求读文件并回传
        if (!m_client->sendMessage(chunkMsg))
        {
            break;
        }

        sent += chunk.size();
    }

    // 如果已经发到文件末尾了，就发个完成帧告诉对端，并带上最终 SHA256 供对端校验。
    if (offset + sent >= target->size)
    {
        if (target->sha256.isEmpty())
        {
            target->sha256 = computeFileSha256Hex(target->path);
        }
        // 为空说明之前计算 SHA256 失败了，这时只能发个 Abort 告诉对端这个文件没法传了。
        if (target->sha256.isEmpty())
        {
            protocol::ClipboardMessage abortMsg;
            abortMsg.type = protocol::MessageType::FileAbort;
            abortMsg.sessionId = message.sessionId;
            abortMsg.sequence = static_cast<quint64>(offset + sent);
            QJsonObject abortPayload;
            abortPayload.insert(QStringLiteral("fileId"), fileId);
            abortPayload.insert(QStringLiteral("requestId"), requestId);
            abortPayload.insert(QStringLiteral("reason"), QStringLiteral("source sha256 unavailable"));
            abortMsg.payload = encodeJson(abortPayload);
            m_client->sendMessage(abortMsg);
            return;
        }

        protocol::ClipboardMessage complete;
        // 完成帧用于告知“发送端已发送到文件末尾”，并携带最终 SHA256。
        complete.type = protocol::MessageType::FileComplete;
        complete.sessionId = message.sessionId;
        complete.sequence = static_cast<quint64>(target->size);
        QJsonObject payload;
        payload.insert(QStringLiteral("fileId"), fileId);
        payload.insert(QStringLiteral("requestId"), requestId);
        payload.insert(QStringLiteral("size"), static_cast<double>(target->size));
        payload.insert(QStringLiteral("sha256"), target->sha256);
        complete.payload = encodeJson(payload);
        m_client->sendMessage(complete);
    }
}

// 处理对端送回来的文件正文块
void SyncCoordinator::handleRemoteFileChunk(const protocol::ClipboardMessage &message)
{
    if (!m_activeDownload.has_value())
    {
        return;
    }

    DownloadState &state = m_activeDownload.value();
    if (state.sessionId != message.sessionId || !m_remoteOfferedFiles.contains(state.sessionId))
    {
        return;
    }

    const FileOffer &offer = m_remoteOfferedFiles[state.sessionId];
    if (state.fileIndex >= offer.files.size())
    {
        return;
    }

    const FileMeta &meta = offer.files[state.fileIndex];
    QJsonObject chunkMeta;
    QByteArray chunk;
    if (!parseChunkPayload(message.payload, &chunkMeta, &chunk))
    {
        emit fileTransferStatus(QStringLiteral("收到无效 FileChunk"));
        return;
    }

    const QString fileId = chunkMeta.value(QStringLiteral("fileId")).toString();
    const QString requestId = chunkMeta.value(QStringLiteral("requestId")).toString();
    const qint64 offset = static_cast<qint64>(chunkMeta.value(QStringLiteral("offset")).toDouble());
    const quint32 expectedCrc = static_cast<quint32>(chunkMeta.value(QStringLiteral("chunkCrc32")).toDouble());

    // 这里同时做三重约束：fileId、requestId、offset，确保只接收“当前窗口期望的下一个块”。
    if (fileId != meta.fileId || requestId != state.requestId || offset != state.nextOffset)
    {
        emit fileTransferStatus(QStringLiteral("FileChunk 顺序不匹配，终止当前传输"));
        m_downloadFile->cancelWriting();
        m_requestWindowTimer.stop();
        m_downloadFile.reset();
        m_downloadHash.reset();
        m_replayPasteAfterCurrentDownload = false;
        m_activeDownload.reset();
        return;
    }

    if (crc32(chunk) != expectedCrc)
    {
        emit fileTransferStatus(QStringLiteral("FileChunk CRC 校验失败，终止当前传输"));
        m_downloadFile->cancelWriting();
        m_requestWindowTimer.stop();
        m_downloadFile.reset();
        m_downloadHash.reset();
        m_replayPasteAfterCurrentDownload = false;
        m_activeDownload.reset();
        return;
    }

    // 写入当前目标文件
    if (!m_downloadFile || m_downloadFile->write(chunk) != chunk.size())
    {
        emit fileTransferStatus(QStringLiteral("写本地文件失败，终止当前传输"));
        if (m_downloadFile)
        {
            m_downloadFile->cancelWriting();
        }
        m_requestWindowTimer.stop();
        m_downloadFile.reset();
        m_downloadHash.reset();
        m_replayPasteAfterCurrentDownload = false;
        m_activeDownload.reset();
        return;
    }
    // 更新状态机
    m_downloadHash->addData(chunk);
    m_requestWindowTimer.start();
    m_currentWindowRetryCount = 0;
    state.nextOffset += chunk.size();
    state.receivedInWindow += chunk.size();

    // 判断要不要继续请求  如果整个文件写完了/当前窗口收满了 回到调度函数让它决定下一步是发下一个窗口还是发下一个文件
    if (state.nextOffset >= meta.size)
    {
        requestNextWindow();
        return;
    }

    if (state.receivedInWindow >= state.requestedLength)
    {
        requestNextWindow();
    }
}

void SyncCoordinator::handleRemoteFileComplete(const protocol::ClipboardMessage &message)
{
    QJsonObject payload;
    if (!decodeJson(message.payload, &payload))
    {
        return;
    }

    const QString fileId = payload.value(QStringLiteral("fileId")).toString();
    emit fileTransferStatus(QStringLiteral("发送端声明文件完成: session=%1 fileId=%2").arg(message.sessionId).arg(fileId));
}

void SyncCoordinator::handleRemoteFileAbort(const protocol::ClipboardMessage &message)
{
    QJsonObject payload;
    if (!decodeJson(message.payload, &payload))
    {
        return;
    }

    const QString reason = payload.value(QStringLiteral("reason")).toString();
    emit fileTransferStatus(QStringLiteral("远端中止文件传输: %1").arg(reason));

    if (m_downloadFile)
    {
        m_downloadFile->cancelWriting();
    }
    m_requestWindowTimer.stop();
    m_downloadFile.reset();
    m_downloadHash.reset();
    m_replayPasteAfterCurrentDownload = false;
    m_activeDownload.reset();
}

void SyncCoordinator::handleRequestWindowTimeout()
{
    if (!m_activeDownload.has_value())
    {
        return;
    }

    if (!m_client->isConnected())
    {
        m_downloadPausedByDisconnect = true;
        emit fileTransferStatus(QStringLiteral("请求窗口超时且连接断开，等待重连后续传"));
        return;
    }

    DownloadState &state = m_activeDownload.value();
    if (!m_remoteOfferedFiles.contains(state.sessionId))
    {
        return;
    }

    const FileOffer &offer = m_remoteOfferedFiles[state.sessionId];
    if (state.fileIndex >= offer.files.size())
    {
        return;
    }

    if (m_currentWindowRetryCount >= m_maxRequestWindowRetries)
    {
        emit fileTransferStatus(QStringLiteral("请求窗口超时重试达到上限，终止当前下载"));
        if (m_downloadFile)
        {
            m_downloadFile->cancelWriting();
        }
        m_downloadFile.reset();
        m_downloadHash.reset();
        m_replayPasteAfterCurrentDownload = false;
        m_activeDownload.reset();
        return;
    }

    ++m_currentWindowRetryCount;
    emit fileTransferStatus(QStringLiteral("请求窗口超时，重试第 %1 次").arg(m_currentWindowRetryCount));
    sendFileRequestWindow(offer.files[state.fileIndex], true);
}

void SyncCoordinator::handlePeerConnected()
{
    if (!m_downloadPausedByDisconnect || !m_activeDownload.has_value())
    {
        return;
    }

    m_downloadPausedByDisconnect = false;
    emit fileTransferStatus(QStringLiteral("连接恢复，继续文件续传"));
    requestNextWindow();
}

void SyncCoordinator::handlePeerDisconnected()
{
    if (!m_activeDownload.has_value())
    {
        return;
    }

    m_downloadPausedByDisconnect = true;
    m_requestWindowTimer.stop();
    emit fileTransferStatus(QStringLiteral("连接断开，已暂停当前文件下载"));
}
