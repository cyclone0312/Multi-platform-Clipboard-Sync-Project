#include "clipboard/WindowsVirtualFilePublisher.h"

#ifdef Q_OS_WIN

#include <QImage>
#include <QtGlobal>

#include <cstring>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objidl.h>
#include <ole2.h>
#include <windows.h>
#include <shlobj.h>

#include <utility>
#include <vector>

namespace
{
    const qint64 kWindowsEpochOffset100ns = 116444736000000000LL;
    const char kClipboardHtmlFormatName[] = "HTML Format";
    const char kClipboardPngFormatName[] = "PNG";
    const char kStartFragmentMarker[] = "<!--StartFragment-->";
    const char kEndFragmentMarker[] = "<!--EndFragment-->";

    bool ensureOleInitialized()
    {
        static bool attempted = false;
        static bool ok = false;
        if (!attempted)
        {
            const HRESULT hr = OleInitialize(nullptr);
            ok = SUCCEEDED(hr) || hr == S_FALSE;
            attempted = true;
        }

        return ok;
    }

    FILETIME unixMsToFileTime(qint64 mtimeMs)
    {
        FILETIME result{};
        if (mtimeMs <= 0)
        {
            return result;
        }

        const quint64 ticks = static_cast<quint64>(mtimeMs) * 10000ULL +
                              static_cast<quint64>(kWindowsEpochOffset100ns);
        result.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFULL);
        result.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
        return result;
    }

    template <typename FillFn>
    HGLOBAL createGlobalHandle(SIZE_T sizeBytes, FillFn fill)
    {
        HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeBytes);
        if (!handle)
        {
            return nullptr;
        }

        void *locked = GlobalLock(handle);
        if (!locked)
        {
            GlobalFree(handle);
            return nullptr;
        }

        fill(locked);
        GlobalUnlock(handle);
        return handle;
    }

    QString normalizeWindowsText(QString text)
    {
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace(QChar('\r'), QChar('\n'));
        text.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
        return text;
    }

    int utf8ByteOffset(const QString &text, int charOffset)
    {
        if (charOffset <= 0)
        {
            return 0;
        }

        return text.left(charOffset).toUtf8().size();
    }

    QByteArray wrapHtmlFragmentForClipboard(const QByteArray &fragmentUtf8)
    {
        QByteArray fullHtml;
        fullHtml.reserve(fragmentUtf8.size() + 128);
        fullHtml += "<html><body>";
        fullHtml += kStartFragmentMarker;
        fullHtml += fragmentUtf8;
        fullHtml += kEndFragmentMarker;
        fullHtml += "</body></html>";
        return fullHtml;
    }

    QByteArray buildClipboardHtmlDocument(const QString &html)
    {
        const QString startMarker = QString::fromLatin1(kStartFragmentMarker);
        const QString endMarker = QString::fromLatin1(kEndFragmentMarker);
        if (html.contains(startMarker) && html.contains(endMarker))
        {
            return html.toUtf8();
        }

        const QString lower = html.toLower();
        const int bodyStart = lower.indexOf(QStringLiteral("<body"));
        if (bodyStart < 0)
        {
            return wrapHtmlFragmentForClipboard(html.toUtf8());
        }

        const int bodyOpenEnd = lower.indexOf(QChar('>'), bodyStart);
        if (bodyOpenEnd < 0)
        {
            return wrapHtmlFragmentForClipboard(html.toUtf8());
        }

        const int bodyEnd = lower.indexOf(QStringLiteral("</body"), bodyOpenEnd + 1);
        if (bodyEnd < 0)
        {
            return wrapHtmlFragmentForClipboard(html.toUtf8());
        }

        QByteArray fullHtml = html.toUtf8();
        const int fragmentStart = utf8ByteOffset(html, bodyOpenEnd + 1);
        fullHtml.insert(fragmentStart, kStartFragmentMarker);

        const int fragmentEnd = utf8ByteOffset(html, bodyEnd) +
                                static_cast<int>(std::strlen(kStartFragmentMarker));
        fullHtml.insert(fragmentEnd, kEndFragmentMarker);
        return fullHtml;
    }

    QByteArray buildClipboardHtmlPayload(const QString &html)
    {
        const QByteArray fullHtml = buildClipboardHtmlDocument(html);

        QByteArray header =
            "Version:0.9\r\n"
            "StartHTML:0000000000\r\n"
            "EndHTML:0000000000\r\n"
            "StartFragment:0000000000\r\n"
            "EndFragment:0000000000\r\n";

        const int startHtml = header.size();
        const int endHtml = startHtml + fullHtml.size();
        const int fragmentMarkerPos = fullHtml.indexOf(kStartFragmentMarker);
        const int endMarkerPos = fullHtml.indexOf(kEndFragmentMarker);
        if (fragmentMarkerPos < 0 || endMarkerPos < 0 || endMarkerPos < fragmentMarkerPos)
        {
            return {};
        }
        const int startFragment = startHtml + fragmentMarkerPos + static_cast<int>(std::strlen(kStartFragmentMarker));
        const int endFragment = startHtml + endMarkerPos;

        const auto patchOffset = [&header](const char *label, int value)
        {
            const int pos = header.indexOf(label);
            if (pos < 0)
            {
                return;
            }

            const QByteArray digits = QByteArray::number(value).rightJustified(10, '0');
            header.replace(pos + static_cast<int>(std::strlen(label)), 10, digits);
        };

        patchOffset("StartHTML:", startHtml);
        patchOffset("EndHTML:", endHtml);
        patchOffset("StartFragment:", startFragment);
        patchOffset("EndFragment:", endFragment);
        return header + fullHtml;
    }

    HGLOBAL createByteHandle(const QByteArray &bytes, bool appendNullTerminator = false)
    {
        const SIZE_T totalBytes = static_cast<SIZE_T>(bytes.size()) + (appendNullTerminator ? 1 : 0);
        if (totalBytes == 0)
        {
            return nullptr;
        }

        return createGlobalHandle(totalBytes,
                                  [&bytes, appendNullTerminator](void *dest)
                                  {
                                      memcpy(dest, bytes.constData(), static_cast<size_t>(bytes.size()));
                                      if (appendNullTerminator)
                                      {
                                          static_cast<char *>(dest)[bytes.size()] = '\0';
                                      }
                                  });
    }

    HGLOBAL createUnicodeTextHandle(const QString &text)
    {
        std::wstring wide = normalizeWindowsText(text).toStdWString();
        wide.push_back(L'\0');
        return createGlobalHandle(static_cast<SIZE_T>(wide.size() * sizeof(wchar_t)),
                                  [&wide](void *dest)
                                  {
                                      memcpy(dest, wide.data(), wide.size() * sizeof(wchar_t));
                                  });
    }

    HGLOBAL createDibHandle(const QByteArray &pngBytes)
    {
        QImage image;
        if ((!image.loadFromData(pngBytes, "PNG") && !image.loadFromData(pngBytes)) || image.isNull())
        {
            return nullptr;
        }

        const QImage dib = image.convertToFormat(QImage::Format_ARGB32);
        const SIZE_T pixelBytes = static_cast<SIZE_T>(dib.sizeInBytes());
        const SIZE_T totalBytes = sizeof(BITMAPINFOHEADER) + pixelBytes;
        return createGlobalHandle(totalBytes,
                                  [&dib, pixelBytes](void *dest)
                                  {
                                      BITMAPINFOHEADER header{};
                                      header.biSize = sizeof(BITMAPINFOHEADER);
                                      header.biWidth = dib.width();
                                      header.biHeight = -dib.height();
                                      header.biPlanes = 1;
                                      header.biBitCount = 32;
                                      header.biCompression = BI_RGB;
                                      header.biSizeImage = static_cast<DWORD>(pixelBytes);
                                      memcpy(dest, &header, sizeof(header));
                                      memcpy(static_cast<unsigned char *>(dest) + sizeof(header),
                                             dib.constBits(),
                                             pixelBytes);
                                  });
    }

    struct ClipboardFormats
    {
        UINT fileDescriptorW = RegisterClipboardFormatA(CFSTR_FILEDESCRIPTORW);
        UINT fileContents = RegisterClipboardFormatA(CFSTR_FILECONTENTS);
        UINT preferredDropEffect = RegisterClipboardFormatA(CFSTR_PREFERREDDROPEFFECT);
        UINT html = RegisterClipboardFormatA(kClipboardHtmlFormatName);
        UINT png = RegisterClipboardFormatA(kClipboardPngFormatName);
    };

    const ClipboardFormats &clipboardFormats()
    {
        static const ClipboardFormats formats;
        return formats;
    }

    class ProviderFileStream : public IStream
    {
    public:
        ProviderFileStream(quint64 sessionId,
                           ClipboardVirtualFileInfo file,
                           IVirtualFileProvider *provider)
            : m_sessionId(sessionId), m_file(std::move(file)), m_provider(provider)
        {
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override
        {
            if (!ppvObject)
            {
                return E_POINTER;
            }

            *ppvObject = nullptr;
            if (IsEqualIID(riid, IID_IUnknown) ||
                IsEqualIID(riid, IID_ISequentialStream) ||
                IsEqualIID(riid, IID_IStream))
            {
                *ppvObject = static_cast<IStream *>(this);
                AddRef();
                return S_OK;
            }

            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG refs = static_cast<ULONG>(InterlockedDecrement(&m_refCount));
            if (refs == 0)
            {
                delete this;
            }
            return refs;
        }

        HRESULT STDMETHODCALLTYPE Read(void *pv, ULONG cb, ULONG *pcbRead) override
        {
            if (pcbRead)
            {
                *pcbRead = 0;
            }
            if (!pv)
            {
                return STG_E_INVALIDPOINTER;
            }

            const qint64 available = qMax<qint64>(0, m_file.size - static_cast<qint64>(m_position));
            const qint64 toRead = qMin<qint64>(available, static_cast<qint64>(cb));
            if (toRead <= 0)
            {
                return S_FALSE;
            }

            ClipboardVirtualFileRangeRequest request;
            request.sessionId = m_sessionId;
            request.fileId = m_file.fileId;
            request.offset = static_cast<qint64>(m_position);
            request.length = toRead;

            bool ok = false;
            const QByteArray bytes = m_provider ? m_provider->readFileRange(request, &ok) : QByteArray();
            if (!ok)
            {
                return STG_E_READFAULT;
            }

            const ULONG copied = static_cast<ULONG>(qMin<qint64>(bytes.size(), toRead));
            if (copied > 0)
            {
                memcpy(pv, bytes.constData(), copied);
                m_position += copied;
            }

            if (pcbRead)
            {
                *pcbRead = copied;
            }

            return copied == cb ? S_OK : S_FALSE;
        }

        HRESULT STDMETHODCALLTYPE Write(const void *, ULONG, ULONG *) override
        {
            return STG_E_ACCESSDENIED;
        }

        HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove,
                                       DWORD dwOrigin,
                                       ULARGE_INTEGER *plibNewPosition) override
        {
            LONGLONG base = 0;
            switch (dwOrigin)
            {
            case STREAM_SEEK_SET:
                base = 0;
                break;
            case STREAM_SEEK_CUR:
                base = static_cast<LONGLONG>(m_position);
                break;
            case STREAM_SEEK_END:
                base = m_file.size;
                break;
            default:
                return STG_E_INVALIDFUNCTION;
            }

            const LONGLONG next = base + dlibMove.QuadPart;
            if (next < 0)
            {
                return STG_E_INVALIDFUNCTION;
            }

            m_position = static_cast<ULONGLONG>(next);
            if (plibNewPosition)
            {
                plibNewPosition->QuadPart = m_position;
            }

            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
        HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_REVERTED; }
        HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
        HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }

        HRESULT STDMETHODCALLTYPE CopyTo(IStream *pstm,
                                         ULARGE_INTEGER cb,
                                         ULARGE_INTEGER *pcbRead,
                                         ULARGE_INTEGER *pcbWritten) override;
        HRESULT STDMETHODCALLTYPE Stat(STATSTG *pstatstg, DWORD grfStatFlag) override;
        HRESULT STDMETHODCALLTYPE Clone(IStream **ppstm) override;

    private:
        LONG m_refCount = 1;
        quint64 m_sessionId = 0;
        ClipboardVirtualFileInfo m_file;
        IVirtualFileProvider *m_provider = nullptr;
        ULONGLONG m_position = 0;
    };

    class FormatEtcEnumerator : public IEnumFORMATETC
    {
    public:
        explicit FormatEtcEnumerator(std::vector<FORMATETC> formats)
            : m_formats(std::move(formats))
        {
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;
        HRESULT STDMETHODCALLTYPE Next(ULONG celt, FORMATETC *rgelt, ULONG *pceltFetched) override;
        HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override;
        HRESULT STDMETHODCALLTYPE Reset() override;
        HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC **ppenum) override;

    private:
        LONG m_refCount = 1;
        std::vector<FORMATETC> m_formats;
        std::size_t m_index = 0;
    };

    HGLOBAL createDropEffectHandle();
    HGLOBAL createFileGroupDescriptorHandle(const QVector<ClipboardVirtualFileInfo> &files);

    class VirtualFileDataObject : public IDataObject
    {
    public:
        VirtualFileDataObject(quint64 sessionId,
                              clipboard::Snapshot snapshot,
                              QVector<ClipboardVirtualFileInfo> files,
                              IVirtualFileProvider *provider);

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;
        HRESULT STDMETHODCALLTYPE GetData(FORMATETC *pformatetcIn, STGMEDIUM *pmedium) override;
        HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override;
        HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *pformatetc) override;
        HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *pformatetcOut) override;
        HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override;
        HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC **ppenumFormatEtc) override;
        HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override;
        HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override;
        HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override;

    private:
        bool supportsFormat(CLIPFORMAT format) const;

        LONG m_refCount = 1;
        quint64 m_sessionId = 0;
        clipboard::Snapshot m_snapshot;
        QVector<ClipboardVirtualFileInfo> m_files;
        IVirtualFileProvider *m_provider = nullptr;
        FORMATETC m_unicodeTextFormat{};
        FORMATETC m_htmlFormat{};
        FORMATETC m_pngFormat{};
        FORMATETC m_dibFormat{};
        FORMATETC m_fileDescriptorFormat{};
        FORMATETC m_fileContentsFormat{};
        FORMATETC m_dropEffectFormat{};
    };
}

