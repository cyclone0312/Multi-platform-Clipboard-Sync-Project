#include "sync/SyncCoordinator.h"

#include <QDebug>
#include <QRandomGenerator>

#include "clipboard/ClipboardMonitor.h"
#include "clipboard/ClipboardWriter.h"
#include "transport/TransportClient.h"
#include "transport/TransportServer.h"

SyncCoordinator::SyncCoordinator(ClipboardMonitor *monitor,
                                 ClipboardWriter *writer,
                                 TransportClient *client,
                                 QObject *parent)
    : QObject(parent), m_monitor(monitor), m_writer(writer), m_client(client)
{
    if (m_monitor)
    {
        QObject::connect(m_monitor,
                         &ClipboardMonitor::localTextChanged,
                         this,
                         &SyncCoordinator::handleLocalTextChanged);
    }
}

void SyncCoordinator::bindServer(TransportServer *server)
{
    QObject::connect(server,
                     &TransportServer::messageReceived,
                     this,
                     &SyncCoordinator::handleRemoteMessage);
}

bool SyncCoordinator::manualInjectAndSend(const QString &text)
{
    if (text.isEmpty())
    {
        return false;
    }

    const quint64 sessionId = QRandomGenerator::global()->generate64();
    if (!m_writer->writeRemoteText(text, sessionId))
    {
        qWarning() << "manual inject failed to write local clipboard";
        return false;
    }

    emit localTextForwarded(text);
    return sendTextToPeer(text, sessionId);
}

bool SyncCoordinator::sendTextToPeer(const QString &text, quint64 sessionId)
{
    protocol::ClipboardMessage message;
    message.type = protocol::MessageType::TextPlain;
    message.flags = 0;
    message.sessionId = sessionId;
    message.sequence = 1;
    message.payload = text.toUtf8();

    qInfo() << "sending text bytes:" << message.payload.size() << "session:" << message.sessionId;

    if (!m_client->sendMessage(message))
    {
        qWarning() << "send text failed (peer may be offline)";
        return false;
    }

    return true;
}

void SyncCoordinator::handleLocalTextChanged(const QString &text, quint32 textHash)
{
    if (m_writer->isRecentlyInjected(textHash))
    {
        qInfo() << "skip local echo text hash:" << textHash;
        return;
    }

    emit localTextForwarded(text);

    const quint64 sessionId = QRandomGenerator::global()->generate64();
    if (!sendTextToPeer(text, sessionId))
    {
        qWarning() << "local clipboard forward failed";
    }
}

void SyncCoordinator::handleRemoteMessage(const protocol::ClipboardMessage &message)
{
    if (message.type != protocol::MessageType::TextPlain)
    {
        return;
    }

    const QString text = QString::fromUtf8(message.payload);
    if (text.isEmpty())
    {
        return;
    }

    emit remoteTextReceived(text);

    qInfo() << "remote message received, bytes:" << message.payload.size() << "session:" << message.sessionId;

    if (m_writer->writeRemoteText(text, message.sessionId))
    {
        qInfo() << "remote clipboard applied";
    }
    else
    {
        qWarning() << "remote clipboard apply failed";
    }
}
