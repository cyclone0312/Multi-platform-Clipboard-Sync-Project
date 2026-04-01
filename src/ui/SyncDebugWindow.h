#pragma once

#include <QStringList>
#include <QWidget>

class QPlainTextEdit;
class QPushButton;
class QListWidget;

class SyncDebugWindow : public QWidget
{
    Q_OBJECT

public:
    explicit SyncDebugWindow(QWidget *parent = nullptr);

signals:
    // 用户点击按钮后发出，要求手动写本地并发送。
    void manualInjectRequested(const QString &text);
    // 当用户把文件拖进窗口拖入区时发出。
    void localFilesDropped(const QStringList &paths);
    // 用户点击按钮后发出，模拟粘贴时触发远端文件请求。
    void requestRemoteFilesTriggered();

public slots:
    // 追加显示本地复制并准备发送的文本。
    void appendLocalText(const QString &text);
    // 追加显示接收到的远端文本。
    void appendRemoteText(const QString &text);
    // 追加文件传输状态日志。
    void appendFileTransferStatus(const QString &status);
    // 将已经下载到本地的文件加入“可拖出”列表。
    void appendDownloadedFiles(const QStringList &paths);

private:
    void onManualSendClicked();
    void onRequestRemoteFilesClicked();
    void appendEntry(QPlainTextEdit *target, const QString &text);

    QPlainTextEdit *m_manualInput = nullptr;
    QPushButton *m_manualSendButton = nullptr;
    QPushButton *m_requestRemoteFilesButton = nullptr;
    QWidget *m_dropZone = nullptr;
    QListWidget *m_readyFileList = nullptr;
    QPlainTextEdit *m_localView = nullptr;
    QPlainTextEdit *m_remoteView = nullptr;
    int m_maxBlocks = 400;
};
