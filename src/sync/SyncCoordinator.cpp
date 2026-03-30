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
                         &ClipboardMonitor::localSnapshotChanged,
                         this,
                         &SyncCoordinator::handleLocalSnapshotChanged);
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
    clipboard::Snapshot snapshot;
    snapshot.text = text;
    clipboard::refreshSnapshotFingerprint(&snapshot);

    // 手动注入现在也先转成 snapshot，这样整条发送链路保持统一。
    if (!m_writer->writeRemoteSnapshot(snapshot, sessionId))
    {
        qWarning() << "manual inject failed to write local clipboard";
        return false;
    }

    emit localTextForwarded(text);
    return sendSnapshotToPeer(snapshot, sessionId);
}

bool SyncCoordinator::requestPendingRemoteFiles()
{
    return startPendingRemoteFilesRequest(false);
}

bool SyncCoordinator::startPendingRemoteFilesRequest(bool replayPasteAfterDownload)
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

    m_lastDownloadedPaths.clear();
    DownloadState state;
    state.sessionId = latestSessionId;
    state.fileIndex = 0;
    state.nextOffset = 0;
    m_activeDownload = state;
    m_replayPasteAfterCurrentDownload = replayPasteAfterDownload;
    m_currentWindowRetryCount = 0;
    m_downloadPausedByDisconnect = false;
    m_requestWindowTimer.stop();

    emit fileTransferStatus(QStringLiteral("开始请求远端文件，总数: %1").arg(offer.files.size()));
    return requestNextWindow();
}

bool SyncCoordinator::requestPendingRemoteFilesOnPasteTrigger()
{
    if (!shouldInterceptPasteTrigger())
    {
        return false;
    }

    return startPendingRemoteFilesRequest(true);
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
    return startPendingRemoteFilesRequest(false);
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

bool SyncCoordinator::sendSnapshotToPeer(const clipboard::Snapshot &snapshot, quint64 sessionId)
{
    if (snapshot.isEmpty())
    {
        return false;
    }

    // Snapshot 消息负责传“这次复制有哪些格式”。
    // 对于文件，只传元信息，不在这里传文件正文。
    protocol::ClipboardMessage message;
    message.type = protocol::MessageType::ClipboardSnapshot;
    message.flags = 0;
    message.sessionId = sessionId;
    message.sequence = 1;
    message.payload = encodeJson(clipboard::snapshotToJson(snapshot));

    qInfo() << "sending snapshot bytes:" << message.payload.size() << "session:" << message.sessionId;

    if (!m_client->sendMessage(message))
    {
        qWarning() << "send snapshot failed (peer may be offline)";
        return false;
    }

    return true;
}

bool SyncCoordinator::populateSnapshotFiles(const QStringList &paths, quint64 sessionId, clipboard::Snapshot *snapshot)
{
    if (!snapshot)
    {
        return false;
    }

    FileOffer offer;
    offer.sessionId = sessionId;
    snapshot->files.clear();

    // 这里是 snapshot 和现有文件流状态机之间的桥：
    // 先把本地路径转成可传输的文件元信息，同时缓存到 m_localOfferedFiles，
    // 后续远端真正拉文件时，仍然复用现有 FileRequest/FileChunk 机制。
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
        meta.sha256 = computeFileSha256Hex(meta.path);
        if (meta.sha256.isEmpty())
        {
            qWarning() << "skip file snapshot because SHA256 failed:" << meta.path;
            continue;
        }

        clipboard::FileDescriptor file;
        file.fileId = meta.fileId;
        file.path = meta.path;
        file.name = meta.name;
        file.size = meta.size;
        file.mtimeMs = meta.mtimeMs;
        file.sha256 = meta.sha256;

        offer.files.push_back(meta);
        snapshot->files.push_back(file);
    }

    if (offer.files.isEmpty())
    {
        return false;
    }

    m_localOfferedFiles.insert(sessionId, offer);
    clipboard::refreshSnapshotFingerprint(snapshot);
    return true;
}

