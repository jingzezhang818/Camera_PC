#include "widget.h"
#include "ui_widget.h"

#include <QFile>
#include <QDateTime>
#include <QRegularExpression>
#include <QImage>
#include <QTimer>
#include <QDebug>

namespace {

int clampToByte(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return value;
}

bool yuyvToRgbImage(const CapturedFrame &frame, QImage &outImage)
{
    const int width = frame.resolution.width();
    const int height = frame.resolution.height();
    if (width <= 0 || height <= 0) {
        return false;
    }

    const int rowStride = (!frame.bytesPerLines.isEmpty() && frame.bytesPerLines.first() > 0)
            ? frame.bytesPerLines.first()
            : width * 2;

    if (frame.payload.size() < rowStride * height) {
        return false;
    }

    QImage image(width, height, QImage::Format_RGB888);
    if (image.isNull()) {
        return false;
    }

    const uchar *src = reinterpret_cast<const uchar *>(frame.payload.constData());
    for (int y = 0; y < height; ++y) {
        const uchar *row = src + y * rowStride;
        uchar *dst = image.scanLine(y);

        for (int x = 0; x < width; x += 2) {
            const int y0 = row[x * 2 + 0];
            const int u  = row[x * 2 + 1] - 128;
            const int y1 = row[x * 2 + 2];
            const int v  = row[x * 2 + 3] - 128;

            const int rAdd = (359 * v) >> 8;
            const int gAdd = ((88 * u) + (183 * v)) >> 8;
            const int bAdd = (454 * u) >> 8;

            const int r0 = clampToByte(y0 + rAdd);
            const int g0 = clampToByte(y0 - gAdd);
            const int b0 = clampToByte(y0 + bAdd);

            dst[x * 3 + 0] = static_cast<uchar>(r0);
            dst[x * 3 + 1] = static_cast<uchar>(g0);
            dst[x * 3 + 2] = static_cast<uchar>(b0);

            if (x + 1 < width) {
                const int r1 = clampToByte(y1 + rAdd);
                const int g1 = clampToByte(y1 - gAdd);
                const int b1 = clampToByte(y1 + bAdd);

                dst[(x + 1) * 3 + 0] = static_cast<uchar>(r1);
                dst[(x + 1) * 3 + 1] = static_cast<uchar>(g1);
                dst[(x + 1) * 3 + 2] = static_cast<uchar>(b1);
            }
        }
    }

    outImage = image;
    return true;
}

} // namespace

Widget::Widget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Widget)
    , m_probe(new CameraProbe(this))
{
    ui->setupUi(this);
    initializePreview();

    connect(m_probe, SIGNAL(logMessage(QString)),
            this, SLOT(onProbeLog(QString)));
    connect(m_probe, SIGNAL(captureSucceeded(CapturedFrame)),
            this, SLOT(onProbeSuccess(CapturedFrame)));
    connect(m_probe, SIGNAL(captureFailed(QString)),
            this, SLOT(onProbeFailed(QString)));
}

Widget::~Widget()
{
    stopPreview();
    delete ui;
}

void Widget::initializePreview()
{
    m_viewfinder = new QCameraViewfinder(this);
    m_viewfinder->setObjectName("cameraPreview");
    m_viewfinder->setMinimumHeight(280);

    ui->verticalLayout->insertWidget(1, m_viewfinder, 1);
    startPreview();
}

void Widget::startPreview()
{
    if (!m_viewfinder) {
        return;
    }

    if (m_previewCamera && m_previewCamera->state() == QCamera::ActiveState) {
        return;
    }

    const QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    if (cameras.isEmpty()) {
        ui->plainTextEdit->appendPlainText(QStringLiteral("[WARN] Live preview is unavailable because no camera was found."));
        return;
    }

    const QCameraInfo info = cameras.first();

    qInfo().noquote() << QString("[CAMERA] selected preview device: desc=%1, dev=%2")
                         .arg(info.description())
                         .arg(info.deviceName());

    if (m_previewCamera) {
        m_previewCamera->stop();
        m_previewCamera->deleteLater();
        m_previewCamera = nullptr;
    }

    m_previewCamera = new QCamera(info, this);
    m_previewCamera->setCaptureMode(QCamera::CaptureViewfinder);
    m_previewCamera->setViewfinder(m_viewfinder);

    connect(m_previewCamera, SIGNAL(error(QCamera::Error)),
            this, SLOT(onPreviewCameraError(QCamera::Error)));

    m_previewCamera->start();
    ui->plainTextEdit->appendPlainText(
                QString("[INFO] Live preview started: %1")
                .arg(info.description()));

    QTimer::singleShot(300, this, [this]() {
        if (!m_previewCamera) {
            return;
        }

        const QCameraViewfinderSettings s = m_previewCamera->viewfinderSettings();
        const QSize r = s.resolution();
        const bool hasResolution = r.width() > 0 && r.height() > 0;
        const bool hasFormat = s.pixelFormat() != QVideoFrame::Format_Invalid;
        const bool hasFrameRate = s.minimumFrameRate() > 0.0 || s.maximumFrameRate() > 0.0;

        if (!hasResolution && !hasFormat && !hasFrameRate) {
            ui->plainTextEdit->appendPlainText(
                        QStringLiteral("[INFO] Preview default params: driver does not expose explicit default viewfinder settings."));
            return;
        }

        ui->plainTextEdit->appendPlainText(
                    QString("[INFO] Preview default params: resolution=%1x%2, format=%3, fps=[%4,%5]")
                    .arg(s.resolution().width())
                    .arg(s.resolution().height())
                    .arg(CameraProbe::pixelFormatToString(s.pixelFormat()))
                    .arg(s.minimumFrameRate())
                    .arg(s.maximumFrameRate()));
    });
}

