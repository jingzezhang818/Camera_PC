#include "linuxpreviewsession.h"

#include <QDir>
#include <QMetaType>
#include <QWidget>

#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/video/videooverlay.h>

#include <linux/videodev2.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <numeric>

namespace {

constexpr quint32 kYuyvFourcc = V4L2_PIX_FMT_YUYV;

int xioctl(int fd, unsigned long request, void *arg)
{
    int ret = 0;
    do {
        ret = ::ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

QString errnoText(const QString &prefix)
{
    return QString("%1: %2").arg(prefix, QString::fromLocal8Bit(std::strerror(errno)));
}

QString fourccToString(quint32 fourcc)
{
    QByteArray text;
    text.resize(4);
    text[0] = static_cast<char>(fourcc & 0xFF);
    text[1] = static_cast<char>((fourcc >> 8) & 0xFF);
    text[2] = static_cast<char>((fourcc >> 16) & 0xFF);
    text[3] = static_cast<char>((fourcc >> 24) & 0xFF);
    return QString::fromLatin1(text);
}

double fpsFromTimePerFrame(int numerator, int denominator)
{
    if (numerator <= 0 || denominator <= 0) {
        return 0.0;
    }
    return static_cast<double>(denominator) / static_cast<double>(numerator);
}

void normalizeFraction(int &numerator, int &denominator)
{
    if (numerator <= 0 || denominator <= 0) {
        return;
    }
    const int divisor = std::gcd(numerator, denominator);
    if (divisor > 1) {
        numerator /= divisor;
        denominator /= divisor;
    }
}

QString modeSummary(const QString &devicePath,
                    const QSize &resolution,
                    quint32 fourcc,
                    int fpsNumerator,
                    int fpsDenominator)
{
    return QString("%1 %2x%3 %4 fps=%5")
            .arg(devicePath)
            .arg(resolution.width())
            .arg(resolution.height())
            .arg(fourccToString(fourcc))
            .arg(fpsFromTimePerFrame(fpsNumerator, fpsDenominator), 0, 'f', 3);
}

QStringList videoSinkFactoryCandidates()
{
    return QStringList {
        QStringLiteral("ximagesink"),
        QStringLiteral("waylandsink"),
        QStringLiteral("xvimagesink"),
        QStringLiteral("glimagesink"),
        QStringLiteral("autovideosink"),
        QStringLiteral("fakesink")
    };
}

void unrefElement(GstElement *element)
{
    if (element) {
        gst_object_unref(element);
    }
}

bool hasGObjectProperty(GObject *object, const char *propertyName)
{
    return object && g_object_class_find_property(G_OBJECT_GET_CLASS(object), propertyName);
}

void setBoolPropertyIfPresent(GstElement *element, const char *propertyName, gboolean value)
{
    if (hasGObjectProperty(G_OBJECT(element), propertyName)) {
        g_object_set(element, propertyName, value, nullptr);
    }
}

QString messageSourceName(GstMessage *message)
{
    if (!message || !message->src) {
        return QStringLiteral("unknown");
    }
    return QString::fromLatin1(GST_OBJECT_NAME(message->src));
}

QString formatGstErrorMessage(GstMessage *message)
{
    GError *error = nullptr;
    gchar *debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    const QString text = QString("[GST][ERROR] source=%1 message=%2%3")
            .arg(messageSourceName(message))
            .arg(error ? QString::fromUtf8(error->message) : QStringLiteral("unknown error"))
            .arg(debug ? QString(" debug=%1").arg(QString::fromUtf8(debug)) : QString());
    if (error) {
        g_error_free(error);
    }
    if (debug) {
        g_free(debug);
    }
    return text;
}

QString formatGstWarningMessage(GstMessage *message)
{
    GError *error = nullptr;
    gchar *debug = nullptr;
    gst_message_parse_warning(message, &error, &debug);
    const QString text = QString("[GST][WARN] source=%1 message=%2%3")
            .arg(messageSourceName(message))
            .arg(error ? QString::fromUtf8(error->message) : QStringLiteral("unknown warning"))
            .arg(debug ? QString(" debug=%1").arg(QString::fromUtf8(debug)) : QString());
    if (error) {
        g_error_free(error);
    }
    if (debug) {
        g_free(debug);
    }
    return text;
}

QString drainStartupBusDiagnostics(GstElement *pipeline)
{
    GstBus *bus = gst_element_get_bus(pipeline);
    if (!bus) {
        return QString();
    }

    QStringList diagnostics;
    const auto mask = static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING);
    GstMessage *message = gst_bus_timed_pop_filtered(bus, 250 * GST_MSECOND, mask);
    while (message) {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            diagnostics << formatGstErrorMessage(message);
        } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) {
            diagnostics << formatGstWarningMessage(message);
        }
        gst_message_unref(message);
        message = gst_bus_pop_filtered(bus, mask);
    }

