#include "widget.h"
#include "ui_widget.h"

#include <QFile>
#include <QDateTime>
#include <QRegularExpression>
#include <QImage>
#include <QTimer>
#include <QDebug>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <vector>
#include <cstring>

// XDMA 辅助 API，由厂商 DLL 导出。
#include "xdmaDLL_public.h"

namespace {

// 句柄有效性判断：统一过滤空句柄与 INVALID_HANDLE_VALUE。
bool isValidHandle(HANDLE handle)
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

// 像素分量钳位到 [0,255]，避免颜色计算溢出。
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

// 将一帧 YUYV 原始数据转换为 RGB888 图像。
// 该函数用于“采一帧”后的 PNG 预览导出，不参与 XDMA 实时发送路径。
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

} // 匿名命名空间

Widget::Widget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Widget)
    , m_probe(new CameraProbe(this))
{
    // 初始化 UI、按钮默认状态和预览链路。
    ui->setupUi(this);
    ui->btnSendTestPacket->setEnabled(false);
    ui->btnSendCapturedFrame->setEnabled(false);
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
    // 析构顺序：先停数据链路（XDMA/预览），再释放 UI。
    closeXdmaHandles();
    stopPreview();
    delete ui;
}

// 初始化实时预览：创建 Viewfinder、插入布局、挂接 VideoProbe。
void Widget::initializePreview()
{
    m_viewfinder = new QCameraViewfinder(this);
    m_viewfinder->setObjectName("cameraPreview");
    m_viewfinder->setMinimumHeight(280);

    ui->verticalLayout->insertWidget(1, m_viewfinder, 1);
    initializeTransferControls();

    m_videoProbe = new QVideoProbe(this);
    connect(m_videoProbe, &QVideoProbe::videoFrameProbed,
            this, &Widget::onPreviewFrameProbed);

    startPreview();
}

// 在界面中动态创建传输调参区：
// - 节流间隔：控制实时帧发送最小间隔；
// - 分包大小：控制每次 write_device 的最大请求长度。
void Widget::initializeTransferControls()
{
    QWidget *panel = new QWidget(this);
    panel->setObjectName("transferTuningPanel");

    QHBoxLayout *row = new QHBoxLayout(panel);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    QLabel *throttleLabel = new QLabel(
                QString::fromWCharArray(L"\u8282\u6D41\u95F4\u9694(ms):"), panel);
    m_throttleSpin = new QSpinBox(panel);
    m_throttleSpin->setRange(5, 1000);
    m_throttleSpin->setSingleStep(5);
    m_throttleSpin->setValue(static_cast<int>(m_liveStreamThrottleMs));

    QLabel *chunkLabel = new QLabel(
                QString::fromWCharArray(L"\u5206\u5305\u5927\u5C0F(KB):"), panel);
    m_chunkSizeSpin = new QSpinBox(panel);
    m_chunkSizeSpin->setRange(64, 4096);
    m_chunkSizeSpin->setSingleStep(64);
    m_chunkSizeSpin->setValue(m_xdmaChunkBytes / 1024);

    row->addWidget(throttleLabel);
    row->addWidget(m_throttleSpin);
    row->addSpacing(16);
    row->addWidget(chunkLabel);
    row->addWidget(m_chunkSizeSpin);
    row->addStretch(1);

    connect(m_throttleSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int value) {
        // 运行时更新节流参数，无需重启预览或重建相机。
        m_liveStreamThrottleMs = qMax<qint64>(1, value);
        if (ui && ui->plainTextEdit) {
            ui->plainTextEdit->appendPlainText(
                        QString("[CFG] Live throttle interval set to %1 ms")
                        .arg(m_liveStreamThrottleMs));
        }
    });

    connect(m_chunkSizeSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int value) {
        // 运行时更新分包参数，后续发送立即生效。
        const int kb = qMax(1, value);
        m_xdmaChunkBytes = kb * 1024;
        if (ui && ui->plainTextEdit) {
            ui->plainTextEdit->appendPlainText(
                        QString("[CFG] XDMA chunk size set to %1 KB")
                        .arg(kb));
        }
    });

    ui->verticalLayout->insertWidget(2, panel);
}