void Widget::stopPreview()
{
    if (!m_previewCamera) {
        return;
    }

    m_previewCamera->stop();
    m_previewCamera->deleteLater();
    m_previewCamera = nullptr;
}

void Widget::on_btnListModes_clicked()
{
    ui->plainTextEdit->clear();

    const QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    ui->plainTextEdit->appendPlainText("=== Camera Summary ===");
    ui->plainTextEdit->appendPlainText(
                QString("camera count: %1").arg(cameras.size()));

    if (cameras.isEmpty()) {
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[ERROR] No camera is visible to this app."));
        return;
    }

    ui->plainTextEdit->appendPlainText("camera list:");
    for (int i = 0; i < cameras.size(); ++i) {
        const QCameraInfo &info = cameras[i];
        ui->plainTextEdit->appendPlainText(
                    QString("  [%1] %2")
                    .arg(i)
                    .arg(info.description()));

        qInfo().noquote() << QString("[CAMERA] camera[%1] device path: %2")
                             .arg(i)
                             .arg(info.deviceName());
    }

    const auto modes = CameraProbe::enumerateAllModes();
    QList<CameraModeInfo> validModes;
    validModes.reserve(modes.size());

    ui->plainTextEdit->appendPlainText("=== Mode Summary ===");
    ui->plainTextEdit->appendPlainText(
                QString("enumerated entries: %1").arg(modes.size()));

    for (const auto &m : modes) {
        const bool validMode = m.settings.resolution().width() > 0
                && m.settings.resolution().height() > 0
                && m.settings.pixelFormat() != QVideoFrame::Format_Invalid;
        if (validMode) {
            validModes.push_back(m);
        }
    }

    ui->plainTextEdit->appendPlainText(
                QString("valid entries: %1").arg(validModes.size()));

    if (validModes.isEmpty()) {
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[INFO] No explicit camera mode list was returned by the driver. Preview/capture will use camera default mode."));
        return;
    }

    ui->plainTextEdit->appendPlainText("top valid modes:");
    const int showCount = qMin(validModes.size(), 8);
    for (int i = 0; i < showCount; ++i) {
        const CameraModeInfo &m = validModes[i];
        ui->plainTextEdit->appendPlainText(
                    QString("  #%1 cam=%2 %3x%4 %5 fps[%6,%7]")
                    .arg(i + 1)
                    .arg(m.cameraIndex)
                    .arg(m.settings.resolution().width())
                    .arg(m.settings.resolution().height())
                    .arg(CameraProbe::pixelFormatToString(m.settings.pixelFormat()))
                    .arg(m.settings.minimumFrameRate())
                    .arg(m.settings.maximumFrameRate()));
    }

    if (validModes.size() > showCount) {
        ui->plainTextEdit->appendPlainText(
                    QString("  ... %1 more").arg(validModes.size() - showCount));
    }
}

