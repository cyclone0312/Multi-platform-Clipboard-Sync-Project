#include "ui/SyncDebugWindow.h"

#include <QDateTime>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

SyncDebugWindow::SyncDebugWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle(QStringLiteral("Clipboard Sync Monitor"));
    resize(860, 520);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto *tip = new QLabel(QStringLiteral("上方显示本地复制并外发的内容，下方显示收到的远端内容。"), this);
    root->addWidget(tip);

    auto *manualGroup = new QGroupBox(QStringLiteral("手动添加（写本地并发送对端）"), this);
    auto *manualLayout = new QVBoxLayout(manualGroup);
    m_manualInput = new QPlainTextEdit(manualGroup);
    m_manualInput->setPlaceholderText(QStringLiteral("输入要写入剪贴板并发送给对端的文本"));
    m_manualInput->setMaximumHeight(90);
    m_manualSendButton = new QPushButton(QStringLiteral("写入本地并发送"), manualGroup);
    m_requestRemoteFilesButton = new QPushButton(QStringLiteral("模拟粘贴触发远端文件拉取"), manualGroup);
    manualLayout->addWidget(m_manualInput);
    manualLayout->addWidget(m_manualSendButton);
    manualLayout->addWidget(m_requestRemoteFilesButton);
    QObject::connect(m_manualSendButton, &QPushButton::clicked, this, &SyncDebugWindow::onManualSendClicked);
    QObject::connect(m_requestRemoteFilesButton, &QPushButton::clicked, this, &SyncDebugWindow::onRequestRemoteFilesClicked);
    root->addWidget(manualGroup);

    auto *localGroup = new QGroupBox(QStringLiteral("本地复制（发送侧）"), this);
    auto *localLayout = new QVBoxLayout(localGroup);
    m_localView = new QPlainTextEdit(localGroup);
    m_localView->setReadOnly(true);
    m_localView->setMaximumBlockCount(m_maxBlocks);
    localLayout->addWidget(m_localView);

    auto *remoteGroup = new QGroupBox(QStringLiteral("远端接收（接收侧）"), this);
    auto *remoteLayout = new QVBoxLayout(remoteGroup);
    m_remoteView = new QPlainTextEdit(remoteGroup);
    m_remoteView->setReadOnly(true);
    m_remoteView->setMaximumBlockCount(m_maxBlocks);
    remoteLayout->addWidget(m_remoteView);

    root->addWidget(localGroup, 1);
    root->addWidget(remoteGroup, 1);
}

void SyncDebugWindow::appendLocalText(const QString &text)
{
    appendEntry(m_localView, text);
}

void SyncDebugWindow::appendRemoteText(const QString &text)
{
    appendEntry(m_remoteView, text);
}

void SyncDebugWindow::appendFileTransferStatus(const QString &status)
{
    appendEntry(m_remoteView, QStringLiteral("[File] %1").arg(status));
}

void SyncDebugWindow::onManualSendClicked()
{
    if (!m_manualInput)
    {
        return;
    }

    const QString text = m_manualInput->toPlainText();
    if (text.trimmed().isEmpty())
    {
        return;
    }

    emit manualInjectRequested(text);
}

void SyncDebugWindow::onRequestRemoteFilesClicked()
{
    emit requestRemoteFilesTriggered();
}

void SyncDebugWindow::appendEntry(QPlainTextEdit *target, const QString &text)
{
    if (!target)
    {
        return;
    }

    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    target->appendPlainText(QStringLiteral("[%1] %2").arg(timestamp, text));
}
