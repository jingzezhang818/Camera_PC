#include "directshowpreviewsession.h"

#include <QEvent>
#include <QMetaObject>
#include <QPointer>
#include <QStringList>
#include <QWidget>

#include <atomic>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dshow.h>
#include <d3d9.h>
#include <evr.h>
#include <mfapi.h>
#include <mfidl.h>
#include <vmr9.h>
#endif

namespace {

#ifdef Q_OS_WIN

struct VideoInfoHeader2Compat
{
    RECT rcSource;
    RECT rcTarget;
    DWORD dwBitRate;
    DWORD dwBitErrorRate;
    REFERENCE_TIME AvgTimePerFrame;
    DWORD dwInterlaceFlags;
    DWORD dwCopyProtectFlags;
    DWORD dwPictAspectRatioX;
    DWORD dwPictAspectRatioY;
    DWORD dwControlFlags;
    DWORD dwReserved2;
    BITMAPINFOHEADER bmiHeader;
};

#ifndef MIDL_INTERFACE
#define MIDL_INTERFACE(x) struct __declspec(uuid(x)) __declspec(novtable)
#endif

static const CLSID CLSID_SampleGrabberCompat =
{0xC1F400A0, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
static const CLSID CLSID_NullRendererCompat =
{0xC1F400A4, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
static const IID IID_ISampleGrabberCompat =
{0x6B652FFF, 0x11FE, 0x4FCE, {0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F}};
static const IID IID_ISampleGrabberCBCompat =
{0x0579154A, 0x2B53, 0x4994, {0xB0, 0xD0, 0xE7, 0x73, 0x14, 0x8E, 0xFF, 0x85}};

MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
ISampleGrabberCBCompat : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double sampleTime, IMediaSample *sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double sampleTime, BYTE *buffer, long bufferLen) = 0;
};

MIDL_INTERFACE("6B652FFF-11FE-4FCE-92AD-0266B5D7C78F")
ISampleGrabberCompat : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL oneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE *mediaType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE *mediaType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL bufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long *bufferSize, long *buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample **sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCBCompat *callback, long whichMethodToCallback) = 0;
};

