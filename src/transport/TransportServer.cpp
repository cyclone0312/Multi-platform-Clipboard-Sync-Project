#include "transport/TransportServer.h"

#include <QtEndian>

#include <QDebug>
#include <QTcpServer>
#include <QTcpSocket>

#include "protocol/MessageCodec.h"

TransportServer::TransportServer(QObject *parent)
    : QObject(parent), m_server(new QTcpServer(this))
{
    QObject::connect(m_server, &QTcpServer::newConnection, this, &TransportServer::handleNewConnection);
}

TransportServer::~TransportServer() = default;

bool TransportServer::start(quint16 port)
{
    const bool ok = m_server->listen(QHostAddress::AnyIPv4, port);
    if (!ok)
    {
        qWarning() << "listen failed:" << m_server->errorString();
    }
    return ok;
}

void TransportServer::handleNewConnection()
{
    while (m_server->hasPendingConnections())
    {
        QTcpSocket *socket = m_server->nextPendingConnection();
        m_rxBuffers.insert(socket, QByteArray());

        QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket]()
                         { handleSocketReadyRead(socket); });
        QObject::connect(socket, &QTcpSocket::disconnected, this, [this, socket]()
                         { handleSocketDisconnected(socket); });

        qInfo() << "peer connected:" << socket->peerAddress().toString() << socket->peerPort();
    }
}

void TransportServer::handleSocketReadyRead(QTcpSocket *socket)
{
    if (!m_rxBuffers.contains(socket))
    {
        return;
    }

    QByteArray &buffer = m_rxBuffers[socket];
    buffer.append(socket->readAll());

    while (true)
    {
        if (buffer.size() < 4)
        {
            return;
        }

        const quint32 frameSize = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(buffer.constData()));
        if (buffer.size() < static_cast<int>(4 + frameSize))
        {
            return;
        }

        const QByteArray packet = buffer.mid(4, static_cast<int>(frameSize));
        buffer.remove(0, static_cast<int>(4 + frameSize));

        protocol::ClipboardMessage message;
        QString error;
        if (!MessageCodec::decode(packet, &message, &error))
        {
            qWarning().noquote() << "drop invalid packet:" << error;
            continue;
        }

        emit messageReceived(message);
    }
}

void TransportServer::handleSocketDisconnected(QTcpSocket *socket)
{
    m_rxBuffers.remove(socket);
    socket->deleteLater();
}
