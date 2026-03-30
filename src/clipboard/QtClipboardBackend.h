#pragma once

#include "clipboard/IClipboardBackend.h"

class QtClipboardBackend : public IClipboardBackend
{
public:
    // Cross-platform fallback backend built on QClipboard/QMimeData.
    bool writeSnapshot(const ClipboardWriteRequest &request) override;
    clipboard::Snapshot readCurrentSnapshot() const override;
    bool supportsNativeVirtualFiles() const override;
    QString backendName() const override;
};
