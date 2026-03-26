#pragma once

#include <QWidget>

class QPlainTextEdit;
class QPushButton;

class SyncDebugWindow : public QWidget
{
    Q_OBJECT

public:
    explicit SyncDebugWindow(QWidget *parent = nullptr);

signals:
    // 用户点击按钮后发出，要求手动写本地并发送。
    void manualInjectRequested(const QString &text);

public slots:
    // 追加显示本地复制并准备发送的文本。
    void appendLocalText(const QString &text);
    // 追加显示接收到的远端文本。
    void appendRemoteText(const QString &text);

private:
    void onManualSendClicked();
    void appendEntry(QPlainTextEdit *target, const QString &text);

    QPlainTextEdit *m_manualInput = nullptr;
    QPushButton *m_manualSendButton = nullptr;
    QPlainTextEdit *m_localView = nullptr;
    QPlainTextEdit *m_remoteView = nullptr;
    int m_maxBlocks = 400;
};
