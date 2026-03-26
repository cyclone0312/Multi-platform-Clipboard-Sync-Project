#include "clipboard/ClipboardWriter.h"

#include <QClipboard>
#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QMimeData>
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

bool ClipboardWriter::writeRemoteFileList(const QStringList &paths, quint64 sessionId)
{
    Q_UNUSED(sessionId)
    cleanupExpired();
    if (paths.isEmpty())
    {
        return false;
    }

    QClipboard *clipboard = QGuiApplication::clipboard();
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
    if (!actual || !actual->hasUrls())
    {
        qWarning() << "failed to set clipboard file list";
        return false;
    }

    QStringList actualPaths;
    for (const QUrl &url : actual->urls())
    {
        if (url.isLocalFile())
        {
            actualPaths.push_back(url.toLocalFile());
        }
    }

    const quint32 hash = hashFileList(actualPaths);
    m_recentInjectedFileHashes.insert(hash, QDateTime::currentDateTimeUtc());
    return true;
}

bool ClipboardWriter::isRecentlyInjected(quint32 textHash) const
{
    cleanupExpired();
    return m_recentInjectedHashes.contains(textHash);
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
}
