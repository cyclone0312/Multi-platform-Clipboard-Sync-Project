#include "input/PasteTriggerHook.h"

#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMetaObject>
#include <QProcess>
#include <QThread>

#if defined(Q_OS_LINUX) && defined(CLIPBOARD_SYNC_HAS_X11_HOOK)
#include <X11/Xlib.h>
#include <X11/keysym.h>
#if defined(CLIPBOARD_SYNC_HAS_X11_XTEST)
#include <X11/extensions/XTest.h>
#endif

// X11 宏会污染命名空间并与 Qt 枚举名冲突（如 QEvent::KeyPress）。
#ifdef KeyPress
#undef KeyPress
#endif
#ifdef KeyRelease
#undef KeyRelease
#endif
#endif

#ifdef Q_OS_WIN
PasteTriggerHook *PasteTriggerHook::s_instance = nullptr;
#endif

PasteTriggerHook::PasteTriggerHook(QObject *parent)
    : QObject(parent)
{
}

#if defined(Q_OS_LINUX) && defined(CLIPBOARD_SYNC_HAS_X11_HOOK)
namespace
{
    Display *asXDisplay(void *display)
    {
        return reinterpret_cast<Display *>(display);
    }

    bool isKeyPressed(const char keymap[32], int keyCode)
    {
        if (keyCode <= 0)
        {
            return false;
        }

        const int idx = keyCode / 8;
        const int bit = keyCode % 8;
        if (idx < 0 || idx >= 32)
        {
            return false;
        }

        return (keymap[idx] & (1 << bit)) != 0;
    }

    bool isX11Session()
    {
        const QByteArray sessionType = qgetenv("XDG_SESSION_TYPE").trimmed().toLower();
        if (sessionType == "x11")
        {
            return true;
        }
        if (sessionType == "wayland")
        {
            return false;
        }

        return qEnvironmentVariableIsSet("DISPLAY");
    }
}
#endif

PasteTriggerHook::~PasteTriggerHook()
{
    stop();
}

bool PasteTriggerHook::start()
{
    if (m_started)
    {
        return true;
    }

    if (QApplication::instance())
    {
        QApplication::instance()->installEventFilter(this);
    }

#ifdef Q_OS_WIN
    if (!installWindowsGlobalHook())
    {
        if (QApplication::instance())
        {
            QApplication::instance()->removeEventFilter(this);
        }
        return false;
    }
#endif

#if defined(Q_OS_LINUX) && defined(CLIPBOARD_SYNC_HAS_X11_HOOK)
    if (!installLinuxX11GlobalHook())
    {
#ifdef Q_OS_WIN
        uninstallWindowsGlobalHook();
#endif
        if (QApplication::instance())
        {
            QApplication::instance()->removeEventFilter(this);
        }
        return false;
    }
#endif

    m_started = true;
    return true;
}

void PasteTriggerHook::stop()
{
    if (!m_started)
    {
        return;
    }

    if (QApplication::instance())
    {
        QApplication::instance()->removeEventFilter(this);
    }

#ifdef Q_OS_WIN
    uninstallWindowsGlobalHook();
#endif

#if defined(Q_OS_LINUX) && defined(CLIPBOARD_SYNC_HAS_X11_HOOK)
    uninstallLinuxX11GlobalHook();
#endif

    m_started = false;
}

void PasteTriggerHook::setPasteInterceptDecider(PasteInterceptDecider decider, void *context)
{
    m_pasteInterceptDecider = decider;
    m_pasteInterceptContext = context;
}