#endif

bool publishWindowsVirtualFiles(const ClipboardWriteRequest &request)
{
#ifdef Q_OS_WIN
    if (!request.virtualFileProvider || request.snapshot.files.isEmpty() || !ensureOleInitialized())
    {
        return false;
    }

    const QVector<ClipboardVirtualFileInfo> files =
        request.virtualFileProvider->describeFiles(request.sessionId, request.snapshot.files);
    if (files.size() != request.snapshot.files.size())
    {
        return false;
    }

    for (const ClipboardVirtualFileInfo &file : files)
    {
        if (file.fileId.isEmpty() || file.displayName.isEmpty() || file.size < 0)
        {
            return false;
        }
    }

    auto *dataObject = new VirtualFileDataObject(request.sessionId,
                                                 request.snapshot,
                                                 files,
                                                 request.virtualFileProvider);
    const HRESULT hr = OleSetClipboard(dataObject);
    dataObject->Release();
    return SUCCEEDED(hr);
#else
    Q_UNUSED(request)
    return false;
#endif
}

#ifdef Q_OS_WIN

namespace
{
    HRESULT ProviderFileStream::CopyTo(IStream *pstm,
                                       ULARGE_INTEGER cb,
                                       ULARGE_INTEGER *pcbRead,
                                       ULARGE_INTEGER *pcbWritten)
    {
        if (!pstm)
        {
            return STG_E_INVALIDPOINTER;
        }

        if (pcbRead)
        {
            pcbRead->QuadPart = 0;
        }
        if (pcbWritten)
        {
            pcbWritten->QuadPart = 0;
        }

        QByteArray buffer;
        buffer.resize(64 * 1024);
        ULONGLONG remaining = cb.QuadPart;
        while (remaining > 0)
        {
            const ULONG chunkSize = static_cast<ULONG>(qMin<ULONGLONG>(
                remaining, static_cast<ULONGLONG>(buffer.size())));
            ULONG justRead = 0;
            const HRESULT hrRead = Read(buffer.data(), chunkSize, &justRead);
            if (FAILED(hrRead))
            {
                return hrRead;
            }
            if (justRead == 0)
            {
                break;
            }

            ULONG justWritten = 0;
            const HRESULT hrWrite = pstm->Write(buffer.constData(), justRead, &justWritten);
            if (FAILED(hrWrite))
            {
                return hrWrite;
            }

            if (pcbRead)
            {
                pcbRead->QuadPart += justRead;
            }
            if (pcbWritten)
            {
                pcbWritten->QuadPart += justWritten;
            }

            remaining -= justRead;
            if (hrRead == S_FALSE)
            {
                break;
            }
        }

        return S_OK;
    }

