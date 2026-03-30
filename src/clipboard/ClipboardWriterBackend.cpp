#include "clipboard/ClipboardWriter.h"

#include <utility>

#include <QDebug>
#include <QThread>

#include "clipboard/QtClipboardBackend.h"
#include "clipboard/WindowsClipboardBackend.h"
#include "clipboard/X11ClipboardBackend.h"

namespace
{
    QStringList localPathsFromProvider(IVirtualFileProvider *provider,
                                       quint64 sessionId,
                                       const QVector<clipboard::FileDescriptor> &files)
    {
        if (!provider || files.isEmpty() || !provider->canProvideFiles(sessionId, files))
        {
            return {};
        }

        const QVector<ClipboardVirtualFileInfo> described = provider->describeFiles(sessionId, files);
        if (described.size() != files.size())
        {
            return {};
        }

        QStringList localPaths;
        localPaths.reserve(described.size());
        for (const ClipboardVirtualFileInfo &file : described)
        {
            if (file.localPath.isEmpty())
            {
                return {};
            }

            localPaths.push_back(file.localPath);
        }

        return localPaths;
    }

    std::unique_ptr<IClipboardBackend> createDefaultClipboardBackend()
    {
#if defined(Q_OS_WIN)
        return std::make_unique<WindowsClipboardBackend>();
#elif defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
        return std::make_unique<X11ClipboardBackend>();
#else
        return std::make_unique<QtClipboardBackend>();
#endif
    }

    clipboard::Snapshot makeTextSnapshot(const QString &text)
    {
        clipboard::Snapshot snapshot;
        snapshot.text = text;
        clipboard::refreshSnapshotFingerprint(&snapshot);
        return snapshot;
    }

    clipboard::Snapshot makeImageSnapshot(const QByteArray &pngBytes)
    {
        clipboard::Snapshot snapshot;
        snapshot.imagePng = pngBytes;
        clipboard::refreshSnapshotFingerprint(&snapshot);
        return snapshot;
    }

    clipboard::Snapshot makeFileListSnapshot(const QStringList &paths)
    {
        clipboard::Snapshot snapshot;
        snapshot.localFilePaths = paths;
        clipboard::refreshSnapshotFingerprint(&snapshot);
        return snapshot;
    }

    bool snapshotMatchesExpected(const clipboard::Snapshot &expected, const clipboard::Snapshot &actual)
    {
        if (expected.hasText() && expected.textHash != actual.textHash)
        {
            return false;
        }

        if (expected.hasHtml() && expected.htmlHash != actual.htmlHash)
        {
            return false;
        }

        if (expected.hasImage() && expected.imageHash != actual.imageHash)
        {
            return false;
        }

        if (expected.hasLocalFiles() && expected.localFilesHash != actual.localFilesHash)
        {
            return false;
        }

        if (expected.hasTransportFiles() && expected.transportFilesHash != actual.transportFilesHash)
        {
            if (!expected.hasLocalFiles() || expected.localFilesHash != actual.localFilesHash)
            {
                return false;
            }
        }

        return !expected.isEmpty();
    }
}

ClipboardWriter::ClipboardWriter(QObject *parent)
    : ClipboardWriter(createDefaultClipboardBackend(), parent)
{
}

ClipboardWriter::ClipboardWriter(std::unique_ptr<IClipboardBackend> backend, QObject *parent)
    : QObject(parent), m_backend(std::move(backend))
{
    if (!m_backend)
    {
        m_backend = createDefaultClipboardBackend();
    }
}

void ClipboardWriter::setVirtualFileProvider(IVirtualFileProvider *provider)
{
    m_virtualFileProvider = provider;
}

