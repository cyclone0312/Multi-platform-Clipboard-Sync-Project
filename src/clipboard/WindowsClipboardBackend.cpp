#include "clipboard/WindowsClipboardBackend.h"

bool WindowsClipboardBackend::writeSnapshot(const clipboard::Snapshot &snapshot)
{
    return m_fallback.writeSnapshot(snapshot);
}

clipboard::Snapshot WindowsClipboardBackend::readCurrentSnapshot() const
{
    return m_fallback.readCurrentSnapshot();
}

bool WindowsClipboardBackend::supportsNativeVirtualFiles() const
{
    return false;
}

QString WindowsClipboardBackend::backendName() const
{
    return QStringLiteral("windows-qt-fallback");
}
