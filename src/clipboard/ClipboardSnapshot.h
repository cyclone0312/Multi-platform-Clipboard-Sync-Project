#pragma once

#include <QByteArray>
#include <QMetaType>
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
        // Declares a file in the snapshot; bytes still flow through FileRequest/FileChunk.
        QString fileId;
        QString path;
        QString name;
        qint64 size = 0;
        qint64 mtimeMs = 0;
        QString sha256;
    };

    struct Snapshot
    {
        // A single clipboard capture can carry text/html/image/files together.
        QString text;
        QString html;
        QByteArray imagePng;
        QStringList localFilePaths;
        QVector<FileDescriptor> files;

        // Used for dedupe, loop prevention, and writeback verification.
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

    // Extract as many formats as possible from one QMimeData payload.
    Snapshot captureSnapshotFromMime(const QMimeData *mime, const QString &clipboardTextFallback = QString());
    // Recompute hashes and the fingerprint after any content mutation.
    void refreshSnapshotFingerprint(Snapshot *snapshot);
    quint32 hashSnapshot(const Snapshot &snapshot);
    // Snapshot transport currently uses JSON to keep the protocol simple.
    QJsonObject snapshotToJson(const Snapshot &snapshot);
    bool snapshotFromJson(const QJsonObject &json, Snapshot *outSnapshot);
} // namespace clipboard

Q_DECLARE_METATYPE(clipboard::Snapshot)