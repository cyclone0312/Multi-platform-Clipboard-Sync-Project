#pragma once

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

private:
    // 在端点可用且 socket 空闲时发起连接。
    void tryConnect();

    // 从配置读取的对端主机地址。
    QString m_host;
    // 从配置读取的对端 TCP 端口。
    quint16 m_port = 0;
    // 在重连周期中复用的持久化 socket。
    QTcpSocket *m_socket = nullptr;
    // 周期性重连定时器。
    QTimer *m_reconnectTimer = nullptr;
};
