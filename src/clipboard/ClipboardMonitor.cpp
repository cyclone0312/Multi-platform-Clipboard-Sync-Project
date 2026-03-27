#include "clipboard/ClipboardMonitor.h"

#include <QGuiApplication>
#include <QClipboard>
#include <QDir>
#include <QMimeData>
#include <QTimer>
#include <QUrl>

//匿名命名空间
//内部链接性,防止冲突,替代 static
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
}

ClipboardMonitor::ClipboardMonitor(QObject *parent)
    : QObject(parent)
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    QObject::connect(clipboard, &QClipboard::dataChanged, this, &ClipboardMonitor::handleClipboardChanged);
}

void ClipboardMonitor::handleClipboardChanged()
{
    m_readRetryBudget = m_maxReadRetries;
    if (m_retryScheduled)
    {
        return;
    }

    tryEmitClipboardText();
}

void ClipboardMonitor::tryEmitClipboardText()
{
    m_retryScheduled = false;

    QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *mime = clipboard->mimeData();
    if (mime && mime->hasUrls())
    {
        const QStringList paths = canonicalLocalFilePaths(mime->urls());
        if (!paths.isEmpty())
        {
            m_readRetryBudget = 0;
            emit localFilesChanged(paths, hashFileList(paths));
            return;
        }
    }

    const QString text = clipboard->text();
    if (text.isNull() || text.isEmpty())
    {
        if (m_readRetryBudget <= 0)
        {
            return;
        }

        --m_readRetryBudget;
        m_retryScheduled = true;
        QTimer::singleShot(m_readRetryDelayMs, this, &ClipboardMonitor::tryEmitClipboardText);
        return;
    }

    m_readRetryBudget = 0;
    emit localTextChanged(text, qHash(canonicalClipboardText(text)));
}
