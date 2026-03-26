#include "transport/TransportClient.h"

#include <QtEndian>

#include <QDebug>
#include <QTimer>
#include <QTcpSocket>

#include "protocol/MessageCodec.h"

TransportClient::TransportClient(QObject *parent)
    : QObject(parent), m_socket(new QTcpSocket(this)), m_reconnectTimer(new QTimer(this))
{
    m_reconnectTimer->setInterval(2000);
    QObject::connect(m_reconnectTimer, &QTimer::timeout, this, &TransportClient::tryConnect);

    QObject::connect(m_socket, &QTcpSocket::connected, this, [this]()
                     { qInfo() << "connected to peer:" << m_host << m_port; });
    QObject::connect(m_socket, &QTcpSocket::disconnected, this, [this]()
                     { qWarning() << "peer disconnected, waiting reconnect"; });
    QObject::connect(m_socket,
                     QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
                     this,
                     [this](QAbstractSocket::SocketError)
                     {
                         qWarning() << "socket error:" << m_socket->errorString();
                     });
}

void TransportClient::configurePeer(const QString &host, quint16 port)
{
    m_host = host;
    m_port = port;
}

void TransportClient::start()
{
    m_reconnectTimer->start();
    tryConnect();
}

bool TransportClient::sendMessage(const protocol::ClipboardMessage &message)
{
    if (m_socket->state() != QAbstractSocket::ConnectedState)
    {
        return false;
    }

    const QByteArray packet = MessageCodec::encode(message);
    QByteArray frame;
    frame.resize(4);
    qToLittleEndian<quint32>(static_cast<quint32>(packet.size()), reinterpret_cast<uchar *>(frame.data()));
    frame.append(packet);

    const qint64 written = m_socket->write(frame);
    return written == frame.size();
}

void TransportClient::tryConnect()
{
    if (m_host.isEmpty() || m_port == 0)
    {
        return;
    }

    if (m_socket->state() == QAbstractSocket::ConnectedState || m_socket->state() == QAbstractSocket::ConnectingState)
    {
        return;
    }

    m_socket->connectToHost(m_host, m_port);
}
