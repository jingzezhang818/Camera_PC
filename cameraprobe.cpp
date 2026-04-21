#include "cameraprobe.h"

#include <QDebug>

FrameGrabSurface::FrameGrabSurface(QObject *parent)
    : QAbstractVideoSurface(parent)
{
}

QList<QVideoFrame::PixelFormat> FrameGrabSurface::supportedPixelFormats(
        QAbstractVideoBuffer::HandleType type) const
{
    Q_UNUSED(type);
    QList<QVideoFrame::PixelFormat> formats;
    for (int i = static_cast<int>(QVideoFrame::Format_Invalid) + 1;
         i < static_cast<int>(QVideoFrame::NPixelFormats);
         ++i) {
        formats << static_cast<QVideoFrame::PixelFormat>(i);
    }
    return formats;
}

void FrameGrabSurface::setExpectedMeta(const QString &desc, const QString &devName)
{
    m_cameraDescription = desc;
    m_deviceName = devName;
}

void FrameGrabSurface::armOneShot()
{
    m_waitingFirstFrame = true;
}

bool FrameGrabSurface::present(const QVideoFrame &frame)
{
    if (!m_waitingFirstFrame) {
        return true;
    }

    if (!frame.isValid()) {
        emit logMessage(QStringLiteral("Invalid video frame received. Ignored."));
        return true;
    }

    QVideoFrame copy(frame);

    if (!copy.map(QAbstractVideoBuffer::ReadOnly)) {
        emit frameCaptureFailed(QStringLiteral("QVideoFrame::map(ReadOnly) failed."));
        m_waitingFirstFrame = false;
        return false;
    }

    const QVideoFrame::PixelFormat actualFmt = copy.pixelFormat();
    const int width = copy.width();
    const int height = copy.height();
    const int planeCount = copy.planeCount();

    emit logMessage(QStringLiteral("First frame received: actualFormat=%1, size=%2x%3, planes=%4")
                    .arg(CameraProbe::pixelFormatToString(actualFmt))
                    .arg(width)
                    .arg(height)
                    .arg(planeCount));

    if (width <= 0 || height <= 0) {
        copy.unmap();
        emit frameCaptureFailed(QStringLiteral("Invalid first-frame resolution."));
        m_waitingFirstFrame = false;
        return false;
    }

    CapturedFrame out;
    out.cameraDescription = m_cameraDescription;
    out.cameraDeviceName = m_deviceName;
    out.resolution = QSize(width, height);
    out.pixelFormat = actualFmt;
    out.startTimeUs = copy.startTime();
    out.planeCount = planeCount;

    const int loopPlaneCount = planeCount > 0 ? planeCount : 1;
    const int mappedBytesTotal = copy.mappedBytes();
    for (int p = 0; p < loopPlaneCount; ++p) {
        const int bytesPerLine = copy.bytesPerLine(p);

        out.bytesPerLines.push_back(bytesPerLine);
        out.mappedBytesPerPlane.push_back(p == 0 ? mappedBytesTotal : -1);
    }

    const uchar *bits = copy.bits();
    if (mappedBytesTotal > 0 && bits) {
        out.payload.append(reinterpret_cast<const char *>(bits), mappedBytesTotal);
    }

    copy.unmap();

    if (out.payload.isEmpty()) {
        emit frameCaptureFailed(QStringLiteral("First frame arrived, but no usable image payload was found."));
        m_waitingFirstFrame = false;
        return false;
    }

    m_waitingFirstFrame = false;
    emit firstFrameCaptured(out);
    return true;
}

CameraProbe::CameraProbe(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<CapturedFrame>("CapturedFrame");

    m_frameTimeout = new QTimer(this);
    m_frameTimeout->setSingleShot(true);
    connect(m_frameTimeout, &QTimer::timeout,
            this, &CameraProbe::onCaptureTimeout);
}

CameraProbe::~CameraProbe()
{
    stopCapture();
}

QString CameraProbe::pixelFormatToString(QVideoFrame::PixelFormat fmt)
{
    switch (fmt) {
    case QVideoFrame::Format_Invalid: return "Invalid";
    case QVideoFrame::Format_ARGB32_Premultiplied: return "ARGB32_Premultiplied";
    case QVideoFrame::Format_YUYV:    return "YUYV";
    case QVideoFrame::Format_UYVY:    return "UYVY";
    case QVideoFrame::Format_NV12:    return "NV12";
    case QVideoFrame::Format_NV21:    return "NV21";
    case QVideoFrame::Format_YUV420P: return "YUV420P";
    case QVideoFrame::Format_YV12:    return "YV12";
    case QVideoFrame::Format_RGB32:   return "RGB32";
    case QVideoFrame::Format_RGB24:   return "RGB24";
    case QVideoFrame::Format_ARGB32:  return "ARGB32";
    case QVideoFrame::Format_BGR32:   return "BGR32";
    case QVideoFrame::Format_BGR24:   return "BGR24";
    case QVideoFrame::Format_Jpeg:    return "JPEG";
    default:
        return QString("Other(%1)").arg(static_cast<int>(fmt));
    }
}

