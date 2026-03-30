#include "clipboard/ClipboardWriter.h"

#include <QClipboard>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QPixmap>
#include <QThread>
#include <QUrl>

namespace
{
    QString canonicalClipboardText(QString text)
    {
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace(QChar('\r'), QChar('\n'));
        return text;
    }

    quint32 hashFileList(const QStringList &paths)
    {
        QStringList normalized = paths;
        for (QString &path : normalized)
        {
            path = QDir::cleanPath(path).toLower();
        }
        normalized.sort();
        return qHash(normalized.join(QStringLiteral("\n")));
    }

    quint32 hashImageContent(const QImage &image)
    {
        if (image.isNull())
        {
            return 0;
        }

        const QImage normalized = image.convertToFormat(QImage::Format_RGBA8888);
        QCryptographicHash hash(QCryptographicHash::Sha256);

        const QByteArray meta = QByteArray::number(normalized.width()) + 'x' + QByteArray::number(normalized.height()) + ':' + QByteArray::number(static_cast<int>(normalized.format()));
        hash.addData(meta);
        hash.addData(reinterpret_cast<const char *>(normalized.constBits()), static_cast<int>(normalized.sizeInBytes()));
        return qHash(hash.result());
    }

    quint32 hashImagePngBytes(const QByteArray &pngBytes)
    {
        if (pngBytes.isEmpty())
        {
            return 0;
        }

        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(pngBytes);
        return qHash(hash.result());
    }

    bool extractImage(const QMimeData *mime, QImage *outImage)
    {
        if (!mime || !outImage || !mime->hasImage())
        {
            return false;
        }

        const QVariant imageData = mime->imageData();
        if (imageData.canConvert<QImage>())
        {
            const QImage image = qvariant_cast<QImage>(imageData);
            if (!image.isNull())
            {
                *outImage = image;
                return true;
            }
        }

        if (imageData.canConvert<QPixmap>())
        {
            const QPixmap pixmap = qvariant_cast<QPixmap>(imageData);
            if (!pixmap.isNull())
            {
                *outImage = pixmap.toImage();
                return !outImage->isNull();
            }
        }

        const QByteArray pngData = mime->data(QStringLiteral("image/png"));
        if (!pngData.isEmpty())
        {
            QImage image;
            if (image.loadFromData(pngData, "PNG"))
            {
                *outImage = image;
                return true;
            }
        }

        return false;
    }
}

ClipboardWriter::ClipboardWriter(QObject *parent)
    : QObject(parent)
{
}

bool ClipboardWriter::writeRemoteText(const QString &text, quint64 sessionId)
{
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
        return true;
    }

    for (int attempt = 1; attempt <= m_writeRetryCount; ++attempt)
    {
        clipboard->setText(text);

        const QString actualText = clipboard->text();
        if (canonicalClipboardText(actualText) == canonicalIncoming)
        {
            m_recentInjectedHashes.insert(qHash(canonicalClipboardText(actualText)), QDateTime::currentDateTimeUtc());
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

    QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *current = clipboard->mimeData();
    if (current)
    {
        QImage currentImage;
        if (extractImage(current, &currentImage) && hashImageContent(currentImage) == incomingHash)
        {
            markImageInjected(incomingHash, incomingPngHash);
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
