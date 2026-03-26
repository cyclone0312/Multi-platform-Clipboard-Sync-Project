#pragma once

#include <QObject>

#include "protocol/ProtocolHeader.h"

class ClipboardMonitor;
class ClipboardWriter;
class TransportClient;
class TransportServer;

class SyncCoordinator : public QObject
{
    Q_OBJECT

public:
    explicit SyncCoordinator(ClipboardMonitor *monitor,
                             ClipboardWriter *writer,
                             TransportClient *client,
                             QObject *parent = nullptr);

    // 绑定服务端入站消息到协调器处理逻辑。
    void bindServer(TransportServer *server);
    // 手动写入本地剪贴板并发送到对端。
    bool manualInjectAndSend(const QString &text);

signals:
    // 本地文本通过防回环校验后，准备外发时发出。
    void localTextForwarded(const QString &text);
    // 收到远端文本消息时发出。
    void remoteTextReceived(const QString &text);

private:
    // 按统一协议编码并发送文本到对端。
    bool sendTextToPeer(const QString &text, quint64 sessionId);
    // 处理本地剪贴板更新，并在满足条件时转发到对端。
    void handleLocalTextChanged(const QString &text, quint32 textHash);
    // 处理远端消息并写入本地剪贴板。
    void handleRemoteMessage(const protocol::ClipboardMessage &message);

    // 可选依赖：监听器可通过配置关闭。
    ClipboardMonitor *m_monitor = nullptr;
    // 负责远端内容写入与防回环判定。
    ClipboardWriter *m_writer = nullptr;
    // 本地到远端转发所使用的发送通道。
    TransportClient *m_client = nullptr;
};
