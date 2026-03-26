#include "clipboard/ClipboardWriter.h"

#include <QClipboard>
#include <QDebug>
#include <QGuiApplication>
#include <QThread>

namespace
{
    QString canonicalClipboardText(QString text)
    {
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace(QChar('\r'), QChar('\n'));
        return text;
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

bool ClipboardWriter::isRecentlyInjected(quint32 textHash) const
{
    cleanupExpired();
    return m_recentInjectedHashes.contains(textHash);
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
}