bool SyncCoordinator::sendFileOfferToPeer(const QStringList &paths, quint64 sessionId)
{
    FileOffer offer;
    offer.sessionId = sessionId;

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
        bool clipboardReady = !m_lastDownloadedPaths.isEmpty();
        if (clipboardReady)
        {
            // 将远端文件列表写入本地剪贴板，并记录防回环指纹。
            clipboardReady = m_writer->writeRemoteFileList(m_lastDownloadedPaths, state.sessionId);
        }
        if (clipboardReady)
        {
            emit fileTransferStatus(QStringLiteral("文件下载完成，已写入本地剪贴板，文件数: %1").arg(m_lastDownloadedPaths.size()));
        }
        else
        {
            emit fileTransferStatus(QStringLiteral("文件下载完成，但写入本地剪贴板失败，文件数: %1").arg(m_lastDownloadedPaths.size()));
        }
        const bool shouldReplayPaste = m_replayPasteAfterCurrentDownload && clipboardReady;
        if (m_replayPasteAfterCurrentDownload && !clipboardReady)
        {
            emit fileTransferStatus(QStringLiteral("已跳过自动补发 Ctrl+V：本地剪贴板文件列表未就绪"));
        }
        m_remoteOfferedFiles.remove(state.sessionId);
        m_requestWindowTimer.stop();
        m_replayPasteAfterCurrentDownload = false;
        m_activeDownload.reset();
        m_downloadFile.reset();
        m_downloadHash.reset();

        if (shouldReplayPaste)
        {
            emit autoPasteReplayRequested();
        }

#if !defined(Q_OS_WIN) && !defined(CLIPBOARD_SYNC_HAS_X11_HOOK)
        // 非 Windows 平台继续尝试后续 Offer，避免依赖全局粘贴钩子。
        if (!m_remoteOfferedFiles.isEmpty())
        {
            QMetaObject::invokeMethod(this, [this]()
                                      { requestPendingRemoteFiles(); }, Qt::QueuedConnection);
        }
#endif
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

void SyncCoordinator::handleLocalSnapshotChanged(const clipboard::Snapshot &snapshot)
{
    if (snapshot.isEmpty())
    {
        return;
    }

    // 本地监听器现在先看 snapshot 级指纹，
    // 这样一次“多格式写回”不会被拆成多条本地事件再转发出去。
    if (m_writer->isRecentlyInjectedSnapshot(snapshot.fingerprint))
    {
        qInfo() << "skip local echo snapshot hash:" << snapshot.fingerprint;
        return;
    }

    const quint64 sessionId = QRandomGenerator::global()->generate64();
    clipboard::Snapshot outbound = snapshot;

    if (snapshot.hasText())
    {
        emit localTextForwarded(snapshot.text);
    }

    if (snapshot.hasImage())
    {
        emit localImageForwarded(static_cast<qint64>(snapshot.imagePng.size()));
    }

    if (snapshot.hasLocalFiles())
    {
        // 本机剪贴板里的 file:// 路径不能直接发给远端；
        // 这里要先转成跨机器可理解的文件元信息。
        if (!populateSnapshotFiles(snapshot.localFilePaths, sessionId, &outbound))
        {
            qWarning() << "local clipboard snapshot file offer forward failed";
            return;
        }
        emit localFilesForwarded(snapshot.localFilePaths);
    }

    if (!sendSnapshotToPeer(outbound, sessionId))
    {
        qWarning() << "local clipboard snapshot forward failed";
    }
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
    if (!sendFileOfferToPeer(paths, sessionId))
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
    case protocol::MessageType::ClipboardSnapshot:
        handleRemoteSnapshot(message);
        return;
    case protocol::MessageType::FileOffer:
        handleRemoteFileOffer(message);
        return;
    case protocol::MessageType::FileRequest:
        handleRemoteFileRequest(message);
        return;
    case protocol::MessageType::FileChunk:
        handleRemoteFileChunk(message);
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

void SyncCoordinator::handleRemoteSnapshot(const protocol::ClipboardMessage &message)
{
    QJsonObject root;
    if (!decodeJson(message.payload, &root))
    {
        qWarning() << "received invalid clipboard snapshot payload";
        return;
    }

    clipboard::Snapshot snapshot;
    if (!clipboard::snapshotFromJson(root, &snapshot) || snapshot.isEmpty())
    {
        qWarning() << "received empty clipboard snapshot payload";
        return;
    }

    // 如果 snapshot 里带的是文件元信息，就不要直接写回系统剪贴板。
    // 文件仍然要先进入 offer 缓存，等待后续 Ctrl+V/自动拉取流程触发下载。
    if (snapshot.hasTransportFiles())
    {
        FileOffer offer;
        offer.sessionId = message.sessionId;
        offer.receivedAtMs = QDateTime::currentMSecsSinceEpoch();

        QStringList names;
        for (const clipboard::FileDescriptor &file : snapshot.files)
        {
            FileMeta meta;
            meta.fileId = file.fileId;
            meta.name = file.name;
            meta.size = file.size;
            meta.mtimeMs = file.mtimeMs;
            meta.sha256 = file.sha256;
            offer.files.push_back(meta);
            names.push_back(meta.name);
        }

        if (!offer.files.isEmpty())
        {
            m_remoteOfferedFiles.insert(message.sessionId, offer);
            emit remoteFileOfferReceived(names);
            emit fileTransferStatus(QStringLiteral("鏀跺埌杩滅 snapshot 鏂囦欢 Offer锛宻ession=%1 鏂囦欢鏁?%2")
                                        .arg(message.sessionId)
                                        .arg(offer.files.size()));

#if !defined(Q_OS_WIN) && !defined(CLIPBOARD_SYNC_HAS_X11_HOOK)
            if (!m_activeDownload.has_value())
            {
                QMetaObject::invokeMethod(this, [this]()
                                          { requestPendingRemoteFiles(); }, Qt::QueuedConnection);
            }
#endif
        }

        return;
    }

    // 非文件 snapshot 可以直接落到本地剪贴板，
    // 例如 text/html/image 的组合会在这里一次写回。
    qInfo() << "remote snapshot received, bytes:" << message.payload.size() << "session:" << message.sessionId;

    if (m_writer->writeRemoteSnapshot(snapshot, message.sessionId))
    {
        if (snapshot.hasText())
        {
            emit remoteTextReceived(snapshot.text);
        }
        if (snapshot.hasImage())
        {
            emit remoteImageReceived(static_cast<qint64>(snapshot.imagePng.size()));
        }
        qInfo() << "remote snapshot applied";
    }
    else
    {
        qWarning() << "remote snapshot apply failed";
    }
}

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

    m_remoteOfferedFiles.insert(message.sessionId, offer);
    emit remoteFileOfferReceived(names);
    emit fileTransferStatus(QStringLiteral("收到远端文件 Offer，session=%1 文件数=%2").arg(message.sessionId).arg(offer.files.size()));

#if !defined(Q_OS_WIN) && !defined(CLIPBOARD_SYNC_HAS_X11_HOOK)
    // 非 Windows 平台默认没有全局 Ctrl+V 监听，收到 Offer 后自动发起拉取。
    if (!m_activeDownload.has_value())
    {
        QMetaObject::invokeMethod(this, [this]()
                                  { requestPendingRemoteFiles(); }, Qt::QueuedConnection);
    }
#endif
}

void SyncCoordinator::handleRemoteFileRequest(const protocol::ClipboardMessage &message)
{
    QJsonObject req;
    if (!decodeJson(message.payload, &req))
    {
        return;
    }

    const QString fileId = req.value(QStringLiteral("fileId")).toString();
    const QString requestId = req.value(QStringLiteral("requestId")).toString();
    const qint64 offset = static_cast<qint64>(req.value(QStringLiteral("offset")).toDouble());
    const qint64 length = static_cast<qint64>(req.value(QStringLiteral("length")).toDouble());

    // 参数不合法或会话不存在时直接丢弃，避免生成无效响应流量。
    if (!m_localOfferedFiles.contains(message.sessionId) || fileId.isEmpty() || requestId.isEmpty() || offset < 0 || length <= 0)
    {
        return;
    }

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

        if (!m_client->sendMessage(chunkMsg))
        {
            break;
        }

        sent += chunk.size();
    }

    if (offset + sent >= target->size)
    {
        if (target->sha256.isEmpty())
        {
            target->sha256 = computeFileSha256Hex(target->path);
        }

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

    m_downloadHash->addData(chunk);
    m_requestWindowTimer.start();
    m_currentWindowRetryCount = 0;
    state.nextOffset += chunk.size();
    state.receivedInWindow += chunk.size();

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
