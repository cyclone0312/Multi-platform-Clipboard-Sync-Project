#pragma once

#include "clipboard/IClipboardBackend.h"
#include "clipboard/QtClipboardBackend.h"

class X11ClipboardBackend : public IClipboardBackend
{
public:
    // Placeholder backend: keeps X11-specific selection logic isolated so a
    // future native owner/INCR implementation can live here.
    bool writeSnapshot(const clipboard::Snapshot &snapshot) override;
    clipboard::Snapshot readCurrentSnapshot() const override;
    bool supportsNativeVirtualFiles() const override;
    QString backendName() const override;

private:
    QtClipboardBackend m_fallback;
};
