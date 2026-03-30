#pragma once
#include <atomic>

#include <QString>
#include <QObject>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#if defined(Q_OS_LINUX) && defined(CLIPBOARD_SYNC_HAS_X11_HOOK)
#include <thread>
#endif

class QEvent;

class PasteTriggerHook : public QObject
{
    Q_OBJECT

public:
    using PasteInterceptDecider = bool (*)(void *context);

    explicit PasteTriggerHook(QObject *parent = nullptr);
    ~PasteTriggerHook() override;

    // 启动粘贴触发监听。
    bool start();
    // 停止粘贴触发监听。
    void stop();
    void setPasteInterceptDecider(PasteInterceptDecider decider, void *context);
    bool replayPasteShortcut();
    QString lastReplayPasteError() const;

signals:
    // 检测到 Ctrl+V 粘贴触发时发出。
    void pasteTriggered();
    // 检测到 Ctrl+Shift+V 时发出（用于触发远端文件拉取）。
    void ctrlShiftPasteTriggered();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void emitPasteTriggeredDebounced();
    void emitCtrlShiftPasteTriggeredDebounced();
    bool shouldInterceptPasteShortcut() const;
    bool shouldIgnorePasteShortcut() const;
    void markSyntheticPasteWindow();

#ifdef Q_OS_WIN
    static LRESULT CALLBACK keyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    bool installWindowsGlobalHook();
    void uninstallWindowsGlobalHook();

    static PasteTriggerHook *s_instance;
    HHOOK m_keyboardHook = nullptr;
#endif

#if defined(Q_OS_LINUX) && defined(CLIPBOARD_SYNC_HAS_X11_HOOK)
    bool installLinuxX11GlobalHook();
    void uninstallLinuxX11GlobalHook();
    void runLinuxX11EventLoop();

    void *m_x11Display = nullptr;
    int m_x11VKeyCode = 0;
    int m_x11CtrlLKeyCode = 0;
    int m_x11CtrlRKeyCode = 0;
    int m_x11ShiftLKeyCode = 0;
    int m_x11ShiftRKeyCode = 0;
    bool m_x11PrevComboDown = false;
    bool m_x11PrevCtrlShiftComboDown = false;
    std::atomic_bool m_x11Running{false};
    std::thread m_x11Thread;
#endif

    bool m_started = false;
    qint64 m_lastTriggeredMs = 0;
    qint64 m_lastCtrlShiftTriggeredMs = 0;
    int m_debounceMs = 120;
    PasteInterceptDecider m_pasteInterceptDecider = nullptr;
    void *m_pasteInterceptContext = nullptr;
    std::atomic<qint64> m_ignorePasteUntilMs{0};
    QString m_lastReplayPasteError;
};
