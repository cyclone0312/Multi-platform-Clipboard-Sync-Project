#include "clipboard/WindowsClipboardBackend.h"
#include "clipboard/WindowsVirtualFilePublisher.h"

#include <QDir>

#ifdef Q_OS_WIN
#include <cstring>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <string>
#endif

namespace
{
#ifdef Q_OS_WIN
    class ScopedClipboard
    {
    public:
        ScopedClipboard()
            : m_open(OpenClipboard(nullptr) != FALSE)
        {
        }

        ~ScopedClipboard()
        {
            if (m_open)
            {
                CloseClipboard();
            }
        }

        bool isOpen() const
        {
            return m_open;
        }

    private:
        bool m_open = false;
    };

    template <typename FillFn>
    bool setClipboardHandle(UINT format, SIZE_T sizeBytes, FillFn fill)
    {
        HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, sizeBytes);
        if (!handle)
        {
            return false;
        }

        void *locked = GlobalLock(handle);
        if (!locked)
        {
            GlobalFree(handle);
            return false;
        }

        fill(locked);
        GlobalUnlock(handle);

        if (!SetClipboardData(format, handle))
        {
            GlobalFree(handle);
            return false;
        }

        return true;
    }

    QString normalizeWindowsText(QString text)
    {
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace(QChar('\r'), QChar('\n'));
        text.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
        return text;
    }

    bool setUnicodeText(const QString &text)
    {
        std::wstring wide = normalizeWindowsText(text).toStdWString();
        wide.push_back(L'\0');
        return setClipboardHandle(CF_UNICODETEXT,
                                  static_cast<SIZE_T>(wide.size() * sizeof(wchar_t)),
                                  [&wide](void *dest)
                                  {
                                      memcpy(dest, wide.data(), wide.size() * sizeof(wchar_t));
                                  });
    }

    bool setFileDropList(const QStringList &paths)
    {
        QStringList cleaned;
        cleaned.reserve(paths.size());
        for (const QString &path : paths)
        {
            const QString cleanedPath = QDir::toNativeSeparators(QDir::cleanPath(path));
            if (!cleanedPath.isEmpty())
            {
                cleaned.push_back(cleanedPath);
            }
        }

        if (cleaned.isEmpty())
        {
            return false;
        }

        std::wstring payload;
        for (const QString &path : cleaned)
        {
            const std::wstring widePath = path.toStdWString();
            payload.append(widePath);
            payload.push_back(L'\0');
        }
        payload.push_back(L'\0');

        const SIZE_T totalBytes = sizeof(DROPFILES) +
                                  static_cast<SIZE_T>(payload.size() * sizeof(wchar_t));
        return setClipboardHandle(CF_HDROP,
                                  totalBytes,
                                  [&payload](void *dest)
                                  {
                                      auto *drop = static_cast<DROPFILES *>(dest);
                                      drop->pFiles = sizeof(DROPFILES);
                                      drop->pt.x = 0;
                                      drop->pt.y = 0;
                                      drop->fNC = FALSE;
                                      drop->fWide = TRUE;

                                      auto *files = reinterpret_cast<wchar_t *>(
                                          static_cast<unsigned char *>(dest) + sizeof(DROPFILES));
                                      memcpy(files, payload.data(), payload.size() * sizeof(wchar_t));
                                  });
    }

    bool writeSnapshotNative(const clipboard::Snapshot &snapshot)
    {
        ScopedClipboard clipboard;
        if (!clipboard.isOpen())
        {
            return false;
        }

        if (!EmptyClipboard())
        {
            return false;
        }

        bool wroteAny = false;
        if (snapshot.hasText())
        {
            wroteAny = setUnicodeText(snapshot.text) || wroteAny;
        }
        if (snapshot.hasLocalFiles())
        {
            wroteAny = setFileDropList(snapshot.localFilePaths) || wroteAny;
        }

        return wroteAny;
    }
#endif
}

bool WindowsClipboardBackend::writeSnapshot(const ClipboardWriteRequest &request)
{
    const clipboard::Snapshot &snapshot = request.snapshot;
#ifdef Q_OS_WIN
    if (snapshot.isEmpty())
    {
        return false;
    }

    if (snapshot.hasTransportFiles() &&
        !snapshot.hasLocalFiles() &&
        !snapshot.hasText() &&
        !snapshot.hasHtml() &&
        !snapshot.hasImage() &&
        publishWindowsVirtualFiles(request))
    {
        m_lastPublishedVirtualSnapshot = snapshot;
        return true;
    }

    m_lastPublishedVirtualSnapshot = {};

    // Keep richer mixed representations on the Qt path for now.
    if (snapshot.hasHtml() || snapshot.hasImage())
    {
        return m_fallback.writeSnapshot(request);
    }

    if (writeSnapshotNative(snapshot))
    {
        return true;
    }
#endif

    return m_fallback.writeSnapshot(request);
}

clipboard::Snapshot WindowsClipboardBackend::readCurrentSnapshot() const
{
#ifdef Q_OS_WIN
    if (!m_lastPublishedVirtualSnapshot.isEmpty())
    {
        return m_lastPublishedVirtualSnapshot;
    }
#endif
    return m_fallback.readCurrentSnapshot();
}

bool WindowsClipboardBackend::supportsNativeVirtualFiles() const
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

QString WindowsClipboardBackend::backendName() const
{
#ifdef Q_OS_WIN
    return QStringLiteral("windows-native-virtual");
#else
    return QStringLiteral("windows-qt-fallback");
#endif
}