    gst_object_unref(bus);
    return diagnostics.join(QStringLiteral("; "));
}

void appendIntervalLog(QStringList *logLines,
                       const QString &devicePath,
                       const QSize &resolution,
                       const v4l2_frmivalenum &interval)
{
    if (!logLines) {
        return;
    }

    if (interval.type == V4L2_FRMIVAL_TYPE_STEPWISE ||
        interval.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
        const auto &s = interval.stepwise;
        *logLines << QString("[V4L2] %1 %2x%3 reports %4 frame intervals: min=%5/%6, max=%7/%8, step=%9/%10. Not shown as selectable discrete fps.")
                     .arg(devicePath)
                     .arg(resolution.width())
                     .arg(resolution.height())
                     .arg(interval.type == V4L2_FRMIVAL_TYPE_CONTINUOUS ? "continuous" : "stepwise")
                     .arg(s.min.numerator)
                     .arg(s.min.denominator)
                     .arg(s.max.numerator)
                     .arg(s.max.denominator)
                     .arg(s.step.numerator)
                     .arg(s.step.denominator);
    }
}

} // namespace

bool LinuxPreviewMode::isValid() const
{
    return !devicePath.isEmpty()
            && resolution.width() > 0
            && resolution.height() > 0
            && fourcc == kYuyvFourcc
            && fpsNumerator > 0
            && fpsDenominator > 0;
}

double LinuxPreviewMode::fps() const
{
    return fpsFromTimePerFrame(fpsNumerator, fpsDenominator);
}

QString LinuxPreviewMode::fourccText() const
{
    return fourccToString(fourcc);
}

QString LinuxPreviewMode::displayText() const
{
    return QString("%1 | %2x%3 | %4 | fps=%5")
            .arg(devicePath)
            .arg(resolution.width())
            .arg(resolution.height())
            .arg(fourccText())
            .arg(fps(), 0, 'f', 3);
}

double LinuxAcceptedMode::fps() const
{
    return fpsFromTimePerFrame(fpsNumerator, fpsDenominator);
}

QString LinuxAcceptedMode::fourccText() const
{
    return fourccToString(fourcc);
}

QString LinuxAcceptedMode::displayText() const
{
    return QString("%1 %2x%3 %4 fps=%5 bytesPerLine=%6 imageSize=%7")
            .arg(devicePath)
            .arg(resolution.width())
            .arg(resolution.height())
            .arg(fourccText())
            .arg(fps(), 0, 'f', 3)
            .arg(bytesPerLine)
            .arg(imageSize);
}

LinuxPreviewSession::LinuxPreviewSession(QObject *parent)
    : QObject(parent)
{
    static std::once_flag gstInitFlag;
    std::call_once(gstInitFlag, []() {
        gst_init(nullptr, nullptr);
    });

    qRegisterMetaType<CapturedFrame>("CapturedFrame");
    qRegisterMetaType<LinuxAcceptedMode>("LinuxAcceptedMode");

    m_busTimer.setInterval(100);
    connect(&m_busTimer, &QTimer::timeout, this, &LinuxPreviewSession::pollBus);
}

LinuxPreviewSession::~LinuxPreviewSession()
{
    stop();
}

