#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>

class ClipboardWriter : public QObject
{
    Q_OBJECT

public:
    explicit ClipboardWriter(QObject *parent = nullptr);
    // 将远端文本写入本地剪贴板，并记录防回环指纹。
    bool writeRemoteText(const QString &text, quint64 sessionId);
    // 将远端文件列表写入本地剪贴板，并记录防回环指纹。
    bool writeRemoteFileList(const QStringList &paths, quint64 sessionId);
    // 若该文本哈希是最近一次远端注入产生，则返回 true。
    bool isRecentlyInjected(quint32 textHash) const;
    // 若该文件列表哈希是最近一次远端注入产生，则返回 true。
    bool isRecentlyInjectedFileList(quint32 listHash) const;

private:
    // 清理已过期的防回环指纹。
    void cleanupExpired() const;

    // 哈希 -> 注入时间，用于短时间窗口内抑制回环。
    mutable QHash<quint32, QDateTime> m_recentInjectedHashes;
    // 文件列表哈希 -> 注入时间。
    mutable QHash<quint32, QDateTime> m_recentInjectedFileHashes;
    // 认为“最近注入”的时间窗口（毫秒）。
    int m_injectionTtlMs = 1500;
    // 写剪贴板失败时的重试次数。
    int m_writeRetryCount = 6;
    // 写剪贴板重试间隔（毫秒）。
    int m_writeRetryDelayMs = 40;
};