// 启动实时预览。
// 若已有活动预览则直接返回，避免重复创建相机对象。
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

    if (m_videoProbe) {
        if (!m_videoProbe->setSource(m_previewCamera)) {
            ui->plainTextEdit->appendPlainText(
                        QStringLiteral("[WARN] Video probe attach failed. Live XDMA streaming may be unavailable."));
        }
    }

    connect(m_previewCamera, SIGNAL(error(QCamera::Error)),
            this, SLOT(onPreviewCameraError(QCamera::Error)));

    m_previewCamera->start();
    ui->plainTextEdit->appendPlainText(
                QString("[INFO] Live preview started: %1")
                .arg(info.description()));

    QTimer::singleShot(300, this, [this]() {
        // 延迟读取 viewfinderSettings，可提高拿到有效默认参数的概率。
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

// 停止并释放预览相机。
void Widget::stopPreview()
{
    if (!m_previewCamera) {
        return;
    }

    m_previewCamera->stop();
    m_previewCamera->deleteLater();
    m_previewCamera = nullptr;
}

// 关闭 XDMA 句柄并复位会话状态。
void Widget::closeXdmaHandles()
{
    // 重连或退出时都要关闭两类句柄。
    // 封装库内部通常用 CreateFile 打开通道，
    // 因此每个成功打开的通道都必须对应 CloseHandle。
    const HANDLE h2c = reinterpret_cast<HANDLE>(m_xdmaH2c0Handle);
    const HANDLE user = reinterpret_cast<HANDLE>(m_xdmaUserHandle);

    if (isValidHandle(h2c)) {
        CloseHandle(h2c);
    }
    if (isValidHandle(user)) {
        CloseHandle(user);
    }

    m_xdmaH2c0Handle = nullptr;
    m_xdmaUserHandle = nullptr;
    m_xdmaDevicePath.clear();

    if (ui && ui->btnSendTestPacket) {
        ui->btnSendTestPacket->setEnabled(false);
    }
}

// 打开 XDMA 并做基础自检：
// 1) 枚举设备；
// 2) 打开 user 通道；
// 3) 打开 h2c_0 通道；
// 4) 读取 ready_state。
bool Widget::openXdmaAndSelfCheck()
{
    // 重开策略：先清理旧句柄，再做一次完整打开流程。
    closeXdmaHandles();

    constexpr int kMaxDevices = 16;
    constexpr size_t kPathLength = 1024;

    std::vector<QByteArray> pathBuffers;
    pathBuffers.reserve(kMaxDevices);
    std::vector<char *> pathPtrs(kMaxDevices, nullptr);
    for (int i = 0; i < kMaxDevices; ++i) {
        pathBuffers.push_back(QByteArray(static_cast<int>(kPathLength), '\0'));
        pathPtrs[i] = pathBuffers[i].data();
    }

    // 1) 枚举 GUID_DEVINTERFACE_XDMA 对应的基础设备路径。
    // 封装函数会把每个设备路径写入传入的字符缓冲区。
    const int deviceCount = get_devices(GUID_DEVINTERFACE_XDMA, pathPtrs.data(), kPathLength);
    ui->plainTextEdit->appendPlainText(QString("[XDMA] detected devices: %1").arg(deviceCount));
    if (deviceCount <= 0) {
        ui->plainTextEdit->appendPlainText(QStringLiteral("[ERROR] XDMA device not found."));
        return false;
    }

    const int scanCount = qMin(deviceCount, kMaxDevices);
    int selectedIndex = -1;
    for (int i = 0; i < scanCount; ++i) {
        if (pathPtrs[i] && pathPtrs[i][0] != '\0') {
            qInfo().noquote() << QString("[XDMA] device[%1] path: %2").arg(i).arg(pathPtrs[i]);
            if (selectedIndex < 0) {
                selectedIndex = i;
            }
        }
    }

    if (selectedIndex < 0) {
        ui->plainTextEdit->appendPlainText(QStringLiteral("[ERROR] XDMA path list is empty."));
        return false;
    }

    QByteArray basePath = QByteArray(pathPtrs[selectedIndex]);
    if (basePath.isEmpty()) {
        ui->plainTextEdit->appendPlainText(QStringLiteral("[ERROR] Invalid XDMA base path."));
        return false;
    }

    m_xdmaDevicePath = QString::fromLocal8Bit(basePath.constData());
    ui->plainTextEdit->appendPlainText(
                QString("[XDMA] selected path index=%1").arg(selectedIndex));

    // 2) 先打开 user（控制/BAR）通道。
    HANDLE userHandle = nullptr;
    {
        QByteArray userPath = basePath;
        const int ok = open_devices(&userHandle,
                                    GENERIC_READ | GENERIC_WRITE,
                                    userPath.data(),
                                    XDMA_FILE_USER);
        if (ok != 1 || !isValidHandle(userHandle)) {
            ui->plainTextEdit->appendPlainText(QStringLiteral("[ERROR] Failed to open XDMA user channel."));
            return false;
        }
    }

    // 3) 打开 h2c_0 流发送通道，用于主机到 FPGA 的数据传输。
    HANDLE h2cHandle = nullptr;
    {
        QByteArray h2cPath = basePath;
        const int ok = open_devices(&h2cHandle,
                                    GENERIC_WRITE,
                                    h2cPath.data(),
                                    XDMA_FILE_H2C_0);
        if (ok != 1 || !isValidHandle(h2cHandle)) {
            ui->plainTextEdit->appendPlainText(QStringLiteral("[ERROR] Failed to open XDMA h2c_0 channel."));
            CloseHandle(userHandle);
            return false;
        }
    }

    m_xdmaUserHandle = reinterpret_cast<void *>(userHandle);
    m_xdmaH2c0Handle = reinterpret_cast<void *>(h2cHandle);

    // 4) 调用 ready_state 做自检（具体语义由厂商 API 定义）。
    unsigned int opState = 0;
    unsigned int ddrState = 0;
    const int readyRet = ready_state(userHandle, &opState, &ddrState);
    if (readyRet < 0) {
        ui->plainTextEdit->appendPlainText(
                    QString("[WARN] ready_state failed: ret=%1").arg(readyRet));
    } else {
        ui->plainTextEdit->appendPlainText(
                    QString("[OK] self-check: ready_state ret=%1, op=%2, ddr=%3")
                    .arg(readyRet)
                    .arg(opState)
                    .arg(ddrState));
    }

    ui->btnSendTestPacket->setEnabled(true);
    ui->plainTextEdit->appendPlainText(QStringLiteral("[OK] XDMA open complete: user + h2c_0 ready."));
    return true;
}

// 通用 XDMA 发送函数。
// 输入 payload 为“原始字节流”，函数内部负责：
// - 自动打开 h2c_0（必要时）；
// - 对齐缓冲区申请与拷贝；
// - 分块 write_device 循环发送；
// - 失败回滚与日志。
bool Widget::sendXdmaPayload(const QByteArray &payload, const QString &label, bool verbose)
{
    // XDMA 通用发送路径，供以下场景复用：
    // - 手动发送最近采集帧；
    // - 发送测试包；
    // - 实时视频流发送。
    // 流程：
    // 1) 如未打开通道则自动打开 XDMA；
    // 2) 将 payload 拷贝到对齐缓冲区（allocate_buffer）；
    // 3) 在 h2c_0 上分块写入，直到全部发送完成。
    if (payload.isEmpty()) {
        ui->plainTextEdit->appendPlainText(QString("[ERROR] %1 is empty, skip XDMA send.").arg(label));
        return false;
    }

    HANDLE h2c = reinterpret_cast<HANDLE>(m_xdmaH2c0Handle);
    if (!isValidHandle(h2c)) {
        ui->plainTextEdit->appendPlainText(QStringLiteral("[XDMA] h2c_0 is not open, trying auto-open..."));
        if (!openXdmaAndSelfCheck()) {
            ui->plainTextEdit->appendPlainText(QStringLiteral("[ERROR] XDMA auto-open failed."));
            return false;
        }
        h2c = reinterpret_cast<HANDLE>(m_xdmaH2c0Handle);
    }

    if (!isValidHandle(h2c)) {
        ui->plainTextEdit->appendPlainText(QStringLiteral("[ERROR] h2c_0 handle is invalid."));
        return false;
    }

    const int totalBytes = payload.size();
    BYTE *txBuffer = allocate_buffer(static_cast<size_t>(totalBytes), 0);
    if (!txBuffer) {
        ui->plainTextEdit->appendPlainText(QString("[ERROR] allocate_buffer failed for %1 (%2 bytes).")
                                           .arg(label)
                                           .arg(totalBytes));
        return false;
    }

    std::memcpy(txBuffer, payload.constData(), static_cast<size_t>(totalBytes));

    int sent = 0;
    // 分块发送可提升大包写入的稳定性，也更方便定位问题。
    const int chunkBytes = qMax(4 * 1024, m_xdmaChunkBytes);
    while (sent < totalBytes) {
        const int remain = totalBytes - sent;
        const DWORD req = static_cast<DWORD>(qMin(remain, chunkBytes));
        const int written = write_device(h2c, 0x00000000, req, txBuffer + sent);
        if (written <= 0) {
            free_buffer(txBuffer);
            ui->plainTextEdit->appendPlainText(
                        QString("[ERROR] XDMA write failed while sending %1: sent=%2/%3, ret=%4")
                        .arg(label)
                        .arg(sent)
                        .arg(totalBytes)
                        .arg(written));
            return false;
        }
        sent += written;
    }

    free_buffer(txBuffer);
    if (verbose) {
        ui->plainTextEdit->appendPlainText(
                    QString("[OK] XDMA sent %1: %2 bytes")
                    .arg(label)
                    .arg(sent));
    }
    return true;
}

// 发送固定测试包，用于验证 PC->FPGA H2C 通道连通性。
bool Widget::sendXdmaTestPacket()
{
    // 4KB 固定模式测试包，用于链路连通性验证。
    // 头部 [0..3]="XDMA"， [4..7]=序号。
    constexpr int packetSize = 4096;
    QByteArray payload(packetSize, '\0');
    BYTE *buffer = reinterpret_cast<BYTE *>(payload.data());

    for (int i = 0; i < packetSize; ++i) {
        buffer[i] = static_cast<BYTE>((i + 0x5A) & 0xFF);
    }

    buffer[0] = 'X';
    buffer[1] = 'D';
    buffer[2] = 'M';
    buffer[3] = 'A';
    static quint32 sequence = 0;
    ++sequence;
    std::memcpy(buffer + 4, &sequence, sizeof(sequence));

    return sendXdmaPayload(payload,
                           QString("test packet to h2c_0 (seq=%1)").arg(sequence));
}

// 列出相机与模式信息。
// 该函数用于联调阶段快速确认：设备可见性、驱动是否返回模式列表。
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

// 单帧抓取入口：优先请求 YUY2 指定分辨率，失败则回退默认模式。
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

// 发送“采一帧”缓存到的最近负载。
void Widget::on_btnSendCapturedFrame_clicked()
{
    // 解耦路径：采集与发送分离。
    // 本按钮发送“采一帧”缓存下来的最后一帧数据。
    if (m_lastCapturedFramePayload.isEmpty()) {
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[ERROR] No captured frame cached. Please click \"采一帧\" first."));
        return;
    }

    const QString label = m_lastCapturedFrameLabel.isEmpty()
            ? QStringLiteral("cached camera frame")
            : m_lastCapturedFrameLabel;
    sendXdmaPayload(m_lastCapturedFramePayload, label);
}

