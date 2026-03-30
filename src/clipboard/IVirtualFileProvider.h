#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include "clipboard/ClipboardSnapshot.h"

struct ClipboardVirtualFileInfo
{
    QString fileId;
    QString displayName;
    QString localPath;
    qint64 size = 0;
    qint64 mtimeMs = 0;
    QString sha256;
};

struct ClipboardVirtualFileRangeRequest
{
    quint64 sessionId = 0;
    QString fileId;
    qint64 offset = 0;
    qint64 length = 0;
};

class IVirtualFileProvider
{
public:
    virtual ~IVirtualFileProvider() = default;

    // Whether this provider can currently back every file in the session.
    virtual bool canProvideFiles(quint64 sessionId,
                                 const QVector<clipboard::FileDescriptor> &files) const = 0;
    // Describe the file set and expose any already-materialized local path.
    virtual QVector<ClipboardVirtualFileInfo> describeFiles(
        quint64 sessionId,
        const QVector<clipboard::FileDescriptor> &files) const = 0;
    // Read a file range from the provider. This is the future hook for native
    // virtual-file backends that need random-access reads during paste.
    virtual QByteArray readFileRange(const ClipboardVirtualFileRangeRequest &request,
                                     bool *ok) = 0;
};
