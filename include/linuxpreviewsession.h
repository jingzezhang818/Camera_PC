#ifndef LINUXPREVIEWSESSION_H
#define LINUXPREVIEWSESSION_H

#include "cameraprobe.h"

#include <QByteArray>
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
    void rawFrameAvailable(const CapturedFrame &frame);
    void rawFrameFailed(const QString &reason);

private slots:
    void pollBus();

private:
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

    GstElement *m_pipeline = nullptr;
    GstElement *m_appSink = nullptr;
    GstElement *m_videoSink = nullptr;
    QTimer m_busTimer;

    mutable QMutex m_stateMutex;
    LinuxAcceptedMode m_acceptedMode;
    std::atomic_bool m_rawDeliveryEnabled { false };
};

#endif // LINUXPREVIEWSESSION_H