QList<LinuxPreviewMode> LinuxPreviewSession::enumerateModes(QStringList *logLines)
{
    QList<LinuxPreviewMode> modes;

    QDir devDir(QStringLiteral("/dev"));
    const QStringList entries = devDir.entryList(QStringList() << QStringLiteral("video*"),
                                                 QDir::System,
                                                 QDir::Name);
    for (const QString &entry : entries) {
        const QString devicePath = QStringLiteral("/dev/") + entry;
        const int fd = ::open(devicePath.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            if (logLines) {
                *logLines << errnoText(QString("[V4L2] Cannot open %1").arg(devicePath));
            }
            continue;
        }

        v4l2_capability cap {};
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
            if (logLines) {
                *logLines << errnoText(QString("[V4L2] VIDIOC_QUERYCAP failed for %1").arg(devicePath));
            }
            ::close(fd);
            continue;
        }

        const bool captureCapable =
                (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
                (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE);
        if (!captureCapable) {
            ::close(fd);
            continue;
        }

        const QString deviceName = QString::fromLocal8Bit(reinterpret_cast<const char *>(cap.card));
        v4l2_fmtdesc fmt {};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        for (fmt.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0; ++fmt.index) {
            if (fmt.pixelformat != kYuyvFourcc) {
                continue;
            }

            v4l2_frmsizeenum size {};
            size.pixel_format = fmt.pixelformat;
            for (size.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0; ++size.index) {
                if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
                    if (logLines) {
                        *logLines << QString("[V4L2] %1 %2 reports non-discrete frame sizes. Not shown in UI.")
                                     .arg(devicePath)
                                     .arg(fourccToString(fmt.pixelformat));
                    }
                    continue;
                }

                const QSize resolution(static_cast<int>(size.discrete.width),
                                       static_cast<int>(size.discrete.height));
                v4l2_frmivalenum interval {};
                interval.pixel_format = fmt.pixelformat;
                interval.width = size.discrete.width;
                interval.height = size.discrete.height;
                bool sawInterval = false;
                for (interval.index = 0;
                     xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == 0;
                     ++interval.index) {
                    sawInterval = true;
                    if (interval.type != V4L2_FRMIVAL_TYPE_DISCRETE) {
                        appendIntervalLog(logLines, devicePath, resolution, interval);
                        continue;
                    }

                    int numerator = static_cast<int>(interval.discrete.numerator);
                    int denominator = static_cast<int>(interval.discrete.denominator);
                    normalizeFraction(numerator, denominator);
                    if (numerator <= 0 || denominator <= 0) {
                        continue;
                    }

                    LinuxPreviewMode mode;
                    mode.devicePath = devicePath;
                    mode.deviceName = deviceName;
                    mode.resolution = resolution;
                    mode.fourcc = fmt.pixelformat;
                    mode.fpsNumerator = numerator;
                    mode.fpsDenominator = denominator;
                    modes.push_back(mode);
                }

                if (!sawInterval && logLines) {
                    *logLines << QString("[V4L2] %1 %2x%3 %4 has no discrete frame interval list.")
                                 .arg(devicePath)
                                 .arg(resolution.width())
                                 .arg(resolution.height())
                                 .arg(fourccToString(fmt.pixelformat));
                }
            }
        }

        ::close(fd);
    }

    std::sort(modes.begin(), modes.end(), [](const LinuxPreviewMode &a, const LinuxPreviewMode &b) {
        if (a.devicePath != b.devicePath) {
            return a.devicePath < b.devicePath;
        }
        const int areaA = a.resolution.width() * a.resolution.height();
        const int areaB = b.resolution.width() * b.resolution.height();
        if (areaA != areaB) {
            return areaA < areaB;
        }
        return a.fps() < b.fps();
    });

    return modes;
}

bool LinuxPreviewSession::start(const LinuxPreviewMode &mode,
                                QWidget *previewWidget,
                                QString *reason)
{
    stop();

    if (!mode.isValid()) {
        if (reason) {
            *reason = QStringLiteral("invalid Linux preview mode");
        }
        return false;
    }
    if (!previewWidget) {
        if (reason) {
            *reason = QStringLiteral("preview widget is null");
        }
        return false;
    }

    LinuxAcceptedMode accepted;
    QStringList setupLogs;
    if (!applyV4l2Mode(mode, accepted, setupLogs, reason)) {
        return false;
    }
    for (const QString &line : setupLogs) {
        emit logMessage(line);
    }

    {
        QMutexLocker locker(&m_stateMutex);
        m_acceptedMode = accepted;
    }

    if (!buildPipeline(mode, previewWidget, reason)) {
        releasePipeline();
        return false;
    }

    emit acceptedModeChanged(accepted);
    emit logMessage(QString("[GST] Preview pipeline started with sink=%1")
                    .arg(m_videoSink ? QString::fromLatin1(G_OBJECT_TYPE_NAME(m_videoSink))
                                     : QStringLiteral("unknown")));
    return true;
}