bool PasteTriggerHook::replayPasteShortcut()
{
    markSyntheticPasteWindow();
    m_lastReplayPasteError.clear();

#ifdef Q_OS_WIN
    INPUT inputs[4] = {};

    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'V';

    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'V';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;

    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    constexpr UINT inputCount = 4;
    const UINT sent = SendInput(inputCount, inputs, sizeof(INPUT));
    if (sent != inputCount)
    {
        m_ignorePasteUntilMs.store(0);
        m_lastReplayPasteError = QStringLiteral("SendInput failed");
        qWarning() << "failed to replay Ctrl+V via SendInput";
        return false;
    }

    return true;
#elif defined(Q_OS_LINUX) && defined(CLIPBOARD_SYNC_HAS_X11_HOOK) && defined(CLIPBOARD_SYNC_HAS_X11_XTEST)
    if (!isX11Session())
    {
        m_ignorePasteUntilMs.store(0);
        m_lastReplayPasteError = QStringLiteral("current desktop session is not X11");
        return false;
    }

    Display *display = XOpenDisplay(nullptr);
    if (!display)
    {
        m_ignorePasteUntilMs.store(0);
        m_lastReplayPasteError = QStringLiteral("XOpenDisplay failed");
        qWarning() << "X11 display unavailable, failed to replay Ctrl+V";
        return false;
    }

    int eventBase = 0;
    int errorBase = 0;
    int major = 0;
    int minor = 0;
    if (!XTestQueryExtension(display, &eventBase, &errorBase, &major, &minor))
    {
        XCloseDisplay(display);
        m_ignorePasteUntilMs.store(0);
        m_lastReplayPasteError = QStringLiteral("XTEST extension is unavailable on this X server");
        qWarning() << "XTEST extension unavailable, failed to replay Ctrl+V";
        return false;
    }

    const KeyCode ctrlKeyCode = XKeysymToKeycode(display, XK_Control_L);
    KeyCode vKeyCode = XKeysymToKeycode(display, XK_V);
    if (vKeyCode == 0)
    {
        vKeyCode = XKeysymToKeycode(display, XK_v);
    }

    if (ctrlKeyCode == 0 || vKeyCode == 0)
    {
        XCloseDisplay(display);
        m_ignorePasteUntilMs.store(0);
        m_lastReplayPasteError = QStringLiteral("failed to resolve X11 keycodes for Ctrl+V");
        qWarning() << "failed to resolve X11 keycodes for replay";
        return false;
    }

    const bool ok = XTestFakeKeyEvent(display, ctrlKeyCode, True, CurrentTime) != 0 &&
                    XTestFakeKeyEvent(display, vKeyCode, True, CurrentTime) != 0 &&
                    XTestFakeKeyEvent(display, vKeyCode, False, CurrentTime) != 0 &&
                    XTestFakeKeyEvent(display, ctrlKeyCode, False, CurrentTime) != 0;
    XFlush(display);
    XCloseDisplay(display);

    if (!ok)
    {
        m_ignorePasteUntilMs.store(0);
        m_lastReplayPasteError = QStringLiteral("XTestFakeKeyEvent returned failure");
        qWarning() << "failed to replay Ctrl+V via XTest";
        return false;
    }

    return true;
#elif defined(Q_OS_LINUX) && defined(CLIPBOARD_SYNC_HAS_X11_HOOK)
    if (!isX11Session())
    {
        m_ignorePasteUntilMs.store(0);
        m_lastReplayPasteError = QStringLiteral("current desktop session is not X11");
        return false;
    }

    QProcess process;
    process.start(QStringLiteral("xdotool"),
                  {QStringLiteral("key"),
                   QStringLiteral("--clearmodifiers"),
                   QStringLiteral("ctrl+v")});
    if (!process.waitForStarted(1000))
    {
        m_ignorePasteUntilMs.store(0);
        m_lastReplayPasteError = QStringLiteral("xdotool is unavailable");
        qWarning() << "failed to replay Ctrl+V because xdotool is unavailable";
        return false;
    }

    if (!process.waitForFinished(2000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        m_ignorePasteUntilMs.store(0);
        const QString stderrText = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        m_lastReplayPasteError = stderrText.isEmpty()
                                     ? QStringLiteral("xdotool key ctrl+v failed")
                                     : QStringLiteral("xdotool failed: %1").arg(stderrText);
        qWarning() << "failed to replay Ctrl+V via xdotool";
        return false;
    }

    return true;
#else
    m_ignorePasteUntilMs.store(0);
    m_lastReplayPasteError = QStringLiteral("this build has no supported global paste replay backend");
    qWarning() << "paste replay is not supported on this platform";
    return false;
#endif
}

QString PasteTriggerHook::lastReplayPasteError() const
{
    return m_lastReplayPasteError;
}

bool PasteTriggerHook::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched)
    // 应用内按键兜底：窗口有焦点时可捕获 Ctrl+V / Ctrl+Shift+V。
    // 注意：这不是系统全局钩子，窗口失焦时不保证触发。
    if (event->type() == QEvent::KeyPress)
    {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const Qt::KeyboardModifiers mods = keyEvent->modifiers();
        if (keyEvent->key() == Qt::Key_V && (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier))
        {
            qInfo() << "app-level Ctrl+Shift+V detected";
            emitCtrlShiftPasteTriggeredDebounced();
            return QObject::eventFilter(watched, event);
        }

        if (keyEvent->matches(QKeySequence::Paste))
        {
            if (shouldIgnorePasteShortcut())
            {
                return QObject::eventFilter(watched, event);
            }

            qInfo() << "app-level paste shortcut detected";
            emitPasteTriggeredDebounced();
            if (shouldInterceptPasteShortcut())
            {
                return true;
            }
        }
    }

    return QObject::eventFilter(watched, event);
}

