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
#include <QVector>

namespace
{
    const QString kLoopGuardMimeType = QStringLiteral("application/x-clipboard-sync-loop-guard");

    struct RecentHashBackup
    {
        quint32 hash = 0;
        bool existed = false;
        QDateTime previousTime;
    };

    // 把各种不同操作系统里乱七八糟的“回车换行符”，全部强行统一成一种标准格式
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
        // 通过 SHA-256 算法提取图片的绝对特征，并将其转化为一个极高查询效率的整型 ID，
        // 用于后续判断这张图片是否已经被处理
        hash.addData(pngBytes);
        // hash.result() 会得到一份 SHA-256 结果，也就是 32 字节摘要,再qhash压缩出来的 32 位整型指纹
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

    // 为了提前登记 hash 来挡住回环，但又不想在写失败时把状态污染掉
    QByteArray makeLoopGuardPayload(const quint64 sessionId, const char *contentType)
    {
        return QByteArray("session=") + QByteArray::number(sessionId) + ";type=" + contentType;
    }

    void applyLoopGuardTag(QMimeData *mimeData, const quint64 sessionId, const char *contentType)
    {
        if (!mimeData)
        {
            return;
        }

        mimeData->setData(kLoopGuardMimeType, makeLoopGuardPayload(sessionId, contentType));
    }

    void stageRecentHash(QHash<quint32, QDateTime> &hashMap,
                         quint32 hash,
                         const QDateTime &timestamp,
                         QVector<RecentHashBackup> *backups)
    {
        // 只跟踪有效 hash；backups 用来在后续写剪贴板失败时恢复现场。
        if (!backups || hash == 0)
        {
            return;
        }

        bool alreadyTracked = false;
        // 同一次写入流程里，同一个 hash 只备份一次，避免回滚信息重复。
        for (const RecentHashBackup &backup : *backups)
        {
            if (backup.hash == hash)
            {
                alreadyTracked = true;
                break;
            }
        }

        // 在改 hashMap 之前，先把这个 hash 原来在表里的状态记下来，方便后面失败时恢复。”
        if (!alreadyTracked)
        {
            RecentHashBackup backup;
            backup.hash = hash;
            // 记录“改写前”这个 hash 在表里的状态：原本是否存在、旧时间戳是多少。
            const auto existing = hashMap.constFind(hash);
            backup.existed = existing != hashMap.cend();
            if (backup.existed)
            {
                backup.previousTime = existing.value();
            }
            backups->push_back(backup);
        }

        // 先把本次远端注入的时间写进去，让随后的本地剪贴板变化能立即识别并跳过回环。
        hashMap.insert(hash, timestamp);
    }

    void rollbackRecentHashes(QHash<quint32, QDateTime> &hashMap,
                              const QVector<RecentHashBackup> &backups)
    {
        for (auto it = backups.crbegin(); it != backups.crend(); ++it)
        {
            if (it->existed)
            {
                hashMap.insert(it->hash, it->previousTime);
            }
            else
            {
                hashMap.remove(it->hash);
            }
        }
    }
}

ClipboardWriter::ClipboardWriter(QObject *parent)
    : QObject(parent)
{
}