void LinuxPreviewSession::stop()
{
    m_rawDeliveryEnabled.store(false);
    releasePipeline();
}

bool LinuxPreviewSession::isRunning() const
{
    return m_pipeline != nullptr;
}

void LinuxPreviewSession::setRawFrameDeliveryEnabled(bool enabled)
{
    m_rawDeliveryEnabled.store(enabled);
}

bool LinuxPreviewSession::applyV4l2Mode(const LinuxPreviewMode &mode,
                                        LinuxAcceptedMode &accepted,
                                        QStringList &logLines,
                                        QString *reason)
{
    const int fd = ::open(mode.devicePath.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        if (reason) {
            *reason = errnoText(QString("Cannot open %1").arg(mode.devicePath));
        }
        return false;
    }

    v4l2_format fmt {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = static_cast<quint32>(mode.resolution.width());
    fmt.fmt.pix.height = static_cast<quint32>(mode.resolution.height());
    fmt.fmt.pix.pixelformat = mode.fourcc;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    logLines << QString("[V4L2] Requested mode: %1")
                .arg(modeSummary(mode.devicePath,
                                 mode.resolution,
                                 mode.fourcc,
                                 mode.fpsNumerator,
                                 mode.fpsDenominator));

    if (xioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
        if (reason) {
            *reason = errnoText(QString("VIDIOC_S_FMT failed for %1").arg(mode.devicePath));
        }
        ::close(fd);
        return false;
    }

    v4l2_streamparm parm {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = static_cast<quint32>(mode.fpsNumerator);
    parm.parm.capture.timeperframe.denominator = static_cast<quint32>(mode.fpsDenominator);
    if (xioctl(fd, VIDIOC_S_PARM, &parm) != 0) {
        logLines << errnoText(QString("[V4L2][WARN] VIDIOC_S_PARM failed for %1").arg(mode.devicePath));
    }

    v4l2_format acceptedFmt {};
    acceptedFmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_G_FMT, &acceptedFmt) != 0) {
        if (reason) {
            *reason = errnoText(QString("VIDIOC_G_FMT failed for %1").arg(mode.devicePath));
        }
        ::close(fd);
        return false;
    }

    v4l2_streamparm acceptedParm {};
    acceptedParm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_G_PARM, &acceptedParm) != 0) {
        acceptedParm.parm.capture.timeperframe.numerator = static_cast<quint32>(mode.fpsNumerator);
        acceptedParm.parm.capture.timeperframe.denominator = static_cast<quint32>(mode.fpsDenominator);
        logLines << errnoText(QString("[V4L2][WARN] VIDIOC_G_PARM failed for %1").arg(mode.devicePath));
    }

    ::close(fd);

    accepted.devicePath = mode.devicePath;
    accepted.resolution = QSize(static_cast<int>(acceptedFmt.fmt.pix.width),
                                static_cast<int>(acceptedFmt.fmt.pix.height));
    accepted.fourcc = acceptedFmt.fmt.pix.pixelformat;
    accepted.fpsNumerator = static_cast<int>(acceptedParm.parm.capture.timeperframe.numerator);
    accepted.fpsDenominator = static_cast<int>(acceptedParm.parm.capture.timeperframe.denominator);
    accepted.bytesPerLine = static_cast<int>(acceptedFmt.fmt.pix.bytesperline);
    accepted.imageSize = static_cast<int>(acceptedFmt.fmt.pix.sizeimage);
    normalizeFraction(accepted.fpsNumerator, accepted.fpsDenominator);
    if (accepted.fpsNumerator <= 0 || accepted.fpsDenominator <= 0) {
        accepted.fpsNumerator = mode.fpsNumerator;
        accepted.fpsDenominator = mode.fpsDenominator;
    }

    logLines << QString("[V4L2] Accepted mode: %1").arg(accepted.displayText());

    if (accepted.fourcc != kYuyvFourcc) {
        if (reason) {
            *reason = QString("V4L2 accepted format=%1, expected=YUYV")
                    .arg(accepted.fourccText());
        }
        return false;
    }
    if (accepted.resolution != mode.resolution) {
        if (reason) {
            *reason = QString("V4L2 accepted resolution=%1x%2, expected=%3x%4")
                    .arg(accepted.resolution.width())
                    .arg(accepted.resolution.height())
                    .arg(mode.resolution.width())
                    .arg(mode.resolution.height());
        }
        return false;
    }

    return true;
}

