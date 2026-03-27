#include "app/AppController.h"

#include <QDebug>

#include "clipboard/ClipboardMonitor.h"
#include "clipboard/ClipboardWriter.h"
#include "common/Logger.h"
#include "input/PasteTriggerHook.h"
#include "sync/SyncCoordinator.h"
#include "transport/TransportClient.h"
#include "transport/TransportServer.h"
#include "ui/SyncDebugWindow.h"

AppController::AppController(QObject *parent)
    : QObject(parent)
{
}

AppController::~AppController() = default;

bool AppController::initialize()
{
    setupLogging();
    m_config = AppConfig::fromEnvironment();

    qInfo().noquote() << "Node:" << m_config.nodeId
                      << "listen:" << m_config.listenPort
                      << "peer:" << (m_config.peerHost + ":" + QString::number(m_config.peerPort))
                      << "monitor:" << (m_config.enableMonitor ? "on" : "off");

    if (m_config.enableMonitor)
    {
        m_monitor = std::make_unique<ClipboardMonitor>();
    }
    m_writer = std::make_unique<ClipboardWriter>();
    m_server = std::make_unique<TransportServer>();
    m_client = std::make_unique<TransportClient>();
    m_coordinator = std::make_unique<SyncCoordinator>(m_monitor.get(), m_writer.get(), m_client.get(), this);
    m_pasteHook = std::make_unique<PasteTriggerHook>(this);
    m_debugWindow = std::make_unique<SyncDebugWindow>();

    QObject::connect(m_coordinator.get(),
                     &SyncCoordinator::localTextForwarded,
                     m_debugWindow.get(),
                     &SyncDebugWindow::appendLocalText);
    QObject::connect(m_coordinator.get(),
                     &SyncCoordinator::remoteTextReceived,
                     m_debugWindow.get(),
                     &SyncDebugWindow::appendRemoteText);
    QObject::connect(m_coordinator.get(),
                     &SyncCoordinator::localImageForwarded,
                     m_debugWindow.get(),
                     [this](qint64 bytes)
                     {
                         m_debugWindow->appendLocalText(QStringLiteral("【图片】已检测到并准备发送，PNG=%1 bytes").arg(bytes));
                     });
    QObject::connect(m_coordinator.get(),
                     &SyncCoordinator::remoteImageReceived,
                     m_debugWindow.get(),
                     [this](qint64 bytes)
                     {
                         m_debugWindow->appendRemoteText(QStringLiteral("【图片】已写入剪贴板，PNG=%1 bytes").arg(bytes));
                     });
    QObject::connect(m_coordinator.get(),
                     &SyncCoordinator::localFilesForwarded,
                     m_debugWindow.get(),
                     [this](const QStringList &paths)
                     {
                         m_debugWindow->appendLocalText(QStringLiteral("[FileOffer] %1").arg(paths.join(QStringLiteral(" | "))));
                     });
    QObject::connect(m_coordinator.get(),
                     &SyncCoordinator::remoteFileOfferReceived,
                     m_debugWindow.get(),
                     [this](const QStringList &names)
                     {
                         m_debugWindow->appendRemoteText(QStringLiteral("[FileOffer] %1").arg(names.join(QStringLiteral(" | "))));
                     });
    QObject::connect(m_coordinator.get(),
                     &SyncCoordinator::fileTransferStatus,
                     m_debugWindow.get(),
                     &SyncDebugWindow::appendFileTransferStatus);
    // 调试窗口“手动写入并发送”按钮 -> 协调器文本注入入口。
    QObject::connect(m_debugWindow.get(),
                     &SyncDebugWindow::manualInjectRequested,
                     m_coordinator.get(),
                     &SyncCoordinator::manualInjectAndSend);
    // 调试窗口“请求远端文件”按钮 -> 复用 Ctrl+Shift+V 的请求入口。
    QObject::connect(m_debugWindow.get(),
                     &SyncDebugWindow::requestRemoteFilesTriggered,
                     m_coordinator.get(),
                     &SyncCoordinator::requestPendingRemoteFilesOnCtrlShiftV);
    // 粘贴钩子监听到 Ctrl+Shift+V -> 拉取最近一次远端文件 Offer。
    QObject::connect(m_pasteHook.get(),
                     &PasteTriggerHook::ctrlShiftPasteTriggered,
                     m_coordinator.get(),
                     &SyncCoordinator::requestPendingRemoteFilesOnCtrlShiftV);

    m_debugWindow->show();

    m_coordinator->bindServer(m_server.get());

    if (!m_server->start(m_config.listenPort))
    {
        qWarning() << "Failed to start server on port" << m_config.listenPort;
        return false;
    }

    m_client->configurePeer(m_config.peerHost, m_config.peerPort);
    m_client->start();

    if (!m_pasteHook->start())
    {
        qWarning() << "Failed to start paste trigger hook";
    }

    return true;
}