template <typename T>
void releaseCom(T *&ptr)
{
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

QString hrText(HRESULT hr)
{
    return QStringLiteral("0x%1")
            .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

double fpsFromAvgTimePerFrame(LONGLONG avgTimePerFrame)
{
    return avgTimePerFrame > 0
            ? (10000000.0 / static_cast<double>(avgTimePerFrame))
            : 0.0;
}

bool fpsAlmostEqual(double lhs, double rhs)
{
    const double delta = lhs - rhs;
    return delta > -0.10 && delta < 0.10;
}

void freeMediaTypeFields(AM_MEDIA_TYPE &mediaType)
{
    if (mediaType.cbFormat != 0 && mediaType.pbFormat != nullptr) {
        CoTaskMemFree(mediaType.pbFormat);
        mediaType.cbFormat = 0;
        mediaType.pbFormat = nullptr;
    }
    if (mediaType.pUnk != nullptr) {
        mediaType.pUnk->Release();
        mediaType.pUnk = nullptr;
    }
}

void deleteMediaType(AM_MEDIA_TYPE *mediaType)
{
    if (!mediaType) {
        return;
    }
    freeMediaTypeFields(*mediaType);
    CoTaskMemFree(mediaType);
}

QString readPropertyBagString(IPropertyBag *bag, const wchar_t *propertyName)
{
    if (!bag || !propertyName) {
        return QString();
    }

    VARIANT variant;
    VariantInit(&variant);
    const HRESULT hr = bag->Read(propertyName, &variant, nullptr);
    if (FAILED(hr) || variant.vt != VT_BSTR || variant.bstrVal == nullptr) {
        VariantClear(&variant);
        return QString();
    }

    const QString out = QString::fromWCharArray(variant.bstrVal);
    VariantClear(&variant);
    return out;
}

QString fourccFromSubtype(const GUID &subtype)
{
    const bool isFourccGuid =
            subtype.Data2 == 0x0000 &&
            subtype.Data3 == 0x0010 &&
            subtype.Data4[0] == 0x80 &&
            subtype.Data4[1] == 0x00 &&
            subtype.Data4[2] == 0x00 &&
            subtype.Data4[3] == 0xAA &&
            subtype.Data4[4] == 0x00 &&
            subtype.Data4[5] == 0x38 &&
            subtype.Data4[6] == 0x9B &&
            subtype.Data4[7] == 0x71;
    if (!isFourccGuid) {
        return QString();
    }

    const quint32 value = subtype.Data1;
    const char chars[5] = {
        static_cast<char>(value & 0xFF),
        static_cast<char>((value >> 8) & 0xFF),
        static_cast<char>((value >> 16) & 0xFF),
        static_cast<char>((value >> 24) & 0xFF),
        '\0'
    };

    bool printable = true;
    for (int i = 0; i < 4; ++i) {
        const unsigned char c = static_cast<unsigned char>(chars[i]);
        if (c < 32 || c > 126) {
            printable = false;
            break;
        }
    }
    return printable ? QString::fromLatin1(chars, 4) : QString();
}

QString subtypeToLabel(const GUID &subtype)
{
    if (subtype == MEDIASUBTYPE_YUY2) return QStringLiteral("YUY2");
    if (subtype == MEDIASUBTYPE_UYVY) return QStringLiteral("UYVY");
    if (subtype == MEDIASUBTYPE_NV12) return QStringLiteral("NV12");
    if (subtype == MEDIASUBTYPE_YV12) return QStringLiteral("YV12");
    if (subtype == MEDIASUBTYPE_MJPG) return QStringLiteral("MJPG");
    if (subtype == MEDIASUBTYPE_RGB24) return QStringLiteral("RGB24");
    if (subtype == MEDIASUBTYPE_RGB32) return QStringLiteral("RGB32");
    if (subtype == MEDIASUBTYPE_ARGB32) return QStringLiteral("ARGB32");

    const QString fourcc = fourccFromSubtype(subtype);
    return fourcc.isEmpty() ? QStringLiteral("UNKNOWN") : fourcc;
}

QString normalizeFormatTag(QString tag)
{
    tag = tag.trimmed().toUpper();
    if (tag == QLatin1String("YUYV")) {
        return QStringLiteral("YUY2");
    }
    if (tag == QLatin1String("JPEG") || tag == QLatin1String("JPG") ||
        tag == QLatin1String("MJPEG")) {
        return QStringLiteral("MJPG");
    }
    return tag;
}

QString formatTagFromPixelFormat(QVideoFrame::PixelFormat format)
{
    switch (format) {
    case QVideoFrame::Format_YUYV: return QStringLiteral("YUY2");
    case QVideoFrame::Format_UYVY: return QStringLiteral("UYVY");
    case QVideoFrame::Format_NV12: return QStringLiteral("NV12");
    case QVideoFrame::Format_YV12: return QStringLiteral("YV12");
    case QVideoFrame::Format_Jpeg: return QStringLiteral("MJPG");
    case QVideoFrame::Format_RGB24: return QStringLiteral("RGB24");
    case QVideoFrame::Format_RGB32: return QStringLiteral("RGB32");
    case QVideoFrame::Format_ARGB32: return QStringLiteral("ARGB32");
    default: return QString();
    }
}

QVideoFrame::PixelFormat pixelFormatFromFormatTag(const QString &tag)
{
    const QString normalized = normalizeFormatTag(tag);
    if (normalized == QLatin1String("YUY2")) return QVideoFrame::Format_YUYV;
    if (normalized == QLatin1String("UYVY")) return QVideoFrame::Format_UYVY;
    if (normalized == QLatin1String("NV12")) return QVideoFrame::Format_NV12;
    if (normalized == QLatin1String("YV12")) return QVideoFrame::Format_YV12;
    if (normalized == QLatin1String("MJPG")) return QVideoFrame::Format_Jpeg;
    if (normalized == QLatin1String("RGB24")) return QVideoFrame::Format_RGB24;
    if (normalized == QLatin1String("RGB32")) return QVideoFrame::Format_RGB32;
    if (normalized == QLatin1String("ARGB32")) return QVideoFrame::Format_ARGB32;
    return QVideoFrame::Format_Invalid;
}

int bytesPerLineForFormat(const QString &formatTag, int width, int bitCount)
{
    const QString normalized = normalizeFormatTag(formatTag);
    if (width <= 0) {
        return 0;
    }
    if (normalized == QLatin1String("YUY2") ||
        normalized == QLatin1String("UYVY")) {
        return width * 2;
    }
    if (normalized == QLatin1String("MJPG")) {
        return 0;
    }
    if (bitCount > 0) {
        return ((width * bitCount + 31) / 32) * 4;
    }
    return 0;
}

struct MediaDetails
{
    int width = 0;
    int height = 0;
    int bitCount = 0;
    int bytesPerLine = 0;
    double fps = 0.0;
    QString formatTag;
    GUID subtype = GUID_NULL;
};

bool extractMediaDetails(const AM_MEDIA_TYPE *mediaType, MediaDetails &details)
{
    if (!mediaType || !mediaType->pbFormat) {
        return false;
    }

    BITMAPINFOHEADER bmi = {};
    LONGLONG avgTimePerFrame = 0;

    if (mediaType->formattype == FORMAT_VideoInfo &&
        mediaType->cbFormat >= static_cast<ULONG>(sizeof(VIDEOINFOHEADER))) {
        const VIDEOINFOHEADER *vih =
                reinterpret_cast<const VIDEOINFOHEADER *>(mediaType->pbFormat);
        bmi = vih->bmiHeader;
        avgTimePerFrame = vih->AvgTimePerFrame;
    } else if (mediaType->formattype == FORMAT_VideoInfo2 &&
               mediaType->cbFormat >= static_cast<ULONG>(sizeof(VideoInfoHeader2Compat))) {
        const VideoInfoHeader2Compat *vih2 =
                reinterpret_cast<const VideoInfoHeader2Compat *>(mediaType->pbFormat);
        bmi = vih2->bmiHeader;
        avgTimePerFrame = vih2->AvgTimePerFrame;
    } else {
        return false;
    }

    details.width = static_cast<int>(bmi.biWidth);
    details.height = qAbs(static_cast<int>(bmi.biHeight));
    details.bitCount = static_cast<int>(bmi.biBitCount);
    details.fps = fpsFromAvgTimePerFrame(avgTimePerFrame);
    details.formatTag = subtypeToLabel(mediaType->subtype);
    details.subtype = mediaType->subtype;
    details.bytesPerLine = bytesPerLineForFormat(details.formatTag,
                                                 details.width,
                                                 details.bitCount);
    return details.width > 0 && details.height > 0;
}

#endif // Q_OS_WIN

} // namespace

struct DirectShowPreviewSessionPrivate
{
    explicit DirectShowPreviewSessionPrivate(DirectShowPreviewSession *owner)
        : q(owner)
    {
    }

    DirectShowPreviewSession *q = nullptr;
    QPointer<QWidget> previewWidget;
    CameraModeInfo mode;
    bool running = false;
    bool rawCaptureActive = false;

    std::atomic_bool rawDeliveryEnabled { false };
    std::atomic_bool oneRawFrameRequested { false };
    std::atomic_bool firstFramePending { true };

#ifdef Q_OS_WIN
    class SampleGrabberCallback final : public ISampleGrabberCBCompat
    {
    public:
        explicit SampleGrabberCallback(DirectShowPreviewSessionPrivate *owner)
            : m_owner(owner)
        {
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
        {
            if (!object) {
                return E_POINTER;
            }
            if (riid == IID_IUnknown || riid == IID_ISampleGrabberCBCompat) {
                *object = static_cast<ISampleGrabberCBCompat *>(this);
                AddRef();
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return static_cast<ULONG>(++m_refCount);
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG value = static_cast<ULONG>(--m_refCount);
            if (value == 0) {
                delete this;
            }
            return value;
        }

        HRESULT STDMETHODCALLTYPE SampleCB(double sampleTime, IMediaSample *sample) override
        {
            Q_UNUSED(sampleTime);
            Q_UNUSED(sample);
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE BufferCB(double sampleTime, BYTE *buffer, long bufferLen) override
        {
            if (m_owner) {
                m_owner->handleSample(sampleTime, buffer, bufferLen);
            }
            return S_OK;
        }

    private:
        std::atomic_long m_refCount { 1 };
        DirectShowPreviewSessionPrivate *m_owner = nullptr;
    };

    bool coUninitialize = false;
    HWND previewHwnd = nullptr;
    QSize frameSize;
    QVideoFrame::PixelFormat pixelFormat = QVideoFrame::Format_Invalid;
    QString formatTag;
    int bytesPerLine = 0;
    double configuredFps = 0.0;
    QString rendererName;

    IGraphBuilder *graph = nullptr;
    ICaptureGraphBuilder2 *builder = nullptr;
    IBaseFilter *sourceFilter = nullptr;
    IBaseFilter *rendererFilter = nullptr;
    IBaseFilter *sampleGrabberFilter = nullptr;
    IBaseFilter *nullRendererFilter = nullptr;
    ISampleGrabberCompat *sampleGrabber = nullptr;
    IMediaControl *mediaControl = nullptr;
    IMFVideoDisplayControl *evrDisplay = nullptr;
    IVMRWindowlessControl9 *vmrControl = nullptr;
    SampleGrabberCallback *sampleCallback = nullptr;

    void postLog(const QString &msg)
    {
        QMetaObject::invokeMethod(q,
                                  "deliverLogMessage",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, msg));
    }

    void postRawFailure(const QString &reason)
    {
        QMetaObject::invokeMethod(q,
                                  "deliverRawFailure",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, reason));
    }

    void postRawFrame(const CapturedFrame &frame)
    {
        QMetaObject::invokeMethod(q,
                                  "deliverRawFrame",
                                  Qt::QueuedConnection,
                                  Q_ARG(CapturedFrame, frame));
    }

    bool initializeCom(QString *reason)
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            coUninitialize = true;
            return true;
        }
        if (hr == RPC_E_CHANGED_MODE) {
            coUninitialize = false;
            return true;
        }
        if (reason) {
            *reason = QStringLiteral("CoInitializeEx failed: %1").arg(hrText(hr));
        }
        return false;
    }

    bool createGraph(QString *reason)
    {
        HRESULT hr = CoCreateInstance(CLSID_FilterGraph,
                                      nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_IGraphBuilder,
                                      reinterpret_cast<void **>(&graph));
        if (FAILED(hr) || !graph) {
            if (reason) {
                *reason = QStringLiteral("Create FilterGraph failed: %1").arg(hrText(hr));
            }
            return false;
        }

        hr = CoCreateInstance(CLSID_CaptureGraphBuilder2,
                              nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_ICaptureGraphBuilder2,
                              reinterpret_cast<void **>(&builder));
        if (FAILED(hr) || !builder) {
            if (reason) {
                *reason = QStringLiteral("Create CaptureGraphBuilder2 failed: %1").arg(hrText(hr));
            }
            return false;
        }

        hr = builder->SetFiltergraph(graph);
        if (FAILED(hr)) {
            if (reason) {
                *reason = QStringLiteral("SetFiltergraph failed: %1").arg(hrText(hr));
            }
            return false;
        }
        return true;
    }

    bool bindCameraSource(QString *reason)
    {
        ICreateDevEnum *devEnum = nullptr;
        IEnumMoniker *enumMoniker = nullptr;

        HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum,
                                      nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_ICreateDevEnum,
                                      reinterpret_cast<void **>(&devEnum));
        if (FAILED(hr) || !devEnum) {
            if (reason) {
                *reason = QStringLiteral("CreateDevEnum failed: %1").arg(hrText(hr));
            }
            releaseCom(devEnum);
            return false;
        }

        hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                            &enumMoniker,
                                            0);
        if (hr != S_OK || !enumMoniker) {
            if (reason) {
                *reason = QStringLiteral("No DirectShow video input device was found.");
            }
            releaseCom(enumMoniker);
            releaseCom(devEnum);
            return false;
        }

        const QString requestedDevicePath = mode.directShowDevicePath.isEmpty()
                ? mode.deviceName
                : mode.directShowDevicePath;
        const bool matchByPath = !requestedDevicePath.isEmpty();
        const int requestedIndex = mode.cameraIndex;

        int cameraIndex = -1;
        ULONG fetched = 0;
        IMoniker *moniker = nullptr;
        while (enumMoniker->Next(1, &moniker, &fetched) == S_OK) {
            ++cameraIndex;

            QString friendlyName;
            QString devicePath;
            IPropertyBag *propertyBag = nullptr;
            if (SUCCEEDED(moniker->BindToStorage(nullptr,
                                                 nullptr,
                                                 IID_IPropertyBag,
                                                 reinterpret_cast<void **>(&propertyBag))) &&
                propertyBag) {
                friendlyName = readPropertyBagString(propertyBag, L"FriendlyName");
                devicePath = readPropertyBagString(propertyBag, L"DevicePath");
                propertyBag->Release();
                propertyBag = nullptr;
            }

            const bool pathMatches = matchByPath &&
                    QString::compare(devicePath,
                                     requestedDevicePath,
                                     Qt::CaseInsensitive) == 0;
            const bool indexMatches = !matchByPath && cameraIndex == requestedIndex;
            if (!pathMatches && !indexMatches) {
                moniker->Release();
                moniker = nullptr;
                continue;
            }

            hr = moniker->BindToObject(nullptr,
                                       nullptr,
                                       IID_IBaseFilter,
                                       reinterpret_cast<void **>(&sourceFilter));
            moniker->Release();
            moniker = nullptr;
            if (FAILED(hr) || !sourceFilter) {
                if (reason) {
                    *reason = QStringLiteral("Bind camera source failed: %1").arg(hrText(hr));
                }
                releaseCom(enumMoniker);
                releaseCom(devEnum);
                return false;
            }

            const QString filterName = friendlyName.isEmpty()
                    ? mode.description
                    : friendlyName;
            hr = graph->AddFilter(sourceFilter,
                                  reinterpret_cast<LPCWSTR>(filterName.utf16()));
            if (FAILED(hr)) {
                if (reason) {
                    *reason = QStringLiteral("Add camera source filter failed: %1").arg(hrText(hr));
                }
                releaseCom(enumMoniker);
                releaseCom(devEnum);
                return false;
            }

            releaseCom(enumMoniker);
            releaseCom(devEnum);
            return true;
        }

        releaseCom(enumMoniker);
        releaseCom(devEnum);
        if (reason) {
            *reason = matchByPath
                    ? QStringLiteral("DirectShow camera not found by device path: %1").arg(requestedDevicePath)
                    : QStringLiteral("DirectShow camera index not found: %1").arg(requestedIndex);
        }
        return false;
    }

    QString requestedFormatTag() const
    {
        if (!mode.directShowFormatTag.isEmpty()) {
            return normalizeFormatTag(mode.directShowFormatTag);
        }
        return normalizeFormatTag(formatTagFromPixelFormat(mode.settings.pixelFormat()));
    }

    double requestedFps() const
    {
        if (mode.directShowFps > 0.0) {
            return mode.directShowFps;
        }
        const double minFps = mode.settings.minimumFrameRate();
        const double maxFps = mode.settings.maximumFrameRate();
        if (minFps > 0.0 && maxFps > 0.0 && fpsAlmostEqual(minFps, maxFps)) {
            return maxFps;
        }
        return 0.0;
    }

    bool configureExactMode(QString *reason)
    {
        IAMStreamConfig *streamConfig = nullptr;
        HRESULT hr = builder->FindInterface(&PIN_CATEGORY_CAPTURE,
                                            &MEDIATYPE_Video,
                                            sourceFilter,
                                            IID_IAMStreamConfig,
                                            reinterpret_cast<void **>(&streamConfig));
        if (FAILED(hr) || !streamConfig) {
            if (reason) {
                *reason = QStringLiteral("Find IAMStreamConfig failed: %1").arg(hrText(hr));
            }
            releaseCom(streamConfig);
            return false;
        }

        int capCount = 0;
        int capSize = 0;
        hr = streamConfig->GetNumberOfCapabilities(&capCount, &capSize);
        if (FAILED(hr) || capCount <= 0 || capSize <= 0) {
            if (reason) {
                *reason = QStringLiteral("GetNumberOfCapabilities failed: %1").arg(hrText(hr));
            }
            releaseCom(streamConfig);
            return false;
        }

        const QSize requestedSize = mode.settings.resolution();
        const QString requestedFormat = requestedFormatTag();
        const double fps = requestedFps();
        QStringList seen;

        QByteArray capBuffer(capSize, 0);
        for (int capIndex = 0; capIndex < capCount; ++capIndex) {
            AM_MEDIA_TYPE *mediaType = nullptr;
            hr = streamConfig->GetStreamCaps(capIndex,
                                             &mediaType,
                                             reinterpret_cast<BYTE *>(capBuffer.data()));
            if (FAILED(hr) || !mediaType) {
                continue;
            }

            MediaDetails details;
            const bool hasDetails = extractMediaDetails(mediaType, details);
            const QString normalizedFormat = normalizeFormatTag(details.formatTag);
            if (hasDetails) {
                seen << QStringLiteral("%1x%2 %3 fps=%4")
                        .arg(details.width)
                        .arg(details.height)
                        .arg(normalizedFormat)
                        .arg(details.fps, 0, 'f', 3);
            }

            const bool sizeMatches = requestedSize.width() <= 0 ||
                    (details.width == requestedSize.width() &&
                     details.height == requestedSize.height());
            const bool formatMatches = requestedFormat.isEmpty() ||
                    normalizedFormat == requestedFormat;
            const bool fpsMatches = fps <= 0.0 ||
                    (details.fps > 0.0 && fpsAlmostEqual(details.fps, fps));

            if (hasDetails && sizeMatches && formatMatches && fpsMatches) {
                hr = streamConfig->SetFormat(mediaType);
                if (FAILED(hr)) {
                    if (reason) {
                        *reason = QStringLiteral("SetFormat failed for %1x%2 %3 fps=%4: %5")
                                .arg(details.width)
                                .arg(details.height)
                                .arg(normalizedFormat)
                                .arg(details.fps, 0, 'f', 3)
                                .arg(hrText(hr));
                    }
                    deleteMediaType(mediaType);
                    releaseCom(streamConfig);
                    return false;
                }

                frameSize = QSize(details.width, details.height);
                pixelFormat = pixelFormatFromFormatTag(normalizedFormat);
                formatTag = normalizedFormat;
                bytesPerLine = details.bytesPerLine;
                configuredFps = details.fps;
                deleteMediaType(mediaType);
                releaseCom(streamConfig);
                return true;
            }

            deleteMediaType(mediaType);
        }

        releaseCom(streamConfig);
        if (reason) {
            *reason = QStringLiteral("Selected DirectShow mode was not found: %1x%2 %3 fps=%4. Enumerated examples: %5")
                    .arg(requestedSize.width())
                    .arg(requestedSize.height())
                    .arg(requestedFormat.isEmpty() ? QStringLiteral("any") : requestedFormat)
                    .arg(fps, 0, 'f', 3)
                    .arg(seen.mid(0, 12).join(QStringLiteral(" | ")));
        }
        return false;
    }

    bool setupEvrRenderer(QString *reason)
    {
        IBaseFilter *renderer = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_EnhancedVideoRenderer,
                                      nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_IBaseFilter,
                                      reinterpret_cast<void **>(&renderer));
        if (FAILED(hr) || !renderer) {
            if (reason) {
                *reason = QStringLiteral("Create EVR failed: %1").arg(hrText(hr));
            }
            releaseCom(renderer);
            return false;
        }

        hr = graph->AddFilter(renderer, L"EVR Preview Renderer");
        if (FAILED(hr)) {
            if (reason) {
                *reason = QStringLiteral("Add EVR filter failed: %1").arg(hrText(hr));
            }
            releaseCom(renderer);
            return false;
        }

        hr = builder->RenderStream(&PIN_CATEGORY_PREVIEW,
                                   &MEDIATYPE_Video,
                                   sourceFilter,
                                   nullptr,
                                   renderer);
        if (FAILED(hr)) {
            graph->RemoveFilter(renderer);
            if (reason) {
                *reason = QStringLiteral("Render preview stream through EVR failed: %1").arg(hrText(hr));
            }
            releaseCom(renderer);
            return false;
        }

        IMFGetService *service = nullptr;
        hr = renderer->QueryInterface(IID_IMFGetService,
                                      reinterpret_cast<void **>(&service));
        if (FAILED(hr) || !service) {
            graph->RemoveFilter(renderer);
            if (reason) {
                *reason = QStringLiteral("Query EVR IMFGetService failed: %1").arg(hrText(hr));
            }
            releaseCom(service);
            releaseCom(renderer);
            return false;
        }

        IMFVideoDisplayControl *display = nullptr;
        hr = service->GetService(MR_VIDEO_RENDER_SERVICE,
                                 IID_IMFVideoDisplayControl,
                                 reinterpret_cast<void **>(&display));
        releaseCom(service);
        if (FAILED(hr) || !display) {
            graph->RemoveFilter(renderer);
            if (reason) {
                *reason = QStringLiteral("Get EVR display control failed: %1").arg(hrText(hr));
            }
            releaseCom(display);
            releaseCom(renderer);
            return false;
        }

        hr = display->SetVideoWindow(previewHwnd);
        if (FAILED(hr)) {
            graph->RemoveFilter(renderer);
            if (reason) {
                *reason = QStringLiteral("EVR SetVideoWindow failed: %1").arg(hrText(hr));
            }
            releaseCom(display);
            releaseCom(renderer);
            return false;
        }
        display->SetAspectRatioMode(MFVideoARMode_PreservePicture);

        rendererFilter = renderer;
        evrDisplay = display;
        rendererName = QStringLiteral("EVR");
        return true;
    }

    bool setupVmr9Renderer(QString *reason)
    {
        IBaseFilter *renderer = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_VideoMixingRenderer9,
                                      nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_IBaseFilter,
                                      reinterpret_cast<void **>(&renderer));
        if (FAILED(hr) || !renderer) {
            if (reason) {
                *reason = QStringLiteral("Create VMR9 failed: %1").arg(hrText(hr));
            }
            releaseCom(renderer);
            return false;
        }

        IVMRFilterConfig9 *config = nullptr;
        hr = renderer->QueryInterface(IID_IVMRFilterConfig9,
                                      reinterpret_cast<void **>(&config));
        if (FAILED(hr) || !config) {
            if (reason) {
                *reason = QStringLiteral("Query VMR9 config failed: %1").arg(hrText(hr));
            }
            releaseCom(config);
            releaseCom(renderer);
            return false;
        }

        hr = config->SetRenderingMode(VMR9Mode_Windowless);
        releaseCom(config);
        if (FAILED(hr)) {
            if (reason) {
                *reason = QStringLiteral("VMR9 SetRenderingMode failed: %1").arg(hrText(hr));
            }
            releaseCom(renderer);
            return false;
        }

        hr = graph->AddFilter(renderer, L"VMR9 Preview Renderer");
        if (FAILED(hr)) {
            if (reason) {
                *reason = QStringLiteral("Add VMR9 filter failed: %1").arg(hrText(hr));
            }
            releaseCom(renderer);
            return false;
        }

        hr = builder->RenderStream(&PIN_CATEGORY_PREVIEW,
                                   &MEDIATYPE_Video,
                                   sourceFilter,
                                   nullptr,
                                   renderer);
        if (FAILED(hr)) {
            graph->RemoveFilter(renderer);
            if (reason) {
                *reason = QStringLiteral("Render preview stream through VMR9 failed: %1").arg(hrText(hr));
            }
            releaseCom(renderer);
            return false;
        }

        IVMRWindowlessControl9 *windowless = nullptr;
        hr = renderer->QueryInterface(IID_IVMRWindowlessControl9,
                                      reinterpret_cast<void **>(&windowless));
        if (FAILED(hr) || !windowless) {
            graph->RemoveFilter(renderer);
            if (reason) {
                *reason = QStringLiteral("Query VMR9 windowless control failed: %1").arg(hrText(hr));
            }
            releaseCom(windowless);
            releaseCom(renderer);
            return false;
        }

        hr = windowless->SetVideoClippingWindow(previewHwnd);
        if (FAILED(hr)) {
            graph->RemoveFilter(renderer);
            if (reason) {
                *reason = QStringLiteral("VMR9 SetVideoClippingWindow failed: %1").arg(hrText(hr));
            }
            releaseCom(windowless);
            releaseCom(renderer);
            return false;
        }
        windowless->SetAspectRatioMode(VMR9ARMode_LetterBox);

        rendererFilter = renderer;
        vmrControl = windowless;
        rendererName = QStringLiteral("VMR9");
        return true;
    }

    bool setupPreviewRenderer(QString *reason)
    {
        QString evrReason;
        if (setupEvrRenderer(&evrReason)) {
            return true;
        }
        postLog(QStringLiteral("EVR preview unavailable, trying VMR9. EVR reason: %1")
                .arg(evrReason));

        QString vmrReason;
        if (setupVmr9Renderer(&vmrReason)) {
            return true;
        }
        if (reason) {
            *reason = QStringLiteral("Preview renderer setup failed. EVR: %1; VMR9: %2")
                    .arg(evrReason, vmrReason);
        }
        return false;
    }

    bool setupRawBranch(QString *reason)
    {
        HRESULT hr = CoCreateInstance(CLSID_SampleGrabberCompat,
                                      nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_IBaseFilter,
                                      reinterpret_cast<void **>(&sampleGrabberFilter));
        if (FAILED(hr) || !sampleGrabberFilter) {
            if (reason) {
                *reason = QStringLiteral("Create SampleGrabber failed: %1").arg(hrText(hr));
            }
            releaseCom(sampleGrabberFilter);
            return false;
        }

        hr = sampleGrabberFilter->QueryInterface(IID_ISampleGrabberCompat,
                                                 reinterpret_cast<void **>(&sampleGrabber));
        if (FAILED(hr) || !sampleGrabber) {
            if (reason) {
                *reason = QStringLiteral("Query ISampleGrabber failed: %1").arg(hrText(hr));
            }
            releaseCom(sampleGrabber);
            releaseCom(sampleGrabberFilter);
            return false;
        }

        AM_MEDIA_TYPE grabberType = {};
        grabberType.majortype = MEDIATYPE_Video;
        grabberType.subtype = GUID_NULL;
        if (formatTag == QLatin1String("YUY2")) {
            grabberType.subtype = MEDIASUBTYPE_YUY2;
        } else if (formatTag == QLatin1String("UYVY")) {
            grabberType.subtype = MEDIASUBTYPE_UYVY;
        } else if (formatTag == QLatin1String("MJPG")) {
            grabberType.subtype = MEDIASUBTYPE_MJPG;
        } else if (formatTag == QLatin1String("NV12")) {
            grabberType.subtype = MEDIASUBTYPE_NV12;
        } else if (formatTag == QLatin1String("RGB24")) {
            grabberType.subtype = MEDIASUBTYPE_RGB24;
        } else if (formatTag == QLatin1String("RGB32")) {
            grabberType.subtype = MEDIASUBTYPE_RGB32;
        }
        if (grabberType.subtype != GUID_NULL) {
            sampleGrabber->SetMediaType(&grabberType);
        }

        sampleGrabber->SetOneShot(FALSE);
        sampleGrabber->SetBufferSamples(FALSE);

        sampleCallback = new SampleGrabberCallback(this);
        hr = sampleGrabber->SetCallback(sampleCallback, 1);
        if (FAILED(hr)) {
            if (reason) {
                *reason = QStringLiteral("Set SampleGrabber callback failed: %1").arg(hrText(hr));
            }
            sampleCallback->Release();
            sampleCallback = nullptr;
            releaseCom(sampleGrabber);
            releaseCom(sampleGrabberFilter);
            return false;
        }

        hr = graph->AddFilter(sampleGrabberFilter, L"Raw Frame SampleGrabber");
        if (FAILED(hr)) {
            if (reason) {
                *reason = QStringLiteral("Add SampleGrabber filter failed: %1").arg(hrText(hr));
            }
            sampleGrabber->SetCallback(nullptr, 0);
            sampleCallback->Release();
            sampleCallback = nullptr;
            releaseCom(sampleGrabber);
            releaseCom(sampleGrabberFilter);
            return false;
        }

        hr = CoCreateInstance(CLSID_NullRendererCompat,
                              nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_IBaseFilter,
                              reinterpret_cast<void **>(&nullRendererFilter));
        if (FAILED(hr) || !nullRendererFilter) {
            if (reason) {
                *reason = QStringLiteral("Create NullRenderer failed: %1").arg(hrText(hr));
            }
            return false;
        }

        hr = graph->AddFilter(nullRendererFilter, L"Raw Frame NullRenderer");
        if (FAILED(hr)) {
            if (reason) {
                *reason = QStringLiteral("Add NullRenderer filter failed: %1").arg(hrText(hr));
            }
            return false;
        }

        hr = builder->RenderStream(&PIN_CATEGORY_CAPTURE,
                                   &MEDIATYPE_Video,
                                   sourceFilter,
                                   sampleGrabberFilter,
                                   nullRendererFilter);
        if (FAILED(hr)) {
            if (reason) {
                *reason = QStringLiteral("Render raw capture branch failed: %1").arg(hrText(hr));
            }
            return false;
        }

        rawCaptureActive = true;
        return true;
    }

    void releaseRawBranch()
    {
        rawCaptureActive = false;
        if (sampleGrabber) {
            sampleGrabber->SetCallback(nullptr, 0);
        }
        if (graph && nullRendererFilter) {
            graph->RemoveFilter(nullRendererFilter);
        }
        if (graph && sampleGrabberFilter) {
            graph->RemoveFilter(sampleGrabberFilter);
        }
        releaseCom(sampleGrabber);
        if (sampleCallback) {
            sampleCallback->Release();
            sampleCallback = nullptr;
        }
        releaseCom(nullRendererFilter);
        releaseCom(sampleGrabberFilter);
    }

    bool runGraph(QString *reason)
    {
        HRESULT hr = graph->QueryInterface(IID_IMediaControl,
                                           reinterpret_cast<void **>(&mediaControl));
        if (FAILED(hr) || !mediaControl) {
            if (reason) {
                *reason = QStringLiteral("Query IMediaControl failed: %1").arg(hrText(hr));
            }
            return false;
        }

        hr = mediaControl->Run();
        if (FAILED(hr)) {
            if (reason) {
                *reason = QStringLiteral("Run DirectShow graph failed: %1").arg(hrText(hr));
            }
            return false;
        }
        running = true;
        return true;
    }

    void updateVideoRect()
    {
        if (!previewWidget || !previewHwnd) {
            return;
        }

        RECT rect = {};
        rect.left = 0;
        rect.top = 0;
        rect.right = previewWidget->width();
        rect.bottom = previewWidget->height();

        if (evrDisplay) {
            evrDisplay->SetVideoPosition(nullptr, &rect);
        }
        if (vmrControl) {
            vmrControl->SetVideoPosition(nullptr, &rect);
            vmrControl->RepaintVideo(previewHwnd, nullptr);
        }
    }

    void handleSample(double sampleTime, BYTE *buffer, long bufferLen)
    {
        Q_UNUSED(sampleTime);
        if (!buffer || bufferLen <= 0) {
            return;
        }

        const bool shouldDeliver =
                rawDeliveryEnabled.load() ||
                oneRawFrameRequested.exchange(false) ||
                firstFramePending.exchange(false);
        if (!shouldDeliver) {
            return;
        }

        CapturedFrame frame;
        frame.cameraDescription = mode.description;
        frame.cameraDeviceName = mode.deviceName;
        frame.resolution = frameSize;
        frame.pixelFormat = pixelFormat;
        frame.startTimeUs = -1;
        frame.planeCount = 1;
        frame.bytesPerLines << bytesPerLine;
        frame.mappedBytesPerPlane << bufferLen;
        frame.payload.append(reinterpret_cast<const char *>(buffer), bufferLen);

        postRawFrame(frame);
    }

    void releaseGraph()
    {
        running = false;
        rawCaptureActive = false;

        if (mediaControl) {
            mediaControl->Stop();
        }

        releaseRawBranch();
        if (evrDisplay) {
            evrDisplay->SetVideoWindow(nullptr);
        }
        if (vmrControl) {
            vmrControl->SetVideoClippingWindow(nullptr);
        }

        releaseCom(mediaControl);
        releaseCom(evrDisplay);
        releaseCom(vmrControl);
        releaseCom(rendererFilter);
        releaseCom(sourceFilter);
        releaseCom(builder);
        releaseCom(graph);

        previewHwnd = nullptr;
        frameSize = QSize();
        pixelFormat = QVideoFrame::Format_Invalid;
        formatTag.clear();
        bytesPerLine = 0;
        configuredFps = 0.0;
        rendererName.clear();

        if (coUninitialize) {
            CoUninitialize();
            coUninitialize = false;
        }
    }
