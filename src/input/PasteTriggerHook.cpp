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
    // 【重放原理/注入假按键】：标记接下来的 500ms 内，我们要“无视”捕获到的 Ctrl+V
    // 因为这是程序“自己”生成的假 Ctrl+V（当我们从远端下完文件刷进剪贴板后，要告诉目标程序去粘出来！）
    markSyntheticPasteWindow();
    m_lastReplayPasteError.clear();

#ifdef Q_OS_WIN
    // 原理：通过 Windows 的 SendInput API 向系统队列模拟注入按键动作
    // 逻辑：按顺序压入 "Ctrl按下 -> V按下 -> V弹起 -> Ctrl弹起"
    INPUT inputs[4] = {};

    inputs[0].type = INPUT_KEYBOARD;     // 1. 模拟按下 Ctrl 键
    inputs[0].ki.wVk = VK_CONTROL;

    inputs[1].type = INPUT_KEYBOARD;     // 2. 模拟按下 V 键
    inputs[1].ki.wVk = 'V';

    inputs[2].type = INPUT_KEYBOARD;     // 3. 模拟释放 V 键
    inputs[2].ki.wVk = 'V';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;

    inputs[3].type = INPUT_KEYBOARD;     // 4. 模拟释放 Ctrl 键
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    constexpr UINT inputCount = 4;
    // 将这 4 个键盘事件直接“塞进”操作系统的消息队列。当前活跃窗口（可能是微信/资源管理器）
    // 会以为这是用户真正敲击了键盘，此时由于我们有刚下好文件的真数据了，它们提取就会成功！
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
    // 【兜底原理】：应用级别的按键过滤。
    // 当我们的应用自己拥有焦点时（即在前台时），不仅可以通过底层 Hook 判断，
    // Qt 的事件循环也会分发按键事件。作为双保险，这里也做拦截处理。
    if (event->type() == QEvent::KeyPress)
    {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const Qt::KeyboardModifiers mods = keyEvent->modifiers();
        if (keyEvent->key() == Qt::Key_V && (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier))
        {
            qInfo() << "app-level Ctrl+Shift+V detected";
            emitCtrlShiftPasteTriggeredDebounced();
            return QObject::eventFilter(watched, event); // 放行给子组件
        }

        // QKeySequence::Paste 是跨平台的粘贴键位匹配（Windows 是 Ctrl+V，Mac 是 Cmd+V）
        if (keyEvent->matches(QKeySequence::Paste))
        {
            // 判断是否应该忽略这个粘贴快捷键事件（比如刚刚为了真正写剪贴板，程序自己补发过 Ctrl+V 导致的事件），
            // 如果是的话就直接放行，避免“拦截自己的粘贴动作”，死循环。
            if (shouldIgnorePasteShortcut())
            {
                return QObject::eventFilter(watched, event);
            }

            qInfo() << "app-level paste shortcut detected";
            // 触发业务层逻辑（如发送请求到远端拉取文件）
            emitPasteTriggeredDebounced();
            
            // 如果业务层决定本次拦截生效，返回 true 将阻止该按键事件继续派发到我们应用内部组件（例如输入框）
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
    // 【核心原理】：Windows 全局低级键盘钩子 (WH_KEYBOARD_LL) 回调函数。
    // 操作系统在处理任何键盘输入之前，会先调用这个函数。
    // 即便我们的剪贴板同步应用在后台（没有焦点），依然能截获所有键盘事件。
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
    {
        auto *kbd = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
        // 核心检测逻辑：当前按下的物理键位是 'V'，且同时检测到系统状态中 'Ctrl' 键被按下 (最高位置 1 即 0x8000 表示按下)
        if (kbd && kbd->vkCode == 'V' && (GetAsyncKeyState(VK_CONTROL) & 0x8000))
        {
            if (s_instance)
            {
                const bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                
                // 【情况A】：如果是程序自己模拟出来的“假” Ctrl+V（比如我们刚从远端拉取完文件，并且写进剪贴板了准备释放），
                // 那么 shouldIgnorePasteShortcut() 会返回 true，此时直接放行，避免死循环拦截。
                if (!shiftDown && s_instance->shouldIgnorePasteShortcut())
                {
                    return CallNextHookEx(nullptr, nCode, wParam, lParam);
                }

                // 【情况B】：程序决定在这个时候吃掉用户的按键事件（比如发现确实有远端传来的offer）
                const bool interceptPaste = !shiftDown && s_instance->shouldInterceptPasteShortcut();
                
                // 将拦截事件抛到 Qt 主线程去处理，因为 Hook 是在系统线程/非界面线程回调的
                QMetaObject::invokeMethod(s_instance, [inst = s_instance, shiftDown]()
                                          {
                                              if (!inst) return;
                                              if (shiftDown) {
                                                  // 处理 Ctrl + Shift + V (如纯文本粘贴等辅助功能)
                                                  inst->emitCtrlShiftPasteTriggeredDebounced();
                                              } else {
                                                  // 发出信号，通知上层业务网络状态机 "检测到粘贴动作啦，开始拉请求！"
                                                  inst->emitPasteTriggeredDebounced();
                                              } }, Qt::QueuedConnection);

                // 【最关键的一步】：如果 interceptPaste 为 true，直接 `return 1;` 
                // 返回非 0 值会告诉 Windows 操作系统：“不要把这个按键传给其它程序了，在此彻底终止！”
                // 这就是为什么目标程序（如记事本、微信）会收不到这个 Ctrl+V 按键。
                if (interceptPaste)
                {
                    return 1;
                }
            }
        }
    }

    // 如果我们不关心这个按键，或者不想拦截，调用 CallNextHookEx 将按键原样传给下一个程序和操作系统
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

bool PasteTriggerHook::installWindowsGlobalHook()
{
    if (m_keyboardHook)
    {
        return true;
    }

    s_instance = this;
    // 【注册原理】：调用 Win32 API 注册全局低级键盘钩子 (WH_KEYBOARD_LL)。
    // 这会将上面定义的 keyboardProc 注册到系统的 Hook 链条中最前端，拦截所有键盘输入消息。
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
