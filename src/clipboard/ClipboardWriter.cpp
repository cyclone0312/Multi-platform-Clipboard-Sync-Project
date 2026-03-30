// Legacy implementation kept only for reference. The active build now uses
// src/clipboard/ClipboardWriterBackend.cpp.
#include "clipboard/ClipboardWriter.h"

#include <utility>

#include <QDebug>
#include <QThread>
#include "clipboard/QtClipboardBackend.h"
#include "clipboard/WindowsClipboardBackend.h"
#include "clipboard/X11ClipboardBackend.h"

namespace
{
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
            return false;
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
        m_backend = std::make_unique<QtClipboardBackend>();
    }
}

bool ClipboardWriter::writeRemoteSnapshot(const clipboard::Snapshot &snapshot, quint64 sessionId)
{
    Q_UNUSED(sessionId)
    cleanupExpired();
    if (m_backend)
    {
        if (snapshot.isEmpty())
        {
            return false;
        }

        clipboard::Snapshot expected = snapshot;
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
            if (!m_backend->writeSnapshot(expected))
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

    // Legacy fallback kept temporarily while the backend split settles.
    if (snapshot.isEmpty())
    {
        return false;
    }

    ::clipboard::Snapshot expected = snapshot;
    ::clipboard::refreshSnapshotFingerprint(&expected);

    const auto markInjected = [this](const ::clipboard::Snapshot &applied)
    {
        // 既记录 snapshot 级指纹，也记录旧的 text/image/files 指纹，
        // 这样新的 snapshot 流程和旧的单格式回环抑制都能继续工作。
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
    };

    QClipboard *systemClipboard = QGuiApplication::clipboard();
    const ::clipboard::Snapshot current = ::clipboard::captureSnapshotFromMime(systemClipboard->mimeData(), systemClipboard->text());
    // 如果系统剪贴板里本来就已经是同一个 snapshot，就不用重复 setMimeData，
    // 但仍要记一次“最近注入”，避免监听器马上把它回传出去。
    if (snapshotMatchesExpected(expected, current))
    {
        markInjected(current);
        return true;
    }

    QImage image;
    if (expected.hasImage() && !image.loadFromData(expected.imagePng, "PNG") && !image.loadFromData(expected.imagePng))
    {
        qWarning() << "failed to decode incoming snapshot image payload";
        return false;
    }

    for (int attempt = 1; attempt <= m_writeRetryCount; ++attempt)
    {
        // 关键点在这里：一次性构造同一个 QMimeData，
        // 让 text/html/image/files 作为一个整体写回系统剪贴板。
        auto *mimeData = new QMimeData();
        if (expected.hasText())
        {
            mimeData->setText(expected.text);
        }
        if (expected.hasHtml())
        {
            mimeData->setHtml(expected.html);
        }
        if (expected.hasImage())
        {
            mimeData->setData(QStringLiteral("image/png"), expected.imagePng);
            mimeData->setImageData(image);
        }
        if (expected.hasLocalFiles())
        {
            QList<QUrl> urls;
            urls.reserve(expected.localFilePaths.size());
            for (const QString &path : expected.localFilePaths)
            {
                urls.push_back(QUrl::fromLocalFile(path));
            }
            mimeData->setUrls(urls);
        }

        systemClipboard->setMimeData(mimeData);

        const ::clipboard::Snapshot actual = ::clipboard::captureSnapshotFromMime(systemClipboard->mimeData(), systemClipboard->text());
        // 写完再读一遍 snapshot，是为了确认系统后端实际接受了我们期望的格式组合。
        if (snapshotMatchesExpected(expected, actual))
        {
            markInjected(actual);
            if (attempt > 1)
            {
                qInfo() << "clipboard snapshot write recovered after retries:" << attempt;
            }
            return true;
        }

        if (attempt < m_writeRetryCount)
        {
            QThread::msleep(static_cast<unsigned long>(m_writeRetryDelayMs));
        }
    }

    qWarning() << "failed to set clipboard snapshot after retries";
    return false;
}

bool ClipboardWriter::writeRemoteText(const QString &text, quint64 sessionId)
{
    if (m_backend)
    {
        return writeRemoteSnapshot(makeTextSnapshot(text), sessionId);
    }

    // Legacy fallback kept temporarily while the backend split settles.
    Q_UNUSED(sessionId)
    // 清理过期注入记录
    cleanupExpired();
    const QString canonicalIncoming = canonicalClipboardText(text);
    const quint32 hash = qHash(canonicalIncoming);

    // 拿到 Qt 的全局剪贴板接口，后续读写都通过它
    QClipboard *clipboard = QGuiApplication::clipboard();
    // 如果目标文本本来就已经在剪贴板里，不必重复写
    // 但仍要记录“最近注入哈希”，这样监听器触发时可识别并抑制回环
    // 然后返回成功
    if (canonicalClipboardText(clipboard->text()) == canonicalIncoming)
    {
        m_recentInjectedHashes.insert(hash, QDateTime::currentDateTimeUtc());
        m_recentInjectedSnapshotHashes.insert(makeTextSnapshot(canonicalIncoming).fingerprint, QDateTime::currentDateTimeUtc());
        return true;
    }

    for (int attempt = 1; attempt <= m_writeRetryCount; ++attempt)
    {
        clipboard->setText(text);

        const QString actualText = clipboard->text();
        if (canonicalClipboardText(actualText) == canonicalIncoming)
        {
            m_recentInjectedHashes.insert(qHash(canonicalClipboardText(actualText)), QDateTime::currentDateTimeUtc());
            m_recentInjectedSnapshotHashes.insert(makeTextSnapshot(actualText).fingerprint, QDateTime::currentDateTimeUtc());
            if (attempt > 1)
            {
                qInfo() << "clipboard write recovered after retries:" << attempt;
            }
            return true;
        }

        if (attempt < m_writeRetryCount)
        {
            QThread::msleep(static_cast<unsigned long>(m_writeRetryDelayMs));
        }
    }

    qWarning() << "failed to set clipboard text after retries";
    return false;
}

bool ClipboardWriter::writeRemoteImage(const QByteArray &pngBytes, quint64 sessionId)
{
    if (m_backend)
    {
        return writeRemoteSnapshot(makeImageSnapshot(pngBytes), sessionId);
    }

    // Legacy fallback kept temporarily while the backend split settles.
    Q_UNUSED(sessionId)
    cleanupExpired();
    if (pngBytes.isEmpty())
    {
        return false;
    }

    QImage image;
    if (!image.loadFromData(pngBytes, "PNG"))
    {
        if (!image.loadFromData(pngBytes))
        {
            qWarning() << "failed to decode incoming image payload";
            return false;
        }
    }

    const quint32 incomingHash = hashImageContent(image);
    const quint32 incomingPngHash = hashImagePngBytes(pngBytes);
    if (incomingHash == 0 && incomingPngHash == 0)
    {
        return false;
    }

    const auto markImageInjected = [this](quint32 contentHash, quint32 pngHash)
    {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        if (contentHash != 0)
        {
            m_recentInjectedImageHashes.insert(contentHash, now);
        }
        if (pngHash != 0)
        {
            m_recentInjectedImageHashes.insert(pngHash, now);
        }
    };

    const auto markImageSnapshotInjected = [this](const QByteArray &appliedPng)
    {
        // 老的图片写回接口仍然存在，所以这里顺手补一份 snapshot 指纹，
        // 让新旧两条路径的防回环行为保持一致。
        const clipboard::Snapshot snapshot = makeImageSnapshot(appliedPng);
        if (snapshot.fingerprint != 0)
        {
            m_recentInjectedSnapshotHashes.insert(snapshot.fingerprint, QDateTime::currentDateTimeUtc());
        }
    };

    QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *current = clipboard->mimeData();
    if (current)
    {
        QImage currentImage;
        if (extractImage(current, &currentImage) && hashImageContent(currentImage) == incomingHash)
        {
            markImageInjected(incomingHash, incomingPngHash);
            markImageSnapshotInjected(pngBytes);
            return true;
        }
    }

    for (int attempt = 1; attempt <= m_writeRetryCount; ++attempt)
    {
        auto *mimeData = new QMimeData();
        mimeData->setData(QStringLiteral("image/png"), pngBytes);
        mimeData->setImageData(image);
        clipboard->setMimeData(mimeData);

        const QMimeData *actual = clipboard->mimeData();
        QImage actualImage;
        if (extractImage(actual, &actualImage) && hashImageContent(actualImage) == incomingHash)
        {
            quint32 actualPngHash = 0;
            if (actual)
            {
                actualPngHash = hashImagePngBytes(actual->data(QStringLiteral("image/png")));
            }
            markImageInjected(hashImageContent(actualImage), incomingPngHash);
            if (actualPngHash != 0)
            {
                markImageInjected(0, actualPngHash);
            }
            if (attempt > 1)
            {
                qInfo() << "clipboard image write recovered after retries:" << attempt;
            }
            markImageSnapshotInjected(actual && !actual->data(QStringLiteral("image/png")).isEmpty()
                                          ? actual->data(QStringLiteral("image/png"))
                                          : pngBytes);
            return true;
        }

        if (attempt < m_writeRetryCount)
        {
            QThread::msleep(static_cast<unsigned long>(m_writeRetryDelayMs));
        }
    }

    qWarning() << "failed to set clipboard image after retries";
    return false;
}

// 将远端文件列表写入本地剪贴板，并记录防回环指纹。
bool ClipboardWriter::writeRemoteFileList(const QStringList &paths, quint64 sessionId)
{
    if (m_backend)
    {
        return writeRemoteSnapshot(makeFileListSnapshot(paths), sessionId);
    }

    // Legacy fallback kept temporarily while the backend split settles.
    Q_UNUSED(sessionId)
    cleanupExpired();
    if (paths.isEmpty())
    {
        return false;
    }

    QClipboard *clipboard = QGuiApplication::clipboard();
    const quint32 expectedHash = hashFileList(paths);
    for (int attempt = 1; attempt <= m_writeRetryCount; ++attempt)
    {
        auto *mimeData = new QMimeData();
        QList<QUrl> urls;
        urls.reserve(paths.size());
        for (const QString &path : paths)
        {
            urls.push_back(QUrl::fromLocalFile(path));
        }
        mimeData->setUrls(urls);

        clipboard->setMimeData(mimeData);

        const QMimeData *actual = clipboard->mimeData();
        if (actual && actual->hasUrls())
        {
            QStringList actualPaths;
            for (const QUrl &url : actual->urls())
            {
                if (url.isLocalFile())
                {
                    actualPaths.push_back(url.toLocalFile());
                }
            }

            if (!actualPaths.isEmpty() && hashFileList(actualPaths) == expectedHash)
            {
                m_recentInjectedFileHashes.insert(hashFileList(actualPaths), QDateTime::currentDateTimeUtc());
                m_recentInjectedSnapshotHashes.insert(makeFileListSnapshot(actualPaths).fingerprint, QDateTime::currentDateTimeUtc());
                if (attempt > 1)
                {
                    qInfo() << "clipboard file list write recovered after retries:" << attempt;
                }
                return true;
            }
        }

        if (attempt < m_writeRetryCount)
        {
            QThread::msleep(static_cast<unsigned long>(m_writeRetryDelayMs));
        }
    }

    qWarning() << "failed to set clipboard file list after retries";
    return false;
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