bool LinuxPreviewSession::buildPipeline(const LinuxPreviewMode &mode,
                                        QWidget *previewWidget,
                                        QString *reason)
{
    LinuxAcceptedMode accepted;
    {
        QMutexLocker locker(&m_stateMutex);
        accepted = m_acceptedMode;
    }

    QStringList startupFailures;
    for (const QString &sinkName : videoSinkFactoryCandidates()) {
        GstElement *pipeline = gst_pipeline_new("linux_preview_pipeline");
        GstElement *source = gst_element_factory_make("v4l2src", "source");
        GstElement *capsFilter = gst_element_factory_make("capsfilter", "capsfilter");
        GstElement *tee = gst_element_factory_make("tee", "tee");
        GstElement *previewQueue = gst_element_factory_make("queue", "preview_queue");
        GstElement *rawQueue = gst_element_factory_make("queue", "raw_queue");
        GstElement *convert = gst_element_factory_make("videoconvert", "preview_convert");
        const QByteArray sinkFactory = sinkName.toLatin1();
        GstElement *videoSink = gst_element_factory_make(sinkFactory.constData(), "preview_sink");
        GstElement *appSink = gst_element_factory_make("appsink", "raw_sink");

        if (!videoSink) {
            startupFailures << QString("%1: element factory is not available").arg(sinkName);
            unrefElement(pipeline);
            unrefElement(source);
            unrefElement(capsFilter);
            unrefElement(tee);
            unrefElement(previewQueue);
            unrefElement(rawQueue);
            unrefElement(convert);
            unrefElement(appSink);
            continue;
        }

        if (!pipeline || !source || !capsFilter || !tee || !previewQueue ||
            !rawQueue || !convert || !appSink) {
            if (reason) {
                *reason = QStringLiteral("failed to create one or more required GStreamer elements");
            }
            unrefElement(pipeline);
            unrefElement(source);
            unrefElement(capsFilter);
            unrefElement(tee);
            unrefElement(previewQueue);
            unrefElement(rawQueue);
            unrefElement(convert);
            unrefElement(videoSink);
            unrefElement(appSink);
            return false;
        }

        g_object_set(source, "device", mode.devicePath.toLocal8Bit().constData(), nullptr);
        g_object_set(previewQueue, "leaky", 2, "max-size-buffers", 2, nullptr);
        g_object_set(rawQueue, "leaky", 2, "max-size-buffers", 2, nullptr);
        g_object_set(appSink,
                     "emit-signals", TRUE,
                     "sync", FALSE,
                     "max-buffers", 2,
                     "drop", TRUE,
                     nullptr);

        setBoolPropertyIfPresent(videoSink, "sync", FALSE);
        setBoolPropertyIfPresent(videoSink, "async", FALSE);
        setBoolPropertyIfPresent(videoSink, "enable-last-sample", FALSE);

        GstCaps *caps = nullptr;
        if (accepted.fpsNumerator > 0 && accepted.fpsDenominator > 0) {
            caps = gst_caps_new_simple("video/x-raw",
                                       "format", G_TYPE_STRING, "YUY2",
                                       "width", G_TYPE_INT, accepted.resolution.width(),
                                       "height", G_TYPE_INT, accepted.resolution.height(),
                                       "framerate", GST_TYPE_FRACTION,
                                       accepted.fpsDenominator,
                                       accepted.fpsNumerator,
                                       nullptr);
        } else {
            caps = gst_caps_new_simple("video/x-raw",
                                       "format", G_TYPE_STRING, "YUY2",
                                       "width", G_TYPE_INT, accepted.resolution.width(),
                                       "height", G_TYPE_INT, accepted.resolution.height(),
                                       nullptr);
        }
        g_object_set(capsFilter, "caps", caps, nullptr);
        gst_caps_unref(caps);

        gst_bin_add_many(GST_BIN(pipeline),
                         source,
                         capsFilter,
                         tee,
                         previewQueue,
                         convert,
                         videoSink,
                         rawQueue,
                         appSink,
                         nullptr);

        const bool fixedLinksOk =
                gst_element_link_many(source, capsFilter, tee, nullptr) &&
                gst_element_link_many(previewQueue, convert, videoSink, nullptr) &&
                gst_element_link_many(rawQueue, appSink, nullptr);
        if (!fixedLinksOk) {
            const QString failure = QString("%1: failed to link fixed GStreamer elements").arg(sinkName);
            startupFailures << failure;
            emit logMessage(QStringLiteral("[GST][WARN] ") + failure);
            gst_object_unref(pipeline);
            continue;
        }

        GstPad *teePreviewPad = gst_element_get_request_pad(tee, "src_%u");
        GstPad *teeRawPad = gst_element_get_request_pad(tee, "src_%u");
        GstPad *previewSinkPad = gst_element_get_static_pad(previewQueue, "sink");
        GstPad *rawSinkPad = gst_element_get_static_pad(rawQueue, "sink");
        const bool teeLinksOk =
                teePreviewPad && teeRawPad && previewSinkPad && rawSinkPad &&
                gst_pad_link(teePreviewPad, previewSinkPad) == GST_PAD_LINK_OK &&
                gst_pad_link(teeRawPad, rawSinkPad) == GST_PAD_LINK_OK;
        if (teePreviewPad) {
            gst_object_unref(teePreviewPad);
        }
        if (teeRawPad) {
            gst_object_unref(teeRawPad);
        }
        if (previewSinkPad) {
            gst_object_unref(previewSinkPad);
        }
        if (rawSinkPad) {
            gst_object_unref(rawSinkPad);
        }

        if (!teeLinksOk) {
            const QString failure = QString("%1: failed to link GStreamer tee pads").arg(sinkName);
            startupFailures << failure;
            emit logMessage(QStringLiteral("[GST][WARN] ") + failure);
            gst_object_unref(pipeline);
            continue;
        }

        if (GST_IS_VIDEO_OVERLAY(videoSink)) {
            gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(videoSink),
                                                static_cast<guintptr>(previewWidget->winId()));
        } else if (sinkName != QStringLiteral("fakesink")) {
            emit logMessage(QString("[GST][WARN] selected sink %1 does not implement GstVideoOverlay; preview may open in a separate window.")
                            .arg(sinkName));
        }

        g_signal_connect(appSink, "new-sample", G_CALLBACK(&LinuxPreviewSession::onNewSample), this);

        m_pipeline = pipeline;
        m_appSink = appSink;
        m_videoSink = videoSink;

        emit logMessage(QString("[GST] Trying preview sink=%1").arg(sinkName));
        const GstStateChangeReturn stateRet = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
        if (stateRet == GST_STATE_CHANGE_FAILURE) {
            QString failure = QString("%1: failed to set GStreamer pipeline to PLAYING").arg(sinkName);
            const QString diagnostics = drainStartupBusDiagnostics(m_pipeline);
            if (!diagnostics.isEmpty()) {
                failure += QStringLiteral(" (") + diagnostics + QStringLiteral(")");
            }
            startupFailures << failure;
            emit logMessage(QStringLiteral("[GST][WARN] ") + failure);
            releasePipeline();
            continue;
        }

        m_busTimer.start();
        emit logMessage(QString("[GST] Selected preview sink=%1").arg(sinkName));
        if (sinkName == QStringLiteral("fakesink")) {
            emit logMessage(QStringLiteral("[GST][WARN] No real video sink reached PLAYING; using fakesink so raw appsink/live XDMA can still run. The embedded preview will be blank."));
        }
        emit logMessage(QString("[GST] Requested pipeline: v4l2src device=%1 ! video/x-raw,format=YUY2,width=%2,height=%3,framerate=%4/%5 ! tee")
                        .arg(mode.devicePath)
                        .arg(accepted.resolution.width())
                        .arg(accepted.resolution.height())
                        .arg(accepted.fpsDenominator)
                        .arg(accepted.fpsNumerator));
        return true;
    }

    if (reason) {
        *reason = QString("failed to set GStreamer pipeline to PLAYING with all preview sinks: %1")
                .arg(startupFailures.join(QStringLiteral(" | ")));
    }
    return false;
}