void PasteTriggerHook::emitPasteTriggeredDebounced()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastTriggeredMs < m_debounceMs)
    {
        return;
    }

    m_lastTriggeredMs = now;
    emit pasteTriggered();
}

void PasteTriggerHook::emitCtrlShiftPasteTriggeredDebounced()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastCtrlShiftTriggeredMs < m_debounceMs)
    {
        return;
    }

    m_lastCtrlShiftTriggeredMs = now;
    emit ctrlShiftPasteTriggered();
}

bool PasteTriggerHook::shouldInterceptPasteShortcut() const
{
    return m_pasteInterceptDecider && m_pasteInterceptDecider(m_pasteInterceptContext);
}

bool PasteTriggerHook::shouldIgnorePasteShortcut() const
{
    return QDateTime::currentMSecsSinceEpoch() < m_ignorePasteUntilMs.load();
}

void PasteTriggerHook::markSyntheticPasteWindow()
{
    m_ignorePasteUntilMs.store(QDateTime::currentMSecsSinceEpoch() + 500);
}

#ifdef Q_OS_WIN
LRESULT CALLBACK PasteTriggerHook::keyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    // Windows 全局低级键盘钩子：即便应用不在前台也能捕获组合键。
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
    {
        auto *kbd = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
        if (kbd && kbd->vkCode == 'V' && (GetAsyncKeyState(VK_CONTROL) & 0x8000))
        {
            if (s_instance)
            {
                const bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                if (!shiftDown && s_instance->shouldIgnorePasteShortcut())
                {
                    return CallNextHookEx(nullptr, nCode, wParam, lParam);
                }

                const bool interceptPaste = !shiftDown && s_instance->shouldInterceptPasteShortcut();
                QMetaObject::invokeMethod(s_instance, [inst = s_instance, shiftDown]()
                                          {
                                              if (!inst)
                                              {
                                                  return;
                                              }

                                              if (shiftDown)
                                              {
                                                  inst->emitCtrlShiftPasteTriggeredDebounced();
                                              }
                                              else
                                              {
                                                  inst->emitPasteTriggeredDebounced();
                                              } }, Qt::QueuedConnection);

                if (interceptPaste)
                {
                    return 1;
                }
            }
        }
    }

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

bool PasteTriggerHook::installWindowsGlobalHook()
{
    if (m_keyboardHook)
    {
        return true;
    }

    s_instance = this;
    m_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, &PasteTriggerHook::keyboardProc, GetModuleHandleW(nullptr), 0);
    if (!m_keyboardHook)
    {
        s_instance = nullptr;
        return false;
    }

    return true;
}

void PasteTriggerHook::uninstallWindowsGlobalHook()
{
    if (m_keyboardHook)
    {
        UnhookWindowsHookEx(m_keyboardHook);
        m_keyboardHook = nullptr;
    }

    if (s_instance == this)
    {
        s_instance = nullptr;
    }
}
#endif

