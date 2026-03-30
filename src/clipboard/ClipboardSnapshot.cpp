#include "clipboard/ClipboardSnapshot.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QMimeData>
#include <QPixmap>
#include <QUrl>

namespace
{
    QString canonicalClipboardText(QString text)
    {
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace(QChar('\r'), QChar('\n'));
        return text;
    }

    QStringList canonicalLocalFilePaths(const QList<QUrl> &urls)
    {
        QStringList paths;
        paths.reserve(urls.size());
        for (const QUrl &url : urls)
        {
            if (!url.isLocalFile())
            {
                continue;
            }

            const QString local = QDir::cleanPath(url.toLocalFile());
            if (!local.isEmpty())
            {
                paths.push_back(local);
            }
        }

        return paths;
    }

    quint32 hashPaths(const QStringList &paths)
    {
        if (paths.isEmpty())
        {
            return 0;
        }

        QStringList normalized = paths;
        for (QString &path : normalized)
        {
            path = QDir::cleanPath(path).toLower();
        }
        normalized.sort();
        return qHash(normalized.join(QStringLiteral("\n")));
    }

    quint32 hashImageContent(const QImage &image)
    {
        if (image.isNull())
        {
            return 0;
        }

        const QImage normalized = image.convertToFormat(QImage::Format_RGBA8888);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        const QByteArray meta = QByteArray::number(normalized.width()) + 'x' +
                                QByteArray::number(normalized.height()) + ':' +
                                QByteArray::number(static_cast<int>(normalized.format()));
        hash.addData(meta);
        hash.addData(reinterpret_cast<const char *>(normalized.constBits()),
                     static_cast<int>(normalized.sizeInBytes()));
        return qHash(hash.result());
    }

    bool encodeImageAsPng(const QImage &image, QByteArray *outPng)
    {
        if (!outPng)
        {
            return false;
        }

        outPng->clear();
        if (image.isNull())
        {
            return false;
        }

        QBuffer buffer(outPng);
        if (!buffer.open(QIODevice::WriteOnly))
        {
            return false;
        }

        return image.save(&buffer, "PNG");
    }

    bool extractImage(const QMimeData *mime, QImage *outImage)
    {
        if (!mime || !outImage || !mime->hasImage())
        {
            return false;
        }

        const QVariant imageData = mime->imageData();
        if (imageData.canConvert<QImage>())
        {
            const QImage image = qvariant_cast<QImage>(imageData);
            if (!image.isNull())
            {
                *outImage = image;
                return true;
            }
        }

        if (imageData.canConvert<QPixmap>())
        {
            const QPixmap pixmap = qvariant_cast<QPixmap>(imageData);
            if (!pixmap.isNull())
            {
                *outImage = pixmap.toImage();
                return !outImage->isNull();
            }
        }

        const QByteArray pngData = mime->data(QStringLiteral("image/png"));
        if (!pngData.isEmpty())
        {
            QImage image;
            if (image.loadFromData(pngData, "PNG"))
            {
                *outImage = image;
                return true;
            }
        }

        return false;
    }

    quint32 hashTransportFiles(const QVector<clipboard::FileDescriptor> &files)
    {
        if (files.isEmpty())
        {
            return 0;
        }

        QStringList normalized;
        normalized.reserve(files.size());
        for (const clipboard::FileDescriptor &file : files)
        {
            normalized.push_back(file.fileId + QStringLiteral("|") +
                                 file.name + QStringLiteral("|") +
                                 QString::number(file.size) + QStringLiteral("|") +
                                 QString::number(file.mtimeMs) + QStringLiteral("|") +
                                 file.sha256);
        }
        normalized.sort();
        return qHash(normalized.join(QStringLiteral("\n")));
    }
}

namespace clipboard
{
    bool Snapshot::hasText() const
    {
        return !text.isEmpty();
    }

    bool Snapshot::hasHtml() const
    {
        return !html.isEmpty();
    }

    bool Snapshot::hasImage() const
    {
        return !imagePng.isEmpty();
    }

    bool Snapshot::hasLocalFiles() const
    {
        return !localFilePaths.isEmpty();
    }

    bool Snapshot::hasTransportFiles() const
    {
        return !files.isEmpty();
    }

    bool Snapshot::isEmpty() const
    {
        return !hasText() && !hasHtml() && !hasImage() && !hasLocalFiles() && !hasTransportFiles();
    }

    Snapshot captureSnapshotFromMime(const QMimeData *mime, const QString &clipboardTextFallback)
    {
        Snapshot snapshot;
        if (!mime)
        {
            return snapshot;
        }

        // Collect formats together instead of forcing a single-format choice.
        if (mime->hasUrls())
        {
            snapshot.localFilePaths = canonicalLocalFilePaths(mime->urls());
        }

        if (mime->hasHtml())
        {
            snapshot.html = mime->html();
        }

        if (mime->hasImage())
        {
            QImage image;
            if (extractImage(mime, &image))
            {
                encodeImageAsPng(image, &snapshot.imagePng);
            }
        }

        QString text = clipboardTextFallback;
        if (text.isNull() || text.isEmpty())
        {
            text = mime->text();
        }
        if (!text.isEmpty())
        {
            snapshot.text = canonicalClipboardText(text);
        }

        refreshSnapshotFingerprint(&snapshot);
        return snapshot;
    }