void LinuxPreviewSession::releasePipeline()
{
    m_busTimer.stop();
    m_rawDeliveryEnabled.store(false);

    if (m_appSink) {
        g_signal_handlers_disconnect_by_data(m_appSink, this);
    }
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
    }

    m_pipeline = nullptr;
    m_appSink = nullptr;
    m_videoSink = nullptr;
}

void LinuxPreviewSession::pollBus()
{
    if (!m_pipeline) {
        return;
    }

    GstBus *bus = gst_element_get_bus(m_pipeline);
    if (!bus) {
        return;
    }

    bool shouldStop = false;
    while (GstMessage *message = gst_bus_pop_filtered(
               bus,
               static_cast<GstMessageType>(GST_MESSAGE_ERROR |
                                           GST_MESSAGE_WARNING |
                                           GST_MESSAGE_EOS))) {
        switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            emit rawFrameFailed(formatGstErrorMessage(message));
            shouldStop = true;
            break;
        }
        case GST_MESSAGE_WARNING: {
            emit logMessage(formatGstWarningMessage(message));
            break;
        }
        case GST_MESSAGE_EOS:
            emit rawFrameFailed(QStringLiteral("[GST][ERROR] preview pipeline reached EOS"));
            shouldStop = true;
            break;
        default:
            break;
        }
        gst_message_unref(message);
    }

    gst_object_unref(bus);

    if (shouldStop) {
        stop();
    }
}