    HRESULT ProviderFileStream::Stat(STATSTG *pstatstg, DWORD) 
    {
        if (!pstatstg)
        {
            return STG_E_INVALIDPOINTER;
        }

        memset(pstatstg, 0, sizeof(STATSTG));
        pstatstg->type = STGTY_STREAM;
        pstatstg->cbSize.QuadPart = static_cast<ULONGLONG>(qMax<qint64>(0, m_file.size));
        pstatstg->grfMode = STGM_READ;
        return S_OK;
    }

    HRESULT ProviderFileStream::Clone(IStream **ppstm)
    {
        if (!ppstm)
        {
            return STG_E_INVALIDPOINTER;
        }

        auto *clone = new ProviderFileStream(m_sessionId, m_file, m_provider);
        clone->m_position = m_position;
        *ppstm = clone;
        return S_OK;
    }

    HRESULT FormatEtcEnumerator::QueryInterface(REFIID riid, void **ppvObject)
    {
        if (!ppvObject)
        {
            return E_POINTER;
        }

        *ppvObject = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumFORMATETC))
        {
            *ppvObject = static_cast<IEnumFORMATETC *>(this);
            AddRef();
            return S_OK;
        }

        return E_NOINTERFACE;
    }

    ULONG FormatEtcEnumerator::AddRef()
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
    }

    ULONG FormatEtcEnumerator::Release()
    {
        const ULONG refs = static_cast<ULONG>(InterlockedDecrement(&m_refCount));
        if (refs == 0)
        {
            delete this;
        }
        return refs;
    }

    HRESULT FormatEtcEnumerator::Next(ULONG celt, FORMATETC *rgelt, ULONG *pceltFetched)
    {
        if (!rgelt)
        {
            return E_POINTER;
        }

        if (pceltFetched)
        {
            *pceltFetched = 0;
        }

        ULONG fetched = 0;
        while (fetched < celt && m_index < m_formats.size())
        {
            rgelt[fetched] = m_formats[m_index];
            ++m_index;
            ++fetched;
        }

        if (pceltFetched)
        {
            *pceltFetched = fetched;
        }

        return fetched == celt ? S_OK : S_FALSE;
    }

    HRESULT FormatEtcEnumerator::Skip(ULONG celt)
    {
        m_index = qMin<std::size_t>(m_index + celt, m_formats.size());
        return m_index < m_formats.size() ? S_OK : S_FALSE;
    }

    HRESULT FormatEtcEnumerator::Reset()
    {
        m_index = 0;
        return S_OK;
    }

    HRESULT FormatEtcEnumerator::Clone(IEnumFORMATETC **ppenum)
    {
        if (!ppenum)
        {
            return E_POINTER;
        }

        auto *clone = new FormatEtcEnumerator(m_formats);
        clone->m_index = m_index;
        *ppenum = clone;
        return S_OK;
    }

    HGLOBAL createDropEffectHandle()
    {
        HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
        if (!handle)
        {
            return nullptr;
        }

        void *locked = GlobalLock(handle);
        if (!locked)
        {
            GlobalFree(handle);
            return nullptr;
        }

        *static_cast<DWORD *>(locked) = DROPEFFECT_COPY;
        GlobalUnlock(handle);
        return handle;
    }

    HGLOBAL createFileGroupDescriptorHandle(const QVector<ClipboardVirtualFileInfo> &files)
    {
        if (files.isEmpty())
        {
            return nullptr;
        }

        const SIZE_T bytes = sizeof(FILEGROUPDESCRIPTORW) +
                             sizeof(FILEDESCRIPTORW) * static_cast<SIZE_T>(files.size() - 1);
        HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (!handle)
        {
            return nullptr;
        }

        auto *group = static_cast<FILEGROUPDESCRIPTORW *>(GlobalLock(handle));
        if (!group)
        {
            GlobalFree(handle);
            return nullptr;
        }

        group->cItems = static_cast<UINT>(files.size());
        for (int i = 0; i < files.size(); ++i)
        {
            const ClipboardVirtualFileInfo &file = files.at(i);
            FILEDESCRIPTORW &descriptor = group->fgd[i];
            descriptor.dwFlags = FD_FILESIZE | FD_WRITESTIME | FD_PROGRESSUI;
            descriptor.ftLastWriteTime = unixMsToFileTime(file.mtimeMs);
            descriptor.nFileSizeHigh = static_cast<DWORD>((static_cast<quint64>(file.size) >> 32) & 0xFFFFFFFFULL);
            descriptor.nFileSizeLow = static_cast<DWORD>(static_cast<quint64>(file.size) & 0xFFFFFFFFULL);

            const QString displayName = file.displayName.isEmpty() ? file.fileId : file.displayName;
            const std::wstring wideName = displayName.left(MAX_PATH - 1).toStdWString();
            wcsncpy(descriptor.cFileName, wideName.c_str(), MAX_PATH - 1);
            descriptor.cFileName[MAX_PATH - 1] = L'\0';
        }

        GlobalUnlock(handle);
        return handle;
    }

    VirtualFileDataObject::VirtualFileDataObject(quint64 sessionId,
                                                 clipboard::Snapshot snapshot,
                                                 QVector<ClipboardVirtualFileInfo> files,
                                                 IVirtualFileProvider *provider)
        : m_sessionId(sessionId),
          m_snapshot(std::move(snapshot)),
          m_files(std::move(files)),
          m_provider(provider)
    {
        const ClipboardFormats &formats = clipboardFormats();

        m_unicodeTextFormat.cfFormat = CF_UNICODETEXT;
        m_unicodeTextFormat.dwAspect = DVASPECT_CONTENT;
        m_unicodeTextFormat.lindex = -1;
        m_unicodeTextFormat.ptd = nullptr;
        m_unicodeTextFormat.tymed = TYMED_HGLOBAL;

        m_htmlFormat.cfFormat = static_cast<CLIPFORMAT>(formats.html);
        m_htmlFormat.dwAspect = DVASPECT_CONTENT;
        m_htmlFormat.lindex = -1;
        m_htmlFormat.ptd = nullptr;
        m_htmlFormat.tymed = TYMED_HGLOBAL;

        m_pngFormat.cfFormat = static_cast<CLIPFORMAT>(formats.png);
        m_pngFormat.dwAspect = DVASPECT_CONTENT;
        m_pngFormat.lindex = -1;
        m_pngFormat.ptd = nullptr;
        m_pngFormat.tymed = TYMED_HGLOBAL;

        m_dibFormat.cfFormat = CF_DIB;
        m_dibFormat.dwAspect = DVASPECT_CONTENT;
        m_dibFormat.lindex = -1;
        m_dibFormat.ptd = nullptr;
        m_dibFormat.tymed = TYMED_HGLOBAL;

        m_fileDescriptorFormat.cfFormat = static_cast<CLIPFORMAT>(formats.fileDescriptorW);
        m_fileDescriptorFormat.dwAspect = DVASPECT_CONTENT;
        m_fileDescriptorFormat.lindex = -1;
        m_fileDescriptorFormat.ptd = nullptr;
        m_fileDescriptorFormat.tymed = TYMED_HGLOBAL;

        m_fileContentsFormat.cfFormat = static_cast<CLIPFORMAT>(formats.fileContents);
        m_fileContentsFormat.dwAspect = DVASPECT_CONTENT;
        m_fileContentsFormat.lindex = 0;
        m_fileContentsFormat.ptd = nullptr;
        m_fileContentsFormat.tymed = TYMED_ISTREAM;

        m_dropEffectFormat.cfFormat = static_cast<CLIPFORMAT>(formats.preferredDropEffect);
        m_dropEffectFormat.dwAspect = DVASPECT_CONTENT;
        m_dropEffectFormat.lindex = -1;
        m_dropEffectFormat.ptd = nullptr;
        m_dropEffectFormat.tymed = TYMED_HGLOBAL;
    }

    bool VirtualFileDataObject::supportsFormat(CLIPFORMAT format) const
    {
        if (format == m_unicodeTextFormat.cfFormat)
        {
            return m_snapshot.hasText();
        }
        if (format == m_htmlFormat.cfFormat)
        {
            return m_htmlFormat.cfFormat != 0 && m_snapshot.hasHtml();
        }
        if (format == m_pngFormat.cfFormat || format == m_dibFormat.cfFormat)
        {
            if (format == m_pngFormat.cfFormat)
            {
                return m_pngFormat.cfFormat != 0 && m_snapshot.hasImage();
            }
            return m_snapshot.hasImage();
        }
        if (format == m_fileDescriptorFormat.cfFormat ||
            format == m_fileContentsFormat.cfFormat ||
            format == m_dropEffectFormat.cfFormat)
        {
            return !m_files.isEmpty();
        }

        return false;
    }

    HRESULT VirtualFileDataObject::QueryInterface(REFIID riid, void **ppvObject)
    {
        if (!ppvObject)
        {
            return E_POINTER;
        }

        *ppvObject = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDataObject))
        {
            *ppvObject = static_cast<IDataObject *>(this);
            AddRef();
            return S_OK;
        }

        return E_NOINTERFACE;
    }

    ULONG VirtualFileDataObject::AddRef()
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
    }

    ULONG VirtualFileDataObject::Release()
    {
        const ULONG refs = static_cast<ULONG>(InterlockedDecrement(&m_refCount));
        if (refs == 0)
        {
            delete this;
        }
        return refs;
    }

    HRESULT VirtualFileDataObject::GetData(FORMATETC *pformatetcIn, STGMEDIUM *pmedium)
    {
        if (!pformatetcIn || !pmedium)
        {
            return E_INVALIDARG;
        }

        memset(pmedium, 0, sizeof(STGMEDIUM));
        if (pformatetcIn->cfFormat == m_unicodeTextFormat.cfFormat &&
            (pformatetcIn->tymed & TYMED_HGLOBAL) &&
            m_snapshot.hasText())
        {
            HGLOBAL handle = createUnicodeTextHandle(m_snapshot.text);
            if (!handle)
            {
                return E_OUTOFMEMORY;
            }
            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = handle;
            return S_OK;
        }

        if (pformatetcIn->cfFormat == m_htmlFormat.cfFormat &&
            (pformatetcIn->tymed & TYMED_HGLOBAL) &&
            m_snapshot.hasHtml())
        {
            HGLOBAL handle = createByteHandle(buildClipboardHtmlPayload(m_snapshot.html), true);
            if (!handle)
            {
                return E_OUTOFMEMORY;
            }
            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = handle;
            return S_OK;
        }

        if (pformatetcIn->cfFormat == m_pngFormat.cfFormat &&
            (pformatetcIn->tymed & TYMED_HGLOBAL) &&
            m_snapshot.hasImage())
        {
            HGLOBAL handle = createByteHandle(m_snapshot.imagePng);
            if (!handle)
            {
                return E_OUTOFMEMORY;
            }
            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = handle;
            return S_OK;
        }

        if (pformatetcIn->cfFormat == m_dibFormat.cfFormat &&
            (pformatetcIn->tymed & TYMED_HGLOBAL) &&
            m_snapshot.hasImage())
        {
            HGLOBAL handle = createDibHandle(m_snapshot.imagePng);
            if (!handle)
            {
                return E_OUTOFMEMORY;
            }
            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = handle;
            return S_OK;
        }

        if (pformatetcIn->cfFormat == m_fileDescriptorFormat.cfFormat &&
            (pformatetcIn->tymed & TYMED_HGLOBAL))
        {
            HGLOBAL handle = createFileGroupDescriptorHandle(m_files);
            if (!handle)
            {
                return E_OUTOFMEMORY;
            }
            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = handle;
            return S_OK;
        }

        if (pformatetcIn->cfFormat == m_dropEffectFormat.cfFormat &&
            (pformatetcIn->tymed & TYMED_HGLOBAL))
        {
            HGLOBAL handle = createDropEffectHandle();
            if (!handle)
            {
                return E_OUTOFMEMORY;
            }
            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = handle;
            return S_OK;
        }

        if (pformatetcIn->cfFormat == m_fileContentsFormat.cfFormat &&
            (pformatetcIn->tymed & TYMED_ISTREAM))
        {
            const LONG index = pformatetcIn->lindex;
            if (index < 0 || index >= m_files.size())
            {
                return DV_E_LINDEX;
            }

            auto *stream = new ProviderFileStream(m_sessionId, m_files.at(index), m_provider);
            pmedium->tymed = TYMED_ISTREAM;
            pmedium->pstm = stream;
            return S_OK;
        }

        return DV_E_FORMATETC;
    }

    HRESULT VirtualFileDataObject::GetDataHere(FORMATETC *, STGMEDIUM *)
    {
        return DATA_E_FORMATETC;
    }

    HRESULT VirtualFileDataObject::QueryGetData(FORMATETC *pformatetc)
    {
        if (!pformatetc)
        {
            return E_INVALIDARG;
        }

        if (pformatetc->dwAspect != DVASPECT_CONTENT)
        {
            return DV_E_DVASPECT;
        }

        if (!supportsFormat(pformatetc->cfFormat))
        {
            return DV_E_FORMATETC;
        }

        if ((pformatetc->cfFormat == m_unicodeTextFormat.cfFormat ||
             pformatetc->cfFormat == m_htmlFormat.cfFormat ||
             pformatetc->cfFormat == m_pngFormat.cfFormat ||
             pformatetc->cfFormat == m_dibFormat.cfFormat ||
             pformatetc->cfFormat == m_fileDescriptorFormat.cfFormat ||
             pformatetc->cfFormat == m_dropEffectFormat.cfFormat) &&
            (pformatetc->tymed & TYMED_HGLOBAL))
        {
            return S_OK;
        }

        if (pformatetc->cfFormat == m_fileContentsFormat.cfFormat &&
            (pformatetc->tymed & TYMED_ISTREAM))
        {
            return (pformatetc->lindex >= 0 && pformatetc->lindex < m_files.size())
                       ? S_OK
                       : DV_E_LINDEX;
        }

        return DV_E_FORMATETC;
    }

    HRESULT VirtualFileDataObject::GetCanonicalFormatEtc(FORMATETC *, FORMATETC *pformatetcOut)
    {
        if (pformatetcOut)
        {
            pformatetcOut->ptd = nullptr;
        }
        return E_NOTIMPL;
    }

    HRESULT VirtualFileDataObject::SetData(FORMATETC *, STGMEDIUM *, BOOL)
    {
        return E_NOTIMPL;
    }

    HRESULT VirtualFileDataObject::EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC **ppenumFormatEtc)
    {
        if (!ppenumFormatEtc)
        {
            return E_POINTER;
        }

        *ppenumFormatEtc = nullptr;
        if (dwDirection != DATADIR_GET)
        {
            return E_NOTIMPL;
        }

        std::vector<FORMATETC> formats;
        if (m_snapshot.hasText())
        {
            formats.push_back(m_unicodeTextFormat);
        }
        if (m_snapshot.hasHtml())
        {
            if (m_htmlFormat.cfFormat != 0)
            {
                formats.push_back(m_htmlFormat);
            }
        }
        if (m_snapshot.hasImage())
        {
            if (m_pngFormat.cfFormat != 0)
            {
                formats.push_back(m_pngFormat);
            }
            formats.push_back(m_dibFormat);
        }
        if (!m_files.isEmpty())
        {
            formats.push_back(m_fileDescriptorFormat);
            formats.push_back(m_fileContentsFormat);
            formats.push_back(m_dropEffectFormat);
        }
        *ppenumFormatEtc = new FormatEtcEnumerator(std::move(formats));
        return S_OK;
    }

    HRESULT VirtualFileDataObject::DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *)
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT VirtualFileDataObject::DUnadvise(DWORD)
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT VirtualFileDataObject::EnumDAdvise(IEnumSTATDATA **)
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }
}

#endif