bool ClipboardWriter::writeRemoteText(const QString &text, quint64 sessionId)
{
    // Q_UNUSED()，它的作用是告诉编译器这个参数是有意未使用的，从而避免编译器发出未使用参数的警告。
    // 清理过期注入记录
    cleanupExpired();
    // 把文本标准化（比如统一换行符），然后算出一段独一无二的 Hash 值（相当于这段文本的“身份证号”）
    const QString canonicalIncoming = canonicalClipboardText(text);
    const quint32 hash = qHash(canonicalIncoming);
    const QDateTime injectionTime = QDateTime::currentDateTimeUtc();
    QVector<RecentHashBackup> stagedHashes;
    stageRecentHash(m_recentInjectedHashes, hash, injectionTime, &stagedHashes);

    // 拿到 Qt 的全局剪贴板接口，后续读写都通过它
    QClipboard *clipboard = QGuiApplication::clipboard();
    // 如果目标文本本来就已经在剪贴板里，不必重复写
    // 但仍要记录“最近注入哈希”，这样监听器触发时可识别并抑制回环
    // 然后返回成功
    if (canonicalClipboardText(clipboard->text()) == canonicalIncoming)
    {
        return true;
    }

    // 剪贴板写入重试机制：有时系统剪贴板可能被其他应用暂时锁定，导致写入失败。我们通过多次尝试写入，并在每次失败后稍作等待，来提高成功率。
    for (int attempt = 1; attempt <= m_writeRetryCount; ++attempt)
    {
        auto *mimeData = new QMimeData();
        mimeData->setText(text);
        applyLoopGuardTag(mimeData, sessionId, "text");
        clipboard->setMimeData(mimeData);

        const QMimeData *actual = clipboard->mimeData();
        const QString actualText = actual ? actual->text() : QString();
        if (canonicalClipboardText(actualText) == canonicalIncoming)
        {
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

    rollbackRecentHashes(m_recentInjectedHashes, stagedHashes);
    qWarning() << "failed to set clipboard text after retries";
    return false;
}

bool ClipboardWriter::writeRemoteImage(const QByteArray &pngBytes, quint64 sessionId)
{
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
    // 计算图片内容哈希和 PNG 数据哈希，至少要有一个成功，才能继续后续的回环检测和写入验证
    const quint32 incomingHash = hashImageContent(image);
    const quint32 incomingPngHash = hashImagePngBytes(pngBytes);
    if (incomingHash == 0 && incomingPngHash == 0)
    {
        return false;
    }

    const QDateTime injectionTime = QDateTime::currentDateTimeUtc();
    QVector<RecentHashBackup> stagedHashes;
    stageRecentHash(m_recentInjectedImageHashes, incomingHash, injectionTime, &stagedHashes);
    stageRecentHash(m_recentInjectedImageHashes, incomingPngHash, injectionTime, &stagedHashes);

    QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *current = clipboard->mimeData();
    if (current)
    {
        QImage currentImage;
        // 写之前先比一下，看看本地剪贴板是不是已经是这张图了。如果是的话，就不必重复写了，直接记录指纹并返回成功。
        if (extractImage(current, &currentImage) && hashImageContent(currentImage) == incomingHash)
        {
            // 如果本地剪贴板已经是目标图片，就跳过实际写入，但仍把它登记为最近注入内容，用来防回环。
            return true;
        }
    }

    for (int attempt = 1; attempt <= m_writeRetryCount; ++attempt)
    {
        auto *mimeData = new QMimeData();
        mimeData->setData(QStringLiteral("image/png"), pngBytes);
        mimeData->setImageData(image);
        applyLoopGuardTag(mimeData, sessionId, "image");
        clipboard->setMimeData(mimeData);

        // 写进去以后，立刻再读出来核对是否真的写成功
        const QMimeData *actual = clipboard->mimeData();
        QImage actualImage;
        if (extractImage(actual, &actualImage) && hashImageContent(actualImage) == incomingHash)
        {
            quint32 actualPngHash = 0;
            if (actual)
            {
                actualPngHash = hashImagePngBytes(actual->data(QStringLiteral("image/png")));
            }
            stageRecentHash(m_recentInjectedImageHashes, actualPngHash, injectionTime, &stagedHashes);
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

    rollbackRecentHashes(m_recentInjectedImageHashes, stagedHashes);
    qWarning() << "failed to set clipboard image after retries";
    return false;
}

// 将远端文件列表写入本地剪贴板，并记录防回环指纹。
bool ClipboardWriter::writeRemoteFileList(const QStringList &paths, quint64 sessionId)
{
    cleanupExpired();
    if (paths.isEmpty())
    {
        return false;
    }

    QClipboard *clipboard = QGuiApplication::clipboard();
    const quint32 expectedHash = hashFileList(paths);
    const QDateTime injectionTime = QDateTime::currentDateTimeUtc();
    QVector<RecentHashBackup> stagedHashes;
    stageRecentHash(m_recentInjectedFileHashes, expectedHash, injectionTime, &stagedHashes);
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
        applyLoopGuardTag(mimeData, sessionId, "files");

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

    rollbackRecentHashes(m_recentInjectedFileHashes, stagedHashes);
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