#else
    void releaseGraph()
    {
        running = false;
        rawCaptureActive = false;
        rawDeliveryEnabled.store(false);
        oneRawFrameRequested.store(false);
        firstFramePending.store(true);
    }
#endif
};

DirectShowPreviewSession::DirectShowPreviewSession(QObject *parent)
    : QObject(parent)
    , d(new DirectShowPreviewSessionPrivate(this))
{
    qRegisterMetaType<CapturedFrame>("CapturedFrame");
}

DirectShowPreviewSession::~DirectShowPreviewSession()
{
    stop();
    delete d;
    d = nullptr;
}

bool DirectShowPreviewSession::start(const CameraModeInfo &mode,
                                     QWidget *previewWidget,
                                     QString *reason)
{
    stop();

    if (!previewWidget) {
        if (reason) {
            *reason = QStringLiteral("Preview widget is null.");
        }
        return false;
    }

    d->mode = mode;
    d->previewWidget = previewWidget;
    d->rawDeliveryEnabled.store(false);
    d->oneRawFrameRequested.store(false);
    d->firstFramePending.store(true);

#ifndef Q_OS_WIN
    if (reason) {
        *reason = QStringLiteral("DirectShow preview is only available on Windows.");
    }
    return false;
#else
    d->previewHwnd = reinterpret_cast<HWND>(previewWidget->winId());
    if (!d->previewHwnd) {
        if (reason) {
            *reason = QStringLiteral("Cannot get native preview window handle.");
        }
        return false;
    }

    QString localReason;
    if (!d->initializeCom(&localReason) ||
        !d->createGraph(&localReason) ||
        !d->bindCameraSource(&localReason) ||
        !d->configureExactMode(&localReason) ||
        !d->setupPreviewRenderer(&localReason)) {
        d->releaseGraph();
        if (reason) {
            *reason = localReason;
        }
        return false;
    }

    QString rawReason;
    if (!d->setupRawBranch(&rawReason)) {
        d->rawCaptureActive = false;
        d->releaseRawBranch();
        d->postLog(QStringLiteral("Raw callback branch unavailable: %1").arg(rawReason));
    }

    if (!d->runGraph(&localReason)) {
        d->releaseGraph();
        if (reason) {
            *reason = localReason;
        }
        return false;
    }

    previewWidget->installEventFilter(this);
    d->updateVideoRect();
    emit logMessage(QStringLiteral("DirectShow preview started: renderer=%1, camera=%2, size=%3x%4, format=%5, fps=%6, rawBranch=%7")
                    .arg(d->rendererName)
                    .arg(mode.description)
                    .arg(d->frameSize.width())
                    .arg(d->frameSize.height())
                    .arg(d->formatTag)
                    .arg(d->configuredFps, 0, 'f', 3)
                    .arg(d->rawCaptureActive ? QStringLiteral("on") : QStringLiteral("off")));
    return true;
#endif
}

