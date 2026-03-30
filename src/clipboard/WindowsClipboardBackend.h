#pragma once

#include "clipboard/IClipboardBackend.h"
#include "clipboard/QtClipboardBackend.h"

class WindowsClipboardBackend : public IClipboardBackend
{
public:
    // Current scope: native Win32 clipboard for text and file lists, while
    // richer formats still fall back to Qt until IDataObject lands here.
    bool writeSnapshot(const ClipboardWriteRequest &request) override;
    clipboard::Snapshot readCurrentSnapshot() const override;
    bool supportsNativeVirtualFiles() const override;
    QString backendName() const override;

private:
    QtClipboardBackend m_fallback;
    mutable clipboard::Snapshot m_lastPublishedVirtualSnapshot;
};