#if defined(Q_OS_LINUX) && defined(CLIPBOARD_SYNC_HAS_X11_HOOK)
bool PasteTriggerHook::installLinuxX11GlobalHook()
{
    if (!isX11Session())
    {
        return true;
    }

    if (m_x11Display)
    {
        return true;
    }

    Display *display = XOpenDisplay(nullptr);
    if (!display)
    {
        qWarning() << "X11 display unavailable, fallback to app-level paste trigger";
        return true;
    }
    m_x11Display = display;

    m_x11VKeyCode = XKeysymToKeycode(asXDisplay(m_x11Display), XK_V);
    m_x11CtrlLKeyCode = XKeysymToKeycode(asXDisplay(m_x11Display), XK_Control_L);
    m_x11CtrlRKeyCode = XKeysymToKeycode(asXDisplay(m_x11Display), XK_Control_R);
    m_x11ShiftLKeyCode = XKeysymToKeycode(asXDisplay(m_x11Display), XK_Shift_L);
    m_x11ShiftRKeyCode = XKeysymToKeycode(asXDisplay(m_x11Display), XK_Shift_R);
    if (m_x11VKeyCode == 0 ||
        (m_x11CtrlLKeyCode == 0 && m_x11CtrlRKeyCode == 0) ||
        (m_x11ShiftLKeyCode == 0 && m_x11ShiftRKeyCode == 0))
    {
        qWarning() << "X11 keycode lookup failed for global paste detection";
        XCloseDisplay(asXDisplay(m_x11Display));
        m_x11Display = nullptr;
        return true;
    }
    m_x11PrevComboDown = false;
    m_x11PrevCtrlShiftComboDown = false;

    m_x11Running.store(true);
    m_x11Thread = std::thread([this]()
                              { runLinuxX11EventLoop(); });
    return true;
}

void PasteTriggerHook::uninstallLinuxX11GlobalHook()
{
    m_x11Running.store(false);
    if (m_x11Thread.joinable())
    {
        m_x11Thread.join();
    }

    if (!m_x11Display)
    {
        return;
    }

    XCloseDisplay(asXDisplay(m_x11Display));
    m_x11Display = nullptr;
    m_x11VKeyCode = 0;
    m_x11CtrlLKeyCode = 0;
    m_x11CtrlRKeyCode = 0;
    m_x11ShiftLKeyCode = 0;
    m_x11ShiftRKeyCode = 0;
    m_x11PrevComboDown = false;
    m_x11PrevCtrlShiftComboDown = false;
}

void PasteTriggerHook::runLinuxX11EventLoop()
{
    while (m_x11Running.load())
    {
        if (!m_x11Display)
        {
            QThread::msleep(30);
            continue;
        }

        char keymap[32] = {};
        XQueryKeymap(asXDisplay(m_x11Display), keymap);

        const bool ctrlDown = isKeyPressed(keymap, m_x11CtrlLKeyCode) || isKeyPressed(keymap, m_x11CtrlRKeyCode);
        const bool shiftDown = isKeyPressed(keymap, m_x11ShiftLKeyCode) || isKeyPressed(keymap, m_x11ShiftRKeyCode);
        const bool vDown = isKeyPressed(keymap, m_x11VKeyCode);
        const bool ctrlShiftComboDown = ctrlDown && shiftDown && vDown;
        const bool comboDown = ctrlDown && !shiftDown && vDown;

        if (ctrlShiftComboDown && !m_x11PrevCtrlShiftComboDown)
        {
            qInfo() << "X11 global Ctrl+Shift+V detected";
            QMetaObject::invokeMethod(this, [this]()
                                      { emitCtrlShiftPasteTriggeredDebounced(); }, Qt::QueuedConnection);
        }

        if (comboDown && !m_x11PrevComboDown)
        {
            if (shouldIgnorePasteShortcut())
            {
                m_x11PrevComboDown = comboDown;
                m_x11PrevCtrlShiftComboDown = ctrlShiftComboDown;
                QThread::msleep(30);
                continue;
            }

            qInfo() << "X11 global Ctrl+V detected";
            QMetaObject::invokeMethod(this, [this]()
                                      { emitPasteTriggeredDebounced(); }, Qt::QueuedConnection);
        }

        m_x11PrevCtrlShiftComboDown = ctrlShiftComboDown;
        m_x11PrevComboDown = comboDown;

        QThread::msleep(30);
    }
}
#endif
