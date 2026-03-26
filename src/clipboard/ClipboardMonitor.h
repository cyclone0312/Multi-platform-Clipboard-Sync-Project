#pragma once

#include <QObject>

class ClipboardMonitor : public QObject
{
    Q_OBJECT

public:
    explicit ClipboardMonitor(QObject *parent = nullptr);

signals:
    // 当本地剪贴板文本可读且非空时发出。
    void localTextChanged(const QString &text, quint32 textHash);

private:
    // QClipboard::dataChanged 的入口处理函数。
    void handleClipboardChanged();
    // 带重试读取剪贴板文本，处理延迟提供数据的场景。
    void tryEmitClipboardText();

    // 当前变更事件下剩余的读取重试次数。
    int m_readRetryBudget = 0;
    // 单次剪贴板变更允许的最大重试次数。
    int m_maxReadRetries = 8;
    // 两次重试之间的延迟毫秒数。
    int m_readRetryDelayMs = 30;
    // 防止重复调度重叠的重试定时器。
    bool m_retryScheduled = false;
};