// 实时发送开关按钮。
// 开启后由 onPreviewFrameProbed 持续处理帧并发送；关闭后仅保留预览不发送。
void Widget::on_btnSendLiveVideo_clicked()
{
    // 运行时开关路径：预览帧 -> QVideoProbe -> sendXdmaPayload -> h2c_0。
    // 这里只切换“是否发送”，不改变相机采集参数。
    if (!m_liveVideoSending) {
        m_liveVideoSending = true;
        m_lastLiveSendMs = 0;
        m_liveSentFrames = 0;
                ui->btnSendLiveVideo->setText(QString::fromUtf8("停止发送实时视频到H2C"));
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[XDMA] Live camera streaming to h2c_0 started."));
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[XDMA] Live camera streaming to h2c_0 started."));
        return;
    }

    m_liveVideoSending = false;
    ui->btnSendLiveVideo->setText(QString::fromUtf8("开始发送实时视频到H2C"));
    ui->plainTextEdit->appendPlainText(
                QString("[XDMA] Live camera streaming stopped. sent frames=%1")
                .arg(m_liveSentFrames));
}

// 手动打开 XDMA 并执行 ready_state 自检。
void Widget::on_btnOpenXdma_clicked()
{
    // 手动打开+自检入口，便于联调和上电验证。
    // 实时发送和手动发送路径也支持自动打开兜底。
    openXdmaAndSelfCheck();
}

