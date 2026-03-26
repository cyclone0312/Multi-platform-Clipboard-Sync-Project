#pragma once

#include <QString>

struct AppConfig
{
    // 本地服务端监听端口。
    quint16 listenPort = 45454;
    // 出站客户端连接使用的对端主机。
    QString peerHost = QStringLiteral("127.0.0.1");
    // 出站客户端连接使用的对端端口。
    quint16 peerPort = 45455;
    // 逻辑节点标识，用于诊断与后续路由扩展。
    QString nodeId;
    // 为 true 时启用本地剪贴板监听。
    bool enableMonitor = true;

    // 从 CSYNC_* 环境变量构建配置。
    static AppConfig fromEnvironment();
};
