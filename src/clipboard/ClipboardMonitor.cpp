#include "clipboard/ClipboardMonitor.h"

#include <QGuiApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QImage>
#include <QMimeData>
#include <QPixmap>
#include <QTimer>
#include <QUrl>

// 匿名命名空间
// 内部链接性,防止冲突,替代 static
namespace
{
    QString canonicalClipboardText(QString text)
    {
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace(QChar('\r'), QChar('\n'));
        return text;
    }

    QStringList canonicalLocalFilePaths(const QList<QUrl> &urls)
    {
        QStringList paths;
        paths.reserve(urls.size());
        for (const QUrl &url : urls)
        {
            if (!url.isLocalFile())
            {
                continue;
            }

            const QString local = QDir::cleanPath(url.toLocalFile());
            if (!local.isEmpty())
            {
                paths.push_back(local);
            }
        }

        return paths;
    }

    quint32 hashFileList(const QStringList &paths)
    {
        QStringList normalized = paths;
        for (QString &path : normalized)
        {
            path = path.toLower();
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

    quint32 hashImageFingerprint(const QMimeData *mime, const QImage &image, const QByteArray &encodedPng)
    {
        if (mime)
        {
            const QByteArray mimePng = mime->data(QStringLiteral("image/png"));
            const quint32 mimePngHash = hashImagePngBytes(mimePng);
            if (mimePngHash != 0)
            {
                return mimePngHash;
            }
        }

        const quint32 encodedHash = hashImagePngBytes(encodedPng);
        if (encodedHash != 0)
        {
            return encodedHash;
        }

        return hashImageContent(image);
    }

    bool encodeImageAsPng(const QImage &image, QByteArray *outPng)
    {
        if (!outPng)
        {
            return false;
        }

        outPng->clear();
        if (image.isNull())
        {
            return false;
        }

        QBuffer buffer(outPng);
        if (!buffer.open(QIODevice::WriteOnly))
        {
            return false;
        }

        return image.save(&buffer, "PNG");
    }

    bool extractImage(const QMimeData *mime, QImage *outImage)
    {
        if (!mime || !outImage || !mime->hasImage())
        {
            return false;
        }

        const QVariant imageData = mime->imageData();
        // 尝试直接提取 QImage
        if (imageData.canConvert<QImage>())
        {
            const QImage image = qvariant_cast<QImage>(imageData);
            if (!image.isNull())
            {
                *outImage = image;
                return true;
            }
        }
        // 尝试转换 QPixmap
        if (imageData.canConvert<QPixmap>())
        {
            const QPixmap pixmap = qvariant_cast<QPixmap>(imageData);
            if (!pixmap.isNull())
            {
                *outImage = pixmap.toImage();
                return !outImage->isNull();
            }
        }
        // 尝试解码原始 PNG 二进制数据
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

ClipboardMonitor::ClipboardMonitor(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<clipboard::Snapshot>("clipboard::Snapshot");
    QClipboard *systemClipboard = QGuiApplication::clipboard();
    QObject::connect(systemClipboard, &QClipboard::dataChanged, this, &ClipboardMonitor::handleClipboardChanged);
}

void ClipboardMonitor::handleClipboardChanged()
{
    m_readRetryBudget = m_maxReadRetries;
    if (m_retryScheduled)
    {
        return;
    }

    tryEmitClipboardSnapshot();
}

void ClipboardMonitor::tryEmitClipboardSnapshot()
{
    m_retryScheduled = false;

    const auto scheduleRetry = [this]()
    {
        if (m_readRetryBudget <= 0)
        {
            return false;
        }

        --m_readRetryBudget;
        m_retryScheduled = true;
        QTimer::singleShot(m_readRetryDelayMs, this, &ClipboardMonitor::tryEmitClipboardSnapshot);
        return true;
    };

    QClipboard *systemClipboard = QGuiApplication::clipboard();
    const QMimeData *mime = systemClipboard->mimeData();

    if (false && mime && mime->hasImage())
    {
        QImage image;
        // 将mime转换为Qimage类
        if (extractImage(mime, &image))
        {
            QByteArray pngBytes;
            // 将Qimage转换为png格式传输
            if (encodeImageAsPng(image, &pngBytes) && !pngBytes.isEmpty())
            {
                m_readRetryBudget = 0;
                emit localImageChanged(pngBytes, hashImageFingerprint(mime, image, pngBytes));
                return;
            }
        }

        if (!scheduleRetry())
        {
            return;
        }
        return;
    }

    const QString text = systemClipboard->text();
    const bool mayHaveClipboardPayload =
        (mime && (mime->hasUrls() || mime->hasImage() || mime->hasHtml() || mime->hasText())) || !text.isEmpty();

    const ::clipboard::Snapshot snapshot = ::clipboard::captureSnapshotFromMime(mime, text);
    // 某些桌面环境下剪贴板数据会延迟可读；
    // 这里先尝试拿完整 snapshot，拿不到再按原有策略短暂重试。
    if (snapshot.isEmpty())
    {
        if (mayHaveClipboardPayload)
        {
            scheduleRetry();
        }
        return;
    }

    m_readRetryBudget = 0;
    // snapshot 是新的主信号；
    // 下面保留旧信号只是为了兼容现有 UI/调试输出。
    emit localSnapshotChanged(snapshot);

    if (snapshot.hasLocalFiles())
    {
        emit localFilesChanged(snapshot.localFilePaths, snapshot.localFilesHash);
    }

    if (snapshot.hasImage())
    {
        emit localImageChanged(snapshot.imagePng, snapshot.imageHash);
    }

    if (snapshot.hasText())
    {
        emit localTextChanged(snapshot.text, snapshot.textHash);
    }
}
