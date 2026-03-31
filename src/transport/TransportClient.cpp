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
                     {
                         qInfo() << "connected to peer:" << m_host << m_port;
                         emit peerConnected();
                         flushTxBuffer(); });
    QObject::connect(m_socket, &QTcpSocket::disconnected, this, [this]()
                     {
                         qWarning() << "peer disconnected, waiting reconnect";
                         m_txBuffer.clear();
                         emit peerDisconnected(); });
    QObject::connect(m_socket, &QTcpSocket::bytesWritten, this, [this](qint64)
                     { flushTxBuffer(); });
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
    // 封装一帧可发送的TCP数据
    QByteArray frame;
    frame.resize(4); // 先预留前4字节，用来写“后面包体有多长”
    // 把 packet.size() 这个长度值，按小端序写进预留的这4字节里。 也就是帧头长度字段。
    qToLittleEndian<quint32>(static_cast<quint32>(packet.size()), reinterpret_cast<uchar *>(frame.data()));
    // 把真正的业务包（MessageCodec编码后的协议包）拼在后面。
    frame.append(packet);
    // 判断“当前待发送缓存 + 新帧”是否超过上限
    if (m_txBuffer.size() + frame.size() > m_maxBufferedBytes)
    {
        qWarning() << "send buffer overflow, drop frame bytes:" << frame.size();
        return false;
    }
    // 没超限就把新帧放入发送缓冲区
    m_txBuffer.append(frame);
    // 触发一次尽可能发送（调用 socket->write）。
    flushTxBuffer();
    return true;
}

bool TransportClient::isConnected() const
{
    return m_socket->state() == QAbstractSocket::ConnectedState;
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

void TransportClient::flushTxBuffer()
{
    if (m_socket->state() != QAbstractSocket::ConnectedState)
    {
        return;
    }

    while (!m_txBuffer.isEmpty())
    {
        // 把当前缓冲区交给 Qt socket。 注意 write 不保证一次全写完，可能只接收一部分（也可能 0 或负数）
        const qint64 written = m_socket->write(m_txBuffer);
        if (written <= 0)
        {
            break;
        }
        // 把已经成功交给 socket 的前 written 个字节从发送队列里删掉
        m_txBuffer.remove(0, static_cast<int>(written));
        if (m_socket->bytesToWrite() > (4 * 1024 * 1024))
        {
            break;
        }
    }
}