QList<CameraModeInfo> CameraProbe::enumerateAllModes()
{
    QList<CameraModeInfo> result;

    const QList<QCameraInfo> cameras = QCameraInfo::availableCameras();

    for (int i = 0; i < cameras.size(); ++i) {
        const QCameraInfo &info = cameras[i];

        QCamera cam(info);

        // 路径 1：直接取完整 settings
        const QList<QCameraViewfinderSettings> settingsList = cam.supportedViewfinderSettings();
        if (!settingsList.isEmpty()) {
            for (const auto &s : settingsList) {
                CameraModeInfo item;
                item.cameraIndex = i;
                item.cameraInfo = info;
                item.description = info.description();
                item.deviceName = info.deviceName();
                item.settings = s;
                result.push_back(item);
            }
            continue;
        }

        // 路径 2：如果完整 settings 为空，就退化为 resolution + pixel format 组合
        const QList<QSize> resolutions = cam.supportedViewfinderResolutions();
        const QList<QVideoFrame::PixelFormat> pixelFormats = cam.supportedViewfinderPixelFormats();

        if (!resolutions.isEmpty() && !pixelFormats.isEmpty()) {
            for (const auto &res : resolutions) {
                for (const auto &fmt : pixelFormats) {
                    QCameraViewfinderSettings s;
                    s.setResolution(res);
                    s.setPixelFormat(fmt);

                    CameraModeInfo item;
                    item.cameraIndex = i;
                    item.cameraInfo = info;
                    item.description = info.description();
                    item.deviceName = info.deviceName();
                    item.settings = s;
                    result.push_back(item);
                }
            }
            continue;
        }

        // 路径 3：如果连 resolution / pixelFormats 都拿不到，
        // 也至少把摄像头本身记录下来，方便 UI 看到“有设备但没模式”
        CameraModeInfo item;
        item.cameraIndex = i;
        item.cameraInfo = info;
        item.description = info.description();
        item.deviceName = info.deviceName();

        QCameraViewfinderSettings s;
        s.setResolution(QSize(0, 0));
        s.setPixelFormat(QVideoFrame::Format_Invalid);
        item.settings = s;

        result.push_back(item);
    }

    return result;
}

QList<CameraModeInfo> CameraProbe::enumerateYuy2Modes()
{
    QList<CameraModeInfo> all = enumerateAllModes();
    QList<CameraModeInfo> yuy2;

    for (const auto &m : all) {
        if (m.settings.pixelFormat() == QVideoFrame::Format_YUYV) {
            yuy2.push_back(m);
        }
    }

    return yuy2;
}

bool CameraProbe::findPreferredYuy2Mode(int width,
                                        int height,
                                        CameraModeInfo &outMode,
                                        QString *reason)
{
    const QList<CameraModeInfo> modes = enumerateYuy2Modes();

    if (modes.isEmpty()) {
        if (reason) {
            *reason = QStringLiteral("No YUY2/YUYV mode found.");
        }
        return false;
    }

    for (const auto &m : modes) {
        if (m.settings.resolution().width() == width &&
            m.settings.resolution().height() == height) {
            outMode = m;
            if (reason) {
                *reason = QStringLiteral("Exact match found: %1 %2x%3 YUY2 fps[%4,%5]")
                        .arg(m.description)
                        .arg(width)
                        .arg(height)
                        .arg(m.settings.minimumFrameRate())
                        .arg(m.settings.maximumFrameRate());
            }
            return true;
        }
    }

    for (const auto &m : modes) {
        if (m.settings.resolution() == QSize(640, 480)) {
            outMode = m;
            if (reason) {
                *reason = QStringLiteral("%1x%2 not found. Falling back to %3 640x480 YUY2 fps[%4,%5]")
                        .arg(width)
                        .arg(height)
                        .arg(m.description)
                        .arg(m.settings.minimumFrameRate())
                        .arg(m.settings.maximumFrameRate());
            }
            return true;
        }
    }

    outMode = modes.first();
    if (reason) {
        *reason = QStringLiteral("%1x%2 and 640x480 are unavailable. Falling back to first YUY2 mode: %3 %4x%5 fps[%6,%7]")
                .arg(width)
                .arg(height)
                .arg(outMode.description)
                .arg(outMode.settings.resolution().width())
                .arg(outMode.settings.resolution().height())
                .arg(outMode.settings.minimumFrameRate())
                .arg(outMode.settings.maximumFrameRate());
    }
    return true;
}

