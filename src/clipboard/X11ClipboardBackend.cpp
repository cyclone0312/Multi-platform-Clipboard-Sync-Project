#include "clipboard/X11ClipboardBackend.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QUrl>

namespace
{
    bool isLikelyX11Session()
    {
        const QByteArray sessionType = qgetenv("XDG_SESSION_TYPE").trimmed().toLower();
        if (sessionType == "x11")
        {
            return true;
        }
        if (sessionType == "wayland")
        {
            return false;
        }

        return qEnvironmentVariableIsSet("DISPLAY");
    }

    QByteArray buildUriListPayload(const QStringList &paths)
    {
        QByteArray payload;
        for (const QString &path : paths)
        {
            payload += QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded).toUtf8();
            payload += QByteArrayLiteral("\r\n");
        }
        return payload;
    }

    QMimeData *createMimeData(const clipboard::Snapshot &snapshot)
    {
        if (snapshot.isEmpty())
        {
            return nullptr;
        }

        QImage image;
        if (snapshot.hasImage() &&
            !image.loadFromData(snapshot.imagePng, "PNG") &&
            !image.loadFromData(snapshot.imagePng))
        {
            return nullptr;
        }

        auto *mimeData = new QMimeData();
        if (snapshot.hasText())
        {
            mimeData->setText(snapshot.text);
        }
        if (snapshot.hasHtml())
        {
            mimeData->setHtml(snapshot.html);
        }
        if (snapshot.hasImage())
        {
            mimeData->setData(QStringLiteral("image/png"), snapshot.imagePng);
            mimeData->setImageData(image);
        }
        if (snapshot.hasLocalFiles())
        {
            QList<QUrl> urls;
            urls.reserve(snapshot.localFilePaths.size());
            for (const QString &path : snapshot.localFilePaths)
            {
                urls.push_back(QUrl::fromLocalFile(path));
            }
            mimeData->setUrls(urls);
            mimeData->setData(QStringLiteral("text/uri-list"),
                              buildUriListPayload(snapshot.localFilePaths));
        }

        return mimeData;
    }
}

bool X11ClipboardBackend::writeSnapshot(const ClipboardWriteRequest &request)
{
    const clipboard::Snapshot &snapshot = request.snapshot;
    if (!isLikelyX11Session())
    {
        return m_fallback.writeSnapshot(request);
    }

    QClipboard *clipboard = QGuiApplication::clipboard();
    QMimeData *clipboardMime = createMimeData(snapshot);
    if (!clipboardMime)
    {
        return false;
    }

    clipboard->setMimeData(clipboardMime, QClipboard::Clipboard);

    if (clipboard->supportsSelection())
    {
        QMimeData *selectionMime = createMimeData(snapshot);
        if (!selectionMime)
        {
            return false;
        }
        clipboard->setMimeData(selectionMime, QClipboard::Selection);
    }

    return true;
}

clipboard::Snapshot X11ClipboardBackend::readCurrentSnapshot() const
{
    if (!isLikelyX11Session())
    {
        return m_fallback.readCurrentSnapshot();
    }

    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard::Snapshot current = clipboard::captureSnapshotFromMime(
        clipboard->mimeData(QClipboard::Clipboard),
        clipboard->text(QClipboard::Clipboard));

    if (!current.isEmpty())
    {
        return current;
    }

    if (clipboard->supportsSelection())
    {
        return clipboard::captureSnapshotFromMime(
            clipboard->mimeData(QClipboard::Selection),
            clipboard->text(QClipboard::Selection));
    }

    return current;
}

bool X11ClipboardBackend::supportsNativeVirtualFiles() const
{
    return false;
}

QString X11ClipboardBackend::backendName() const
{
    return isLikelyX11Session()
               ? QStringLiteral("x11-clipboard-selection")
               : QStringLiteral("x11-qt-fallback");
}
