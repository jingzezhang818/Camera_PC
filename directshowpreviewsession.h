#ifndef DIRECTSHOWPREVIEWSESSION_H
#define DIRECTSHOWPREVIEWSESSION_H

#include <QObject>

#include "cameraprobe.h"

class QWidget;
class QEvent;

struct DirectShowPreviewSessionPrivate;

class DirectShowPreviewSession : public QObject
{
    Q_OBJECT

public:
    explicit DirectShowPreviewSession(QObject *parent = nullptr);
    ~DirectShowPreviewSession() override;

    bool start(const CameraModeInfo &mode,
               QWidget *previewWidget,
               QString *reason = nullptr);
    void stop();

    bool isRunning() const;
    bool isRawCaptureActive() const;

    void setRawFrameDeliveryEnabled(bool enabled);
    void requestOneRawFrame();
    void updateVideoRect();

signals:
    void logMessage(const QString &msg);
    void rawFrameAvailable(const CapturedFrame &frame);
    void rawFrameFailed(const QString &reason);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void deliverRawFrame(const CapturedFrame &frame);
    void deliverLogMessage(const QString &msg);
    void deliverRawFailure(const QString &reason);

private:
    friend struct DirectShowPreviewSessionPrivate;

    DirectShowPreviewSessionPrivate *d = nullptr;
};

#endif // DIRECTSHOWPREVIEWSESSION_H
