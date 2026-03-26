#include "clipboard/ClipboardMonitor.h"

#include <QGuiApplication>
#include <QClipboard>
#include <QTimer>

namespace
{
    QString canonicalClipboardText(QString text)
    {
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace(QChar('\r'), QChar('\n'));
        return text;
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