void DirectShowPreviewSession::stop()
{
    if (!d) {
        return;
    }
    if (d->previewWidget) {
        d->previewWidget->removeEventFilter(this);
    }
    d->releaseGraph();
    d->previewWidget.clear();
}

bool DirectShowPreviewSession::isRunning() const
{
    return d && d->running;
}

bool DirectShowPreviewSession::isRawCaptureActive() const
{
    return d && d->rawCaptureActive;
}

void DirectShowPreviewSession::setRawFrameDeliveryEnabled(bool enabled)
{
    if (!d) {
        return;
    }
    d->rawDeliveryEnabled.store(enabled);
}

void DirectShowPreviewSession::requestOneRawFrame()
{
    if (!d) {
        return;
    }
    d->oneRawFrameRequested.store(true);
}

void DirectShowPreviewSession::updateVideoRect()
{
#ifdef Q_OS_WIN
    if (d) {
        d->updateVideoRect();
    }
#endif
}

bool DirectShowPreviewSession::eventFilter(QObject *watched, QEvent *event)
{
    if (d && watched == d->previewWidget.data()) {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::WinIdChange:
            updateVideoRect();
            break;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

void DirectShowPreviewSession::deliverRawFrame(const CapturedFrame &frame)
{
    emit rawFrameAvailable(frame);
}

void DirectShowPreviewSession::deliverLogMessage(const QString &msg)
{
    emit logMessage(msg);
}

void DirectShowPreviewSession::deliverRawFailure(const QString &reason)
{
    emit rawFrameFailed(reason);
}
