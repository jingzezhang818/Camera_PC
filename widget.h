#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QCamera>
#include <QCameraViewfinder>
#include "cameraprobe.h"

QT_BEGIN_NAMESPACE
namespace Ui { class Widget; }
QT_END_NAMESPACE

class Widget : public QWidget
{
    Q_OBJECT

public:
    explicit Widget(QWidget *parent = nullptr);
    ~Widget();

private slots:
    void on_btnListModes_clicked();
    void on_btnGrabOneFrame_clicked();

    void onProbeLog(const QString &msg);
    void onProbeSuccess(const CapturedFrame &frame);
    void onProbeFailed(const QString &reason);
    void onPreviewCameraError(QCamera::Error error);

private:
    void initializePreview();
    void startPreview();
    void stopPreview();

    Ui::Widget *ui;
    CameraProbe *m_probe = nullptr;
    QCamera *m_previewCamera = nullptr;
    QCameraViewfinder *m_viewfinder = nullptr;
    bool m_restartPreviewAfterCapture = false;
};

#endif // WIDGET_H
