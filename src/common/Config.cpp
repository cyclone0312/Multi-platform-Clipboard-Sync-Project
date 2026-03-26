#include "common/Config.h"

#include <QCoreApplication>
#include <QUuid>

namespace
{
    quint16 readPort(const char *key, quint16 fallback)
    {
        bool ok = false;
        const QString value = qEnvironmentVariable(key);
        if (value.isEmpty())
        {
            return fallback;
        }

        const int port = value.toInt(&ok);
        if (!ok || port <= 0 || port > 65535)
        {
            return fallback;
        }

        return static_cast<quint16>(port);
    }

    bool readBool(const char *key, bool fallback)
    {
        const QString value = qEnvironmentVariable(key).trimmed().toLower();
        if (value.isEmpty())
        {
            return fallback;
        }

        if (value == QStringLiteral("1") || value == QStringLiteral("true") || value == QStringLiteral("yes") || value == QStringLiteral("on"))
        {
            return true;
        }

        if (value == QStringLiteral("0") || value == QStringLiteral("false") || value == QStringLiteral("no") || value == QStringLiteral("off"))
        {
            return false;
        }

        return fallback;
    }
}

AppConfig AppConfig::fromEnvironment()
{
    AppConfig config;
    config.listenPort = readPort("CSYNC_LISTEN_PORT", config.listenPort);
    config.peerHost = qEnvironmentVariable("CSYNC_PEER_HOST", "127.0.0.1");
    config.peerPort = readPort("CSYNC_PEER_PORT", config.peerPort);
    config.nodeId = qEnvironmentVariable("CSYNC_NODE_ID");
    config.enableMonitor = readBool("CSYNC_ENABLE_MONITOR", config.enableMonitor);

    if (config.nodeId.isEmpty())
    {
        config.nodeId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    QCoreApplication::setApplicationName(QStringLiteral("clipboard_sync"));
    return config;
}
