#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

class QMimeData;

namespace clipboard
{
    struct FileDescriptor
    {
        // Snapshot 里的文件元信息只负责“声明有这个文件”，
        // 真正的文件字节仍然走现有的 FileRequest/FileChunk 通道。
        QString fileId;
        QString path;
        QString name;
        qint64 size = 0;
        qint64 mtimeMs = 0;
        QString sha256;
    };

    struct Snapshot
    {
        // 一次复制动作的“多格式快照”。
        // 同一个 snapshot 可以同时携带 text/html/image/files，
        // 后续发送、接收、写回都围绕这个统一对象进行。
        QString text;
        QString html;
        QByteArray imagePng;
        QStringList localFilePaths;
        QVector<FileDescriptor> files;

        // 这些 hash/fingerprint 不直接参与业务展示，
        // 主要用于去重、防回环和“写回后再读取”时的内容校验。
        quint32 textHash = 0;
        quint32 htmlHash = 0;
        quint32 imageHash = 0;
        quint32 localFilesHash = 0;
        quint32 transportFilesHash = 0;
        quint32 fingerprint = 0;

        bool hasText() const;
        bool hasHtml() const;
        bool hasImage() const;
        bool hasLocalFiles() const;
        bool hasTransportFiles() const;
        bool isEmpty() const;
    };

    // 从系统 QMimeData 中尽可能一次性提取多种格式，
    // 不再局限于“文本/图片/文件三选一”。
    Snapshot captureSnapshotFromMime(const QMimeData *mime, const QString &clipboardTextFallback = QString());
    // 当 snapshot 内容变化后，统一重算各类 hash 和总 fingerprint。
    void refreshSnapshotFingerprint(Snapshot *snapshot);
    quint32 hashSnapshot(const Snapshot &snapshot);
    // Snapshot 的网络传输目前走 JSON，便于先把协议主线跑通。
    QJsonObject snapshotToJson(const Snapshot &snapshot);
    bool snapshotFromJson(const QJsonObject &json, Snapshot *outSnapshot);
} // namespace clipboard

Q_DECLARE_METATYPE(clipboard::Snapshot)
