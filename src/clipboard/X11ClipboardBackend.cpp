#include "clipboard/X11ClipboardBackend.h"

bool X11ClipboardBackend::writeSnapshot(const clipboard::Snapshot &snapshot)
{
    return m_fallback.writeSnapshot(snapshot);
}

clipboard::Snapshot X11ClipboardBackend::readCurrentSnapshot() const
{
    return m_fallback.readCurrentSnapshot();
}

bool X11ClipboardBackend::supportsNativeVirtualFiles() const
{
    return false;
}

QString X11ClipboardBackend::backendName() const
{
    return QStringLiteral("x11-qt-fallback");
}
