#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include "protocol/ProtocolHeader.h"

class QTcpSocket;
class QTimer;

class TransportClient : public QObject
{
    Q_OBJECT

public:
    explicit TransportClient(QObject *parent = nullptr);

    // 配置出站连接的对端地址与端口。
    void configurePeer(const QString &host, quint16 port);
    // 启动重连循环并立即尝试一次连接。
    void start();
    // 编码消息并按帧格式发送到对端。
    bool sendMessage(const protocol::ClipboardMessage &message);
    // 当前连接是否可用。
    bool isConnected() const;

signals:
    // 与对端建立连接时发出。
    void peerConnected();
    // 与对端断开连接时发出。
    void peerDisconnected();

private:
    // 在端点可用且 socket 空闲时发起连接。
    void tryConnect();
    // 尝试把发送缓冲中的数据写入 socket。
    void flushTxBuffer();

    // 从配置读取的对端主机地址。
    QString m_host;
    // 从配置读取的对端 TCP 端口。
    quint16 m_port = 0;
    // 在重连周期中复用的持久化 socket。
    QTcpSocket *m_socket = nullptr;
    // 周期性重连定时器。
    QTimer *m_reconnectTimer = nullptr;
    // 待发送字节缓冲，处理部分写场景。
    QByteArray m_txBuffer;
    // 发送缓冲上限（字节），防止内存无限增长。
    qint64 m_maxBufferedBytes = 64 * 1024 * 1024;
};