    void refreshSnapshotFingerprint(Snapshot *snapshot)
    {
        if (!snapshot)
        {
            return;
        }

        // Canonicalize once here so every caller compares the same content.
        snapshot->text = canonicalClipboardText(snapshot->text);
        snapshot->textHash = snapshot->text.isEmpty() ? 0 : qHash(snapshot->text);
        snapshot->htmlHash = snapshot->html.isEmpty() ? 0 : qHash(snapshot->html);

        if (!snapshot->imagePng.isEmpty())
        {
            QImage image;
            if (image.loadFromData(snapshot->imagePng, "PNG") || image.loadFromData(snapshot->imagePng))
            {
                snapshot->imageHash = hashImageContent(image);
            }
            else
            {
                snapshot->imageHash = qHash(snapshot->imagePng);
            }
        }
        else
        {
            snapshot->imageHash = 0;
        }

        snapshot->localFilesHash = hashPaths(snapshot->localFilePaths);
        snapshot->transportFilesHash = hashTransportFiles(snapshot->files);
        snapshot->fingerprint = hashSnapshot(*snapshot);
    }

    quint32 hashSnapshot(const Snapshot &snapshot)
    {
        QCryptographicHash hash(QCryptographicHash::Sha256);

        // fingerprint 琛ㄧず鈥滄暣鍖?snapshot鈥濈殑韬唤銆?        // 鍙鍏朵腑浠绘剰涓€绉嶆湁鏁堣〃绀哄彉浜嗭紝鏈€缁?fingerprint 灏变細鍙橈紝
        // The fingerprint identifies the whole snapshot, not just one format.
        if (snapshot.textHash != 0)
        {
            hash.addData("text", 4);
            hash.addData(snapshot.text.toUtf8());
        }

        if (snapshot.htmlHash != 0)
        {
            hash.addData("html", 4);
            hash.addData(snapshot.html.toUtf8());
        }

        if (snapshot.imageHash != 0)
        {
            hash.addData("image", 5);
            hash.addData(snapshot.imagePng);
        }

        if (snapshot.transportFilesHash != 0)
        {
            hash.addData("transport-files", 15);
            for (const FileDescriptor &file : snapshot.files)
            {
                hash.addData(file.fileId.toUtf8());
                hash.addData("\n", 1);
                hash.addData(file.name.toUtf8());
                hash.addData("\n", 1);
                hash.addData(QByteArray::number(file.size));
                hash.addData("\n", 1);
                hash.addData(QByteArray::number(file.mtimeMs));
                hash.addData("\n", 1);
                hash.addData(file.sha256.toUtf8());
                hash.addData("\n", 1);
            }
        }
        else if (snapshot.localFilesHash != 0)
        {
            hash.addData("local-files", 11);
            for (const QString &path : snapshot.localFilePaths)
            {
                hash.addData(QDir::cleanPath(path).toLower().toUtf8());
                hash.addData("\n", 1);
            }
        }

        return qHash(hash.result());
    }

    QJsonObject snapshotToJson(const Snapshot &snapshot)
    {
        QJsonObject root;
        // Only serialize content that can be reconstructed cross-machine.
        if (snapshot.hasText())
        {
            root.insert(QStringLiteral("text"), snapshot.text);
        }

        if (snapshot.hasHtml())
        {
            root.insert(QStringLiteral("html"), snapshot.html);
        }

        if (snapshot.hasImage())
        {
            root.insert(QStringLiteral("imagePngBase64"),
                        QString::fromLatin1(snapshot.imagePng.toBase64(QByteArray::Base64Encoding)));
        }

        if (!snapshot.files.isEmpty())
        {
            QJsonArray filesJson;
            for (const FileDescriptor &file : snapshot.files)
            {
                QJsonObject fileObj;
                fileObj.insert(QStringLiteral("fileId"), file.fileId);
                fileObj.insert(QStringLiteral("name"), file.name);
                fileObj.insert(QStringLiteral("size"), static_cast<double>(file.size));
                fileObj.insert(QStringLiteral("mtimeMs"), static_cast<double>(file.mtimeMs));
                fileObj.insert(QStringLiteral("sha256"), file.sha256);
                filesJson.push_back(fileObj);
            }
            root.insert(QStringLiteral("files"), filesJson);
        }

        return root;
    }

    bool snapshotFromJson(const QJsonObject &json, Snapshot *outSnapshot)
    {
        if (!outSnapshot)
        {
            return false;
        }

        // Rebuild hashes immediately so remote snapshots behave like local ones.
        Snapshot snapshot;
        snapshot.text = canonicalClipboardText(json.value(QStringLiteral("text")).toString());
        snapshot.html = json.value(QStringLiteral("html")).toString();

        const QString imageBase64 = json.value(QStringLiteral("imagePngBase64")).toString();
        if (!imageBase64.isEmpty())
        {
            snapshot.imagePng = QByteArray::fromBase64(imageBase64.toLatin1());
        }

        const QJsonArray filesJson = json.value(QStringLiteral("files")).toArray();
        for (const QJsonValue &value : filesJson)
        {
            const QJsonObject fileObj = value.toObject();
            FileDescriptor file;
            file.fileId = fileObj.value(QStringLiteral("fileId")).toString();
            file.name = fileObj.value(QStringLiteral("name")).toString();
            file.size = static_cast<qint64>(fileObj.value(QStringLiteral("size")).toDouble());
            file.mtimeMs = static_cast<qint64>(fileObj.value(QStringLiteral("mtimeMs")).toDouble());
            file.sha256 = fileObj.value(QStringLiteral("sha256")).toString();
            snapshot.files.push_back(file);
        }

        refreshSnapshotFingerprint(&snapshot);
        *outSnapshot = snapshot;
        return true;
    }
} // namespace clipboard