// 发送测试包按钮槽。
void Widget::on_btnSendTestPacket_clicked()
{
    // 无相机依赖的固定测试包路径，用于快速验证链路。
    sendXdmaTestPacket();
}

// 透传 CameraProbe 日志到界面。
void Widget::onProbeLog(const QString &msg)
{
    ui->plainTextEdit->appendPlainText("[LOG] " + msg);
}

// 单帧抓取成功回调：
// 1) 保存 raw；
// 2) 缓存 payload 供手动 XDMA 发送；
// 3) 若格式是 YUYV，则额外导出 PNG 预览。
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

    m_lastCapturedFramePayload = frame.payload;
    m_lastCapturedFrameLabel = QString("captured frame %1x%2 %3")
            .arg(frame.resolution.width())
            .arg(frame.resolution.height())
            .arg(CameraProbe::pixelFormatToString(frame.pixelFormat));
    ui->btnSendCapturedFrame->setEnabled(true);
    ui->plainTextEdit->appendPlainText(
                QString("[INFO] Frame cached for manual XDMA send: %1 bytes")
                .arg(m_lastCapturedFramePayload.size()));

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
        // 抓帧结束后延迟恢复预览，降低重入风险。
        m_restartPreviewAfterCapture = false;
        QTimer::singleShot(120, this, [this]() {
            startPreview();
        });
    }
}

