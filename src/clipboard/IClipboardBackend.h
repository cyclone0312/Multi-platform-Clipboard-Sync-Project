#pragma once

#include <QString>

#include "clipboard/ClipboardSnapshot.h"

class IClipboardBackend
{
public:
    virtual ~IClipboardBackend() = default;

    // Publish the snapshot using the current platform's native clipboard mechanism.
    virtual bool writeSnapshot(const clipboard::Snapshot &snapshot) = 0;
    // Read back what the platform clipboard currently exposes so Writer can verify writes.
    virtual clipboard::Snapshot readCurrentSnapshot() const = 0;
    virtual bool supportsNativeVirtualFiles() const = 0;
    virtual QString backendName() const = 0;
};
