#pragma once

#include "clipboard/IClipboardBackend.h"
#include "clipboard/QtClipboardBackend.h"

class WindowsClipboardBackend : public IClipboardBackend
{
public:
    // Transport-backed files publish through a native IDataObject so they can
    // coexist with richer clipboard formats on Windows.
    bool writeSnapshot(const ClipboardWriteRequest &request) override;
    clipboard::Snapshot readCurrentSnapshot() const override;
    bool supportsNativeVirtualFiles() const override;
    QString backendName() const override;

private:
    QtClipboardBackend m_fallback;
    mutable clipboard::Snapshot m_lastPublishedVirtualSnapshot;
    mutable quint32 m_lastPublishedVirtualSequence = 0;
};