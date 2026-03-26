#pragma once

#include <QHash>
#include <QObject>

#include "protocol/ProtocolHeader.h"

class QTcpServer;
class QTcpSocket;

class TransportServer : public QObject
{
    Q_OBJECT

public:
    explicit TransportServer(QObject *parent = nullptr);
    ~TransportServer() override;

    // 启动监听，接收对端入站连接。
    bool start(quint16 port);

signals:
    // 每解析出一条合法协议消息时发出。
    void messageReceived(const protocol::ClipboardMessage &message);

private:
    // 接收待处理连接并注册 socket 信号。
    void handleNewConnection();
    // 追加接收字节并拆解完整的长度前缀帧。
    void handleSocketReadyRead(QTcpSocket *socket);
    // 连接断开后清理该 socket 相关状态。
    void handleSocketDisconnected(QTcpSocket *socket);

    // 管理入站连接的 TCP 监听器。
    QTcpServer *m_server = nullptr;
    // 每个 socket 的接收缓冲，用于分片重组。
    QHash<QTcpSocket *, QByteArray> m_rxBuffers;
};
