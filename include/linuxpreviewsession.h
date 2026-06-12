// Copyright (c) 2026 jingzezhang818.
// All rights reserved.

#ifndef LINUXPREVIEWSESSION_H
#define LINUXPREVIEWSESSION_H

#include "cameraprobe.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <atomic>

#include <gst/gst.h>

class QWidget;

struct LinuxPreviewMode
{
    QString devicePath;
    QString deviceName;
    QSize resolution;
    quint32 fourcc = 0;
    int fpsNumerator = 0;   // V4L2 timeperframe numerator.
    int fpsDenominator = 0; // V4L2 timeperframe denominator.

    bool isValid() const;
    double fps() const;
    QString fourccText() const;
    QString displayText() const;
};

struct LinuxAcceptedMode
{
    QString devicePath;
    QSize resolution;
    quint32 fourcc = 0;
    int fpsNumerator = 0;
    int fpsDenominator = 0;
    int bytesPerLine = 0;
    int imageSize = 0;

    double fps() const;
    QString fourccText() const;
    QString displayText() const;
};

Q_DECLARE_METATYPE(LinuxAcceptedMode)

class LinuxPreviewSession : public QObject
{
    Q_OBJECT

public:
    explicit LinuxPreviewSession(QObject *parent = nullptr);
    ~LinuxPreviewSession() override;

    static QList<LinuxPreviewMode> enumerateModes(QStringList *logLines = nullptr);

    bool start(const LinuxPreviewMode &mode, QWidget *previewWidget, QString *reason = nullptr);
    void stop();
    bool isRunning() const;
    void setRawFrameDeliveryEnabled(bool enabled);

signals:
    void logMessage(const QString &msg);
    void acceptedModeChanged(const LinuxAcceptedMode &mode);
    void captureFpsUpdated(double fps);
    void renderFpsUpdated(double fps);
    void rawFrameAvailable(const CapturedFrame &frame);
    void rawFrameFailed(const QString &reason);

private slots:
    void pollBus();

private:
    static GstPadProbeReturn onCaptureFpsProbe(GstPad *pad, GstPadProbeInfo *info, gpointer userData);
    static GstPadProbeReturn onRenderFpsProbe(GstPad *pad, GstPadProbeInfo *info, gpointer userData);
    static void onFpsMeasurement(GstElement *sink,
                                 gdouble fps,
                                 gdouble droprate,
                                 gdouble avgfps,
                                 gpointer userData);
    static GstFlowReturn onNewSample(GstElement *sink, gpointer userData);
    GstFlowReturn handleNewSample(GstElement *sink);

    static bool applyV4l2Mode(const LinuxPreviewMode &mode,
                              LinuxAcceptedMode &accepted,
                              QStringList &logLines,
                              QString *reason);
    bool buildPipeline(const LinuxPreviewMode &mode,
                       QWidget *previewWidget,
                       QString *reason);
    void releasePipeline();
    bool installCaptureFpsProbe(GstElement *tee);
    bool installRenderFpsProbe(GstElement *convert);
    void removeFpsProbes();
    void resetFpsCounters();
    void recordCaptureFrame();
    void recordRenderFrame();

    GstElement *m_pipeline = nullptr;
    GstElement *m_appSink = nullptr;
    GstElement *m_videoSink = nullptr;
    GstElement *m_fpsDisplaySink = nullptr;
    GstPad *m_captureFpsPad = nullptr;
    GstPad *m_renderFpsPad = nullptr;
    gulong m_captureFpsProbeId = 0;
    gulong m_renderFpsProbeId = 0;
    QTimer m_busTimer;

    mutable QMutex m_stateMutex;
    mutable QMutex m_fpsMutex;
    LinuxAcceptedMode m_acceptedMode;
    QElapsedTimer m_captureFpsTimer;
    QElapsedTimer m_renderFpsTimer;
    int m_captureFpsFrames = 0;
    int m_renderFpsFrames = 0;
    std::atomic_bool m_rawDeliveryEnabled { false };
};

#endif // LINUXPREVIEWSESSION_H