bool CameraProbe::startSingleFrameCapture(const CameraModeInfo &mode)
{
    stopCapture();

    m_camera = new QCamera(mode.cameraInfo, this);
    m_surface = new FrameGrabSurface(this);

    connect(m_camera, SIGNAL(error(QCamera::Error)),
            this, SLOT(onCameraError(QCamera::Error)));

    connect(m_surface, SIGNAL(logMessage(QString)),
            this, SLOT(onSurfaceLog(QString)),
            Qt::QueuedConnection);
    connect(m_surface, SIGNAL(firstFrameCaptured(CapturedFrame)),
            this, SLOT(onSurfaceFrameCaptured(CapturedFrame)),
            Qt::QueuedConnection);
    connect(m_surface, SIGNAL(frameCaptureFailed(QString)),
            this, SLOT(onSurfaceFrameFailed(QString)),
            Qt::QueuedConnection);

    m_surface->setExpectedMeta(mode.description, mode.deviceName);
    m_surface->armOneShot();

    m_camera->setCaptureMode(QCamera::CaptureViewfinder);
    m_camera->setViewfinder(m_surface);

    const QSize resolution = mode.settings.resolution();
    const bool hasResolution = resolution.width() > 0 && resolution.height() > 0;
    const bool hasPixelFormat = mode.settings.pixelFormat() != QVideoFrame::Format_Invalid;
    const bool hasFrameRate = mode.settings.minimumFrameRate() > 0.0
            || mode.settings.maximumFrameRate() > 0.0;

    if (hasResolution || hasPixelFormat || hasFrameRate) {
        m_camera->setViewfinderSettings(mode.settings);
        emit logMessage(QStringLiteral("Requested mode: resolution=%1x%2, format=%3, fps=[%4,%5]")
                        .arg(mode.settings.resolution().width())
                        .arg(mode.settings.resolution().height())
                        .arg(pixelFormatToString(mode.settings.pixelFormat()))
                        .arg(mode.settings.minimumFrameRate())
                        .arg(mode.settings.maximumFrameRate()));
    } else {
        emit logMessage(QStringLiteral("No explicit viewfinder params are set. Using camera default preview format."));
    }

    emit logMessage(QStringLiteral("Start capture: camera=%1, dev=%2, resolution=%3x%4, format=%5, fps=[%6,%7]")
                    .arg(mode.description)
                    .arg(mode.deviceName)
                    .arg(mode.settings.resolution().width())
                    .arg(mode.settings.resolution().height())
                    .arg(pixelFormatToString(mode.settings.pixelFormat()))
                    .arg(mode.settings.minimumFrameRate())
                    .arg(mode.settings.maximumFrameRate()));

    m_camera->start();
    if (m_frameTimeout) {
        m_frameTimeout->start(m_frameTimeoutMs);
    }
    return true;
}

void CameraProbe::stopCapture()
{
    if (m_frameTimeout) {
        m_frameTimeout->stop();
    }

    if (m_camera) {
        m_camera->stop();
        m_camera->deleteLater();
        m_camera = nullptr;
    }

    if (m_surface) {
        m_surface->deleteLater();
        m_surface = nullptr;
    }
}

void CameraProbe::onCameraError(QCamera::Error error)
{
    Q_UNUSED(error);
    const QString msg = m_camera ? m_camera->errorString() : QStringLiteral("Unknown camera error");
    emit captureFailed(QStringLiteral("Camera error: %1").arg(msg));
    QTimer::singleShot(0, this, &CameraProbe::stopCapture);
}

void CameraProbe::onSurfaceLog(const QString &msg)
{
    emit logMessage(msg);
}

void CameraProbe::onSurfaceFrameCaptured(const CapturedFrame &frame)
{
    emit captureSucceeded(frame);
    QTimer::singleShot(0, this, &CameraProbe::stopCapture);
}

void CameraProbe::onSurfaceFrameFailed(const QString &reason)
{
    emit captureFailed(reason);
    QTimer::singleShot(0, this, &CameraProbe::stopCapture);
}

void CameraProbe::onCaptureTimeout()
{
    emit captureFailed(QStringLiteral("Timeout waiting first frame (%1 ms). Try default format or close other apps using the camera.")
                       .arg(m_frameTimeoutMs));
    QTimer::singleShot(0, this, &CameraProbe::stopCapture);
}