// 单帧抓取失败回调：记录原因并按需恢复预览。
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

// 预览相机错误回调。
void Widget::onPreviewCameraError(QCamera::Error error)
{
    Q_UNUSED(error);
    const QString msg = m_previewCamera ? m_previewCamera->errorString()
                                        : QStringLiteral("Unknown preview error");
    ui->plainTextEdit->appendPlainText(QStringLiteral("[WARN] Live preview error: ") + msg);
}

// 预览帧回调（实时发送主链路）：
// 1) 根据开关和节流条件决定是否发送；
// 2) map 帧并拷贝原始字节；
// 3) 调用 sendXdmaPayload 发送到 h2c_0；
// 4) 错误时自动停流并回退 UI 状态。
void Widget::onPreviewFrameProbed(const QVideoFrame &frame)
{
    if (!m_liveVideoSending) {
        return;
    }

    // 发送节流：限制发送速率，避免 PCIe/H2C 被灌满，
    // 同时降低 GUI 线程压力。
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_lastLiveSendMs > 0 && (nowMs - m_lastLiveSendMs) < m_liveStreamThrottleMs) {
        return;
    }

    if (!frame.isValid()) {
        return;
    }

    // 将视频帧映射到 CPU 可访问内存，再拷贝到 QByteArray。
    // 负载数据的布局由驱动给出的像素格式与 stride 决定。
    QVideoFrame copy(frame);
    if (!copy.map(QAbstractVideoBuffer::ReadOnly)) {
        return;
    }

    const int bytes = copy.mappedBytes();
    const uchar *bits = copy.bits();
    const int width = copy.width();
    const int height = copy.height();
    const QVideoFrame::PixelFormat fmt = copy.pixelFormat();

    QByteArray payload;
    if (bytes > 0 && bits) {
        payload.append(reinterpret_cast<const char *>(bits), bytes);
    }

    copy.unmap();

    if (payload.isEmpty()) {
        return;
    }

    m_lastLiveSendMs = nowMs;
    const bool ok = sendXdmaPayload(payload,
                                    QString("live frame %1x%2 %3")
                                    .arg(width)
                                    .arg(height)
                                    .arg(CameraProbe::pixelFormatToString(fmt)),
                                    false);

    if (!ok) {
        // 发送失败即停流，避免持续错误刷屏和驱动压力累积。
        m_liveVideoSending = false;
        ui->btnSendLiveVideo->setText(QString::fromUtf8("开始发送实时视频到H2C"));
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[ERROR] Live camera streaming stopped due to XDMA write failure."));
        return;
    }

    ++m_liveSentFrames;
    if ((m_liveSentFrames % 30) == 0) {
        ui->plainTextEdit->appendPlainText(
                    QString("[XDMA] Live stream progress: sent frames=%1")
                    .arg(m_liveSentFrames));
    }
}
