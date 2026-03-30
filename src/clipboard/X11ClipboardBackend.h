#pragma once

#include "clipboard/IClipboardBackend.h"
#include "clipboard/QtClipboardBackend.h"

class X11ClipboardBackend : public IClipboardBackend
{
public:
    // Current scope: X11-aware Qt publishing to both CLIPBOARD and PRIMARY,
    // leaving a dedicated home for future native selection-owner logic.
    bool writeSnapshot(const ClipboardWriteRequest &request) override;
    clipboard::Snapshot readCurrentSnapshot() const override;
    bool supportsNativeVirtualFiles() const override;
    QString backendName() const override;

private:
    QtClipboardBackend m_fallback;
};
