#pragma once

#include <memory>

#include <QObject>

#include "common/Config.h"

class ClipboardMonitor;
class ClipboardWriter;
class SyncCoordinator;
class TransportClient;
class TransportServer;
class SyncDebugWindow;

class AppController : public QObject
{
    Q_OBJECT

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController();
    // 组装并连接所有运行时组件，然后启动网络端点。
    bool initialize();

private:
    // 从环境变量加载得到的运行配置。
    AppConfig m_config;
    // 可选的本地剪贴板监听器（可通过配置关闭）。
    std::unique_ptr<ClipboardMonitor> m_monitor;
    // 将远端剪贴板内容写入本地系统剪贴板。
    std::unique_ptr<ClipboardWriter> m_writer;
    // 接收入站的对端消息。
    std::unique_ptr<TransportServer> m_server;
    // 向对端发送出站消息。
    std::unique_ptr<TransportClient> m_client;
    // 在监听器、网络层与写入器之间做事件路由。
    std::unique_ptr<SyncCoordinator> m_coordinator;
    // 调试窗口：显示本地复制与远端接收内容。
    std::unique_ptr<SyncDebugWindow> m_debugWindow;
};