GstFlowReturn LinuxPreviewSession::onNewSample(GstElement *sink, gpointer userData)
{
    auto *session = static_cast<LinuxPreviewSession *>(userData);
    return session ? session->handleNewSample(sink) : GST_FLOW_ERROR;
}

GstFlowReturn LinuxPreviewSession::handleNewSample(GstElement *sink)
{
    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (!sample) {
        return GST_FLOW_OK;
    }

    if (!m_rawDeliveryEnabled.load()) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    if (!buffer || !caps) {
        gst_sample_unref(sample);
        emit rawFrameFailed(QStringLiteral("appsink sample is missing buffer or caps"));
        return GST_FLOW_OK;
    }

    GstMapInfo map {};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        emit rawFrameFailed(QStringLiteral("gst_buffer_map(ReadOnly) failed for raw live frame"));
        return GST_FLOW_OK;
    }

    LinuxAcceptedMode accepted;
    {
        QMutexLocker locker(&m_stateMutex);
        accepted = m_acceptedMode;
    }

    GstVideoInfo info;
    gst_video_info_init(&info);
    int bytesPerLine = accepted.bytesPerLine > 0
            ? accepted.bytesPerLine
            : accepted.resolution.width() * 2;
    if (gst_video_info_from_caps(&info, caps)) {
        bytesPerLine = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
    }

    CapturedFrame frame;
    frame.cameraDescription = accepted.devicePath;
    frame.cameraDeviceName = accepted.devicePath;
    frame.resolution = accepted.resolution;
    frame.pixelFormat = QVideoFrame::Format_YUYV;
    frame.startTimeUs = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))
            ? static_cast<qint64>(GST_BUFFER_PTS(buffer) / 1000)
            : -1;
    frame.planeCount = 1;
    frame.bytesPerLines.push_back(bytesPerLine);
    frame.mappedBytesPerPlane.push_back(static_cast<int>(map.size));
    frame.payload = QByteArray(reinterpret_cast<const char *>(map.data),
                               static_cast<int>(map.size));

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);

    if (frame.payload.isEmpty()) {
        emit rawFrameFailed(QStringLiteral("appsink raw frame payload is empty"));
        return GST_FLOW_OK;
    }

    emit rawFrameAvailable(frame);
    return GST_FLOW_OK;
}