void Widget::on_btnGrabOneFrame_clicked()
{
    const QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    if (cameras.isEmpty()) {
        ui->plainTextEdit->appendPlainText(QStringLiteral("[ERROR] No camera found."));
        return;
    }

    CameraModeInfo selected;
    QString reason;
    if (CameraProbe::findPreferredYuy2Mode(640, 480, selected, &reason)) {
        ui->plainTextEdit->appendPlainText(QStringLiteral("[INFO] ") + reason);
    } else {
        const QCameraInfo info = cameras.first();
        selected.cameraIndex = 0;
        selected.cameraInfo = info;
        selected.description = info.description();
        selected.deviceName = info.deviceName();

        selected.settings = QCameraViewfinderSettings();

        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[WARN] No usable YUY2/YUYV mode found. Falling back to camera default preview format."));
        ui->plainTextEdit->appendPlainText(QStringLiteral("[INFO] ") + reason);
    }

    ui->plainTextEdit->appendPlainText(
                QString("[INFO] 准备采集: camera=%1, dev=%2, resolution=%3x%4, format=%5, fps=[%6,%7]")
                .arg(selected.description)
                .arg(selected.deviceName)
                .arg(selected.settings.resolution().width())
                .arg(selected.settings.resolution().height())
                .arg(CameraProbe::pixelFormatToString(selected.settings.pixelFormat()))
                .arg(selected.settings.minimumFrameRate())
                .arg(selected.settings.maximumFrameRate()));

    stopPreview();
    m_restartPreviewAfterCapture = true;

    if (!m_probe->startSingleFrameCapture(selected)) {
        m_restartPreviewAfterCapture = false;
        startPreview();
    }
}

void Widget::onProbeLog(const QString &msg)
{
    ui->plainTextEdit->appendPlainText("[LOG] " + msg);
}

void Widget::onProbeSuccess(const CapturedFrame &frame)
{
    QString fmtTag = CameraProbe::pixelFormatToString(frame.pixelFormat).toLower();
    fmtTag.replace(QRegularExpression("[^a-z0-9]+"), "_");
    if (fmtTag.isEmpty()) {
        fmtTag = "raw";
    }

    const QString fileName =
            QString("frame_%1_%2x%3_%4.raw")
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"))
            .arg(frame.resolution.width())
            .arg(frame.resolution.height())
            .arg(fmtTag);

    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly)) {
        ui->plainTextEdit->appendPlainText(QStringLiteral("[ERROR] Cannot save raw file: ") + fileName);
        return;
    }

    const qint64 written = file.write(frame.payload);
    file.close();

    if (written != frame.payload.size()) {
        ui->plainTextEdit->appendPlainText(
                    QString("[ERROR] Incomplete file write: %1, expected=%2, written=%3")
                    .arg(fileName)
                    .arg(frame.payload.size())
                    .arg(written));
        return;
    }

    ui->plainTextEdit->appendPlainText(
                QString("[OK] One frame saved to %1, bytes=%2, size=%3x%4, format=%5, planes=%6, dev=%7")
                .arg(fileName)
                .arg(frame.payload.size())
                .arg(frame.resolution.width())
                .arg(frame.resolution.height())
                .arg(CameraProbe::pixelFormatToString(frame.pixelFormat))
                .arg(frame.planeCount)
                .arg(frame.cameraDeviceName));

    if (frame.pixelFormat == QVideoFrame::Format_YUYV) {
        QImage image;
        if (yuyvToRgbImage(frame, image)) {
            const QString pngFileName = fileName.left(fileName.lastIndexOf('.')) + ".png";
            if (image.save(pngFileName, "PNG")) {
                ui->plainTextEdit->appendPlainText(
                            QString("[OK] Preview image saved: %1")
                            .arg(pngFileName));
            } else {
                ui->plainTextEdit->appendPlainText(
                            QString("[WARN] Failed to save preview PNG: %1")
                            .arg(pngFileName));
            }
        } else {
            ui->plainTextEdit->appendPlainText(
                        QStringLiteral("[WARN] YUYV frame was captured, but PNG preview conversion failed."));
        }
    } else {
        ui->plainTextEdit->appendPlainText(
                    QString("[INFO] PNG preview is currently generated only for YUYV. Current format=%1")
                    .arg(CameraProbe::pixelFormatToString(frame.pixelFormat)));
    }

    if (m_restartPreviewAfterCapture) {
        m_restartPreviewAfterCapture = false;
        QTimer::singleShot(120, this, [this]() {
            startPreview();
        });
    }
}

void Widget::onProbeFailed(const QString &reason)
{
    ui->plainTextEdit->appendPlainText(QStringLiteral("[ERROR] ") + reason);

    if (m_restartPreviewAfterCapture) {
        m_restartPreviewAfterCapture = false;
        QTimer::singleShot(120, this, [this]() {
            startPreview();
        });
    }
}

void Widget::onPreviewCameraError(QCamera::Error error)
{
    Q_UNUSED(error);
    const QString msg = m_previewCamera ? m_previewCamera->errorString()
                                        : QStringLiteral("Unknown preview error");
    ui->plainTextEdit->appendPlainText(QStringLiteral("[WARN] Live preview error: ") + msg);
}
