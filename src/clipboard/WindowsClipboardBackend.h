#pragma once

#include "clipboard/IClipboardBackend.h"
#include "clipboard/QtClipboardBackend.h"

class WindowsClipboardBackend : public IClipboardBackend
{
public:
    // Placeholder backend: keeps the Windows write path isolated so native
    // IDataObject/virtual-file support can land here later.
    bool writeSnapshot(const clipboard::Snapshot &snapshot) override;
    clipboard::Snapshot readCurrentSnapshot() const override;
    bool supportsNativeVirtualFiles() const override;
    QString backendName() const override;

private:
    QtClipboardBackend m_fallback;
};
