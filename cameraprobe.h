#ifndef CAMERAPROBE_H
#define CAMERAPROBE_H

#include <QObject>
#include <QCamera>
#include <QCameraInfo>
#include <QCameraViewfinderSettings>
#include <QAbstractVideoSurface>
#include <QVideoFrame>
#include <QSize>
#include <QByteArray>
#include <QList>
#include <QString>
#include <QTimer>

struct CameraModeInfo
{
    int cameraIndex = -1;
    QCameraInfo cameraInfo;
    QString description;
    QString deviceName;
    QCameraViewfinderSettings settings;
};

struct CapturedFrame
{
    QString cameraDescription;
    QString cameraDeviceName;
    QSize resolution;
    QVideoFrame::PixelFormat pixelFormat = QVideoFrame::Format_Invalid;
    qint64 startTimeUs = -1;
    int planeCount = 0;
    QList<int> bytesPerLines;
    QList<int> mappedBytesPerPlane;
    QByteArray payload;
};

Q_DECLARE_METATYPE(CapturedFrame)

class FrameGrabSurface : public QAbstractVideoSurface
{
    Q_OBJECT
public:
    explicit FrameGrabSurface(QObject *parent = nullptr);

    QList<QVideoFrame::PixelFormat> supportedPixelFormats(
            QAbstractVideoBuffer::HandleType type = QAbstractVideoBuffer::NoHandle) const override;

    bool present(const QVideoFrame &frame) override;

    void setExpectedMeta(const QString &desc, const QString &devName);
    void armOneShot();

signals:
    void logMessage(const QString &msg);
    void firstFrameCaptured(const CapturedFrame &frame);
    void frameCaptureFailed(const QString &reason);

private:
    bool m_waitingFirstFrame = false;
    QString m_cameraDescription;
    QString m_deviceName;
};

class CameraProbe : public QObject
{
    Q_OBJECT
public:
    explicit CameraProbe(QObject *parent = nullptr);
    ~CameraProbe() override;

    static QList<CameraModeInfo> enumerateAllModes();
    static QList<CameraModeInfo> enumerateYuy2Modes();
    static QString pixelFormatToString(QVideoFrame::PixelFormat fmt);

    static bool findPreferredYuy2Mode(int width,
                                      int height,
                                      CameraModeInfo &outMode,
                                      QString *reason = nullptr);

    bool startSingleFrameCapture(const CameraModeInfo &mode);
    void stopCapture();

signals:
    void logMessage(const QString &msg);
    void captureSucceeded(const CapturedFrame &frame);
    void captureFailed(const QString &reason);

private slots:
    void onCameraError(QCamera::Error error);
    void onSurfaceLog(const QString &msg);
    void onSurfaceFrameCaptured(const CapturedFrame &frame);
    void onSurfaceFrameFailed(const QString &reason);
    void onCaptureTimeout();

private:
    QCamera *m_camera = nullptr;
    FrameGrabSurface *m_surface = nullptr;
    QTimer *m_frameTimeout = nullptr;
    int m_frameTimeoutMs = 3000;
};

#endif // CAMERAPROBE_H