bool ClipboardWriter::writeRemoteSnapshot(const clipboard::Snapshot &snapshot, quint64 sessionId)
{
    cleanupExpired();

    if (!m_backend || snapshot.isEmpty())
    {
        return false;
    }

    clipboard::Snapshot expected = snapshot;
    if (expected.hasTransportFiles() && !expected.hasLocalFiles())
    {
        const QStringList materializedPaths = localPathsFromProvider(
            m_virtualFileProvider, sessionId, expected.files);
        if (!materializedPaths.isEmpty())
        {
            expected.localFilePaths = materializedPaths;
        }
    }
    clipboard::refreshSnapshotFingerprint(&expected);

    if (expected.hasTransportFiles() &&
        !expected.hasLocalFiles() &&
        !m_backend->supportsNativeVirtualFiles())
    {
        qWarning() << "clipboard backend cannot publish transport-only file snapshots:"
                   << m_backend->backendName();
        return false;
    }

    const clipboard::Snapshot current = m_backend->readCurrentSnapshot();
    if (snapshotMatchesExpected(expected, current))
    {
        markInjected(current);
        return true;
    }

    for (int attempt = 1; attempt <= m_writeRetryCount; ++attempt)
    {
        ClipboardWriteRequest request;
        request.snapshot = expected;
        request.sessionId = sessionId;
        request.virtualFileProvider = m_virtualFileProvider;

        if (!m_backend->writeSnapshot(request))
        {
            if (attempt < m_writeRetryCount)
            {
                QThread::msleep(static_cast<unsigned long>(m_writeRetryDelayMs));
                continue;
            }

            break;
        }

        const clipboard::Snapshot actual = m_backend->readCurrentSnapshot();
        if (snapshotMatchesExpected(expected, actual))
        {
            markInjected(actual);
            if (attempt > 1)
            {
                qInfo() << "clipboard snapshot write recovered after retries:"
                        << attempt
                        << "backend:" << m_backend->backendName();
            }
            return true;
        }

        if (attempt < m_writeRetryCount)
        {
            QThread::msleep(static_cast<unsigned long>(m_writeRetryDelayMs));
        }
    }

    qWarning() << "failed to set clipboard snapshot after retries, backend:"
               << m_backend->backendName();
    return false;
}

bool ClipboardWriter::writeRemoteText(const QString &text, quint64 sessionId)
{
    return writeRemoteSnapshot(makeTextSnapshot(text), sessionId);
}

bool ClipboardWriter::writeRemoteImage(const QByteArray &pngBytes, quint64 sessionId)
{
    return writeRemoteSnapshot(makeImageSnapshot(pngBytes), sessionId);
}

bool ClipboardWriter::writeRemoteFileList(const QStringList &paths, quint64 sessionId)
{
    return writeRemoteSnapshot(makeFileListSnapshot(paths), sessionId);
}

bool ClipboardWriter::isRecentlyInjected(quint32 textHash) const
{
    cleanupExpired();
    return m_recentInjectedHashes.contains(textHash);
}

bool ClipboardWriter::isRecentlyInjectedSnapshot(quint32 snapshotHash) const
{
    cleanupExpired();
    return m_recentInjectedSnapshotHashes.contains(snapshotHash);
}

bool ClipboardWriter::isRecentlyInjectedImage(quint32 imageHash) const
{
    cleanupExpired();
    return m_recentInjectedImageHashes.contains(imageHash);
}

bool ClipboardWriter::isRecentlyInjectedFileList(quint32 listHash) const
{
    cleanupExpired();
    return m_recentInjectedFileHashes.contains(listHash);
}

void ClipboardWriter::markInjected(const clipboard::Snapshot &applied)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();

    if (applied.fingerprint != 0)
    {
        m_recentInjectedSnapshotHashes.insert(applied.fingerprint, now);
    }
    if (applied.textHash != 0)
    {
        m_recentInjectedHashes.insert(applied.textHash, now);
    }
    if (applied.imageHash != 0)
    {
        m_recentInjectedImageHashes.insert(applied.imageHash, now);
    }
    if (applied.localFilesHash != 0)
    {
        m_recentInjectedFileHashes.insert(applied.localFilesHash, now);
    }
}

void ClipboardWriter::cleanupExpired() const
{
    const QDateTime now = QDateTime::currentDateTimeUtc();

    for (auto it = m_recentInjectedHashes.begin(); it != m_recentInjectedHashes.end();)
    {
        if (it.value().msecsTo(now) > m_injectionTtlMs)
        {
            it = m_recentInjectedHashes.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = m_recentInjectedSnapshotHashes.begin(); it != m_recentInjectedSnapshotHashes.end();)
    {
        if (it.value().msecsTo(now) > m_injectionTtlMs)
        {
            it = m_recentInjectedSnapshotHashes.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = m_recentInjectedFileHashes.begin(); it != m_recentInjectedFileHashes.end();)
    {
        if (it.value().msecsTo(now) > m_injectionTtlMs)
        {
            it = m_recentInjectedFileHashes.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = m_recentInjectedImageHashes.begin(); it != m_recentInjectedImageHashes.end();)
    {
        if (it.value().msecsTo(now) > m_injectionTtlMs)
        {
            it = m_recentInjectedImageHashes.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
