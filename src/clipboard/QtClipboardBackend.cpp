#include "clipboard/QtClipboardBackend.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QUrl>

bool QtClipboardBackend::writeSnapshot(const ClipboardWriteRequest &request)
{
    const clipboard::Snapshot &snapshot = request.snapshot;
    if (snapshot.isEmpty())
    {
        return false;
    }

    QImage image;
    if (snapshot.hasImage() &&
        !image.loadFromData(snapshot.imagePng, "PNG") &&
        !image.loadFromData(snapshot.imagePng))
    {
        return false;
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
    }

    QGuiApplication::clipboard()->setMimeData(mimeData);
    return true;
}

clipboard::Snapshot QtClipboardBackend::readCurrentSnapshot() const
{
    QClipboard *systemClipboard = QGuiApplication::clipboard();
    return clipboard::captureSnapshotFromMime(systemClipboard->mimeData(), systemClipboard->text());
}

bool QtClipboardBackend::supportsNativeVirtualFiles() const
{
    return false;
}

QString QtClipboardBackend::backendName() const
{
    return QStringLiteral("qt");
}
