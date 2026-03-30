#pragma once

#include <memory>

#include <QDateTime>
#include <QHash>
#include <QObject>

#include "clipboard/IClipboardBackend.h"
#include "clipboard/ClipboardSnapshot.h"

class ClipboardWriter : public QObject
{
    Q_OBJECT

public:
    explicit ClipboardWriter(QObject *parent = nullptr);
    ClipboardWriter(std::unique_ptr<IClipboardBackend> backend, QObject *parent = nullptr);
    void setVirtualFileProvider(IVirtualFileProvider *provider);
    // Preferred path: the writer owns retry and anti-loop policy,
    // while the backend owns platform-specific clipboard publication.
    bool writeRemoteSnapshot(const clipboard::Snapshot &snapshot, quint64 sessionId);
    // 将远端文本写入本地剪贴板，并记录防回环指纹。
    bool writeRemoteText(const QString &text, quint64 sessionId);
    // 将远端 PNG 图片写入本地剪贴板，并记录防回环指纹。
    bool writeRemoteImage(const QByteArray &pngBytes, quint64 sessionId);
    // 将远端文件列表写入本地剪贴板，并记录防回环指纹。
    bool writeRemoteFileList(const QStringList &paths, quint64 sessionId);
    // 若该文本哈希是最近一次远端注入产生，则返回 true。
    bool isRecentlyInjected(quint32 textHash) const;
    bool isRecentlyInjectedSnapshot(quint32 snapshotHash) const;
    // 若该图片哈希是最近一次远端注入产生，则返回 true。
    bool isRecentlyInjectedImage(quint32 imageHash) const;
    // 若该文件列表哈希是最近一次远端注入产生，则返回 true。
    bool isRecentlyInjectedFileList(quint32 listHash) const;

private:
    // Anti-loop bookkeeping stays here so every backend shares the same policy.
    // 清理已过期的防回环指纹。
    void cleanupExpired() const;
    void markInjected(const clipboard::Snapshot &applied);

    // 哈希 -> 注入时间，用于短时间窗口内抑制回环。
    std::unique_ptr<IClipboardBackend> m_backend;
    IVirtualFileProvider *m_virtualFileProvider = nullptr;
    mutable QHash<quint32, QDateTime> m_recentInjectedHashes;
    mutable QHash<quint32, QDateTime> m_recentInjectedSnapshotHashes;
    // 图片哈希 -> 注入时间。
    mutable QHash<quint32, QDateTime> m_recentInjectedImageHashes;
    // 文件列表哈希 -> 注入时间。
    mutable QHash<quint32, QDateTime> m_recentInjectedFileHashes;
    // 认为“最近注入”的时间窗口（毫秒）。
    int m_injectionTtlMs = 1500;
    // 写剪贴板失败时的重试次数。
    int m_writeRetryCount = 6;
    // 写剪贴板重试间隔（毫秒）。
    int m_writeRetryDelayMs = 40;
};
