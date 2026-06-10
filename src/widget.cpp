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
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSet>
#include <QVector>
#include <QMetaType>
#include <vector>
#include <cstring>

// XDMA 辅助 API，由厂商 DLL 导出。
#include "xdmaDLL_public_linux.h"

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

// 统一输出 32bit 十六进制文本（大写、固定 8 位）。
// 用于寄存器读写日志与“读回值”展示，减少格式不一致问题。
QString toHex32(quint32 value)
{
    return QString("0x%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
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

constexpr int kYuyvBytesPerPixel = 2;

bool isValidPreviewMode(const LinuxPreviewMode &mode)
{
    return mode.isValid();
}

int firstPreviewModeIndex(const QList<LinuxPreviewMode> &modes)
{
    for (int i = 0; i < modes.size(); ++i) {
        if (isValidPreviewMode(modes[i])) {
            return i;
        }
    }
    return -1;
}

QString modeToComboText(const LinuxPreviewMode &mode)
{
    return mode.displayText();
}

} // 匿名命名空间

Widget::Widget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Widget)
    , m_probe(new CameraProbe(this))
{
    // 初始化 UI、按钮默认状态和预览链路。
    ui->setupUi(this);
    // 软件协议自测不依赖 XDMA，可直接手动触发。
    ui->btnSendTestPacket->setEnabled(true);
    // 硬件链路测试包依赖 XDMA 通道就绪，初始禁用。
    ui->btnSendLinkTestPacket->setEnabled(false);
    ui->btnSendCapturedFrame->setEnabled(false);
    qRegisterMetaType<CapturedFrame>("CapturedFrame");
    qRegisterMetaType<LinuxAcceptedMode>("LinuxAcceptedMode");
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
    m_liveVideoSending = false;
    if (m_probe) {
        m_probe->stopCapture();
    }
    closeXdmaHandles();
    stopPreview();
    delete ui;
}

// 初始化实时预览：创建 native 预览窗口并挂接 LinuxPreviewSession。
void Widget::initializePreview()
{
    m_previewWidget = new QWidget(this);
    m_previewWidget->setObjectName("cameraPreview");
    m_previewWidget->setMinimumHeight(280);
    m_previewWidget->setAttribute(Qt::WA_NativeWindow, true);
    m_previewWidget->setAttribute(Qt::WA_DontCreateNativeAncestors, true);
    m_previewWidget->setAutoFillBackground(true);
    m_previewWidget->setStyleSheet(QStringLiteral("background: #050505;"));

    ui->verticalLayout->insertWidget(1, m_previewWidget, 1);

    m_previewSession = new LinuxPreviewSession(this);
    connect(m_previewSession, &LinuxPreviewSession::logMessage,
            this, &Widget::onPreviewLog);
    connect(m_previewSession, &LinuxPreviewSession::acceptedModeChanged,
            this, &Widget::onAcceptedPreviewModeChanged);
    connect(m_previewSession, &LinuxPreviewSession::rawFrameAvailable,
            this, &Widget::onRawPreviewFrameAvailable);
    connect(m_previewSession, &LinuxPreviewSession::rawFrameFailed,
            this, &Widget::onRawPreviewFrameFailed);

    initializeModeControls();
    initializeTransferControls();
    initializeAxiLiteControls();

    startPreview();
}

void Widget::initializeModeControls()
{
    QWidget *panel = new QWidget(this);
    panel->setObjectName("cameraModePanel");

    QHBoxLayout *row = new QHBoxLayout(panel);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    QLabel *modeLabel = new QLabel(QString::fromWCharArray(L"\u6444\u50CF\u5934\u6A21\u5F0F:"), panel);
    m_modeCombo = new QComboBox(panel);
    m_modeCombo->setMinimumWidth(360);
    m_applyModeBtn = new QPushButton(QString::fromWCharArray(L"\u5E94\u7528\u6A21\u5F0F"), panel);

    row->addWidget(modeLabel);
    row->addWidget(m_modeCombo, 1);
    row->addWidget(m_applyModeBtn);

    connect(m_applyModeBtn, &QPushButton::clicked,
            this, &Widget::applySelectedModeFromCombo);
    connect(m_modeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) {
        applySelectedModeFromCombo();
    });

    ui->verticalLayout->insertWidget(2, panel);
    refreshModeCombo();
}

void Widget::refreshModeCombo()
{
    if (!m_modeCombo) {
        return;
    }

    const QString previousText = m_modeCombo->currentText();
    const QSignalBlocker blocker(m_modeCombo);
    m_modeCombo->clear();
    m_availablePreviewModes.clear();

    QStringList enumLogs;
    const QList<LinuxPreviewMode> modes = LinuxPreviewSession::enumerateModes(&enumLogs);
    for (const QString &line : enumLogs) {
        if (ui && ui->plainTextEdit) {
            ui->plainTextEdit->appendPlainText(line);
        }
    }

    QSet<QString> dedup;
    for (const LinuxPreviewMode &mode : modes) {
        if (!mode.isValid()) {
            continue;
        }

        const QString key = QString("%1|%2x%3|%4|%5|%6")
                .arg(mode.devicePath)
                .arg(mode.resolution.width())
                .arg(mode.resolution.height())
                .arg(mode.fourcc)
                .arg(mode.fpsNumerator)
                .arg(mode.fpsDenominator);
        if (dedup.contains(key)) {
            continue;
        }
        dedup.insert(key);
        m_availablePreviewModes.push_back(mode);
    }

    for (const LinuxPreviewMode &mode : m_availablePreviewModes) {
        m_modeCombo->addItem(modeToComboText(mode));
    }

    if (m_modeCombo->count() == 0) {
        m_modeCombo->addItem(QStringLiteral("No discrete YUYV V4L2 mode available"));
        m_modeCombo->setEnabled(false);
        m_useManualPreviewMode = false;
        if (m_applyModeBtn) {
            m_applyModeBtn->setEnabled(false);
        }
        return;
    }

    m_modeCombo->setEnabled(true);
    if (m_applyModeBtn) {
        m_applyModeBtn->setEnabled(true);
    }

    int targetIndex = firstPreviewModeIndex(m_availablePreviewModes);
    if (targetIndex < 0) {
        targetIndex = 0;
    }
    if (!previousText.isEmpty()) {
        const int found = m_modeCombo->findText(previousText);
        if (found >= 0) {
            targetIndex = found;
        }
    }

    m_modeCombo->setCurrentIndex(targetIndex);
    if (targetIndex >= 0 && targetIndex < m_availablePreviewModes.size()) {
        m_manualPreviewMode = m_availablePreviewModes[targetIndex];
        m_useManualPreviewMode = true;
    }
}

void Widget::applySelectedModeFromCombo()
{
    if (!m_modeCombo || m_availablePreviewModes.isEmpty()) {
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[WARN] No selectable camera mode is available."));
        return;
    }

    const int index = m_modeCombo->currentIndex();
    if (index < 0 || index >= m_availablePreviewModes.size()) {
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[WARN] Please select a valid camera mode."));
        return;
    }

    if (m_liveVideoSending) {
        stopLiveVideoSending(QStringLiteral("camera mode switch"));
    }
    clearLiveVideoBuffers();

    m_manualPreviewMode = m_availablePreviewModes[index];
    m_useManualPreviewMode = true;
    m_hasAcceptedPreviewMode = false;
    ui->plainTextEdit->appendPlainText(
                QString("[INFO] Applying mode: %1")
                .arg(modeToComboText(m_manualPreviewMode)));

    stopPreview();
    startPreview();
}

// 在界面中动态创建传输调参区：
// - 节流间隔：控制实时帧发送最小间隔；
// - 写入大小：控制视频主链路每次向 XDMA 写入的批次长度。
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
    m_throttleSpin->setRange(0, 1000);
    m_throttleSpin->setSingleStep(5);
    m_throttleSpin->setValue(static_cast<int>(m_liveStreamThrottleMs));

    QLabel *chunkLabel = new QLabel(
                QString::fromWCharArray(L"\u5199\u5165\u5927\u5C0F(KB):"), panel);
    m_chunkSizeSpin = new QSpinBox(panel);
    // 允许按 1KB 粒度调节，最小 1KB（正好 1 个协议包）。
    m_chunkSizeSpin->setRange(1, 4096);
    m_chunkSizeSpin->setSingleStep(1);
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
        m_liveStreamThrottleMs = qMax<qint64>(0, value);
        if (ui && ui->plainTextEdit) {
            ui->plainTextEdit->appendPlainText(
                        QString("[CFG] Live throttle interval set to %1 ms")
                        .arg(m_liveStreamThrottleMs));
        }
    });

    connect(m_chunkSizeSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int value) {
        // 运行时更新写入批次参数，后续发送立即生效。
        const int kb = qMax(1, value);
        const int newBatchBytes = kb * 1024;
        const bool ok = m_videoPacketBatcher.setBatchBytes(newBatchBytes);
        if (ok) {
            m_xdmaChunkBytes = newBatchBytes;
        }
        if (ui && ui->plainTextEdit) {
            if (ok) {
                ui->plainTextEdit->appendPlainText(
                            QString("[CFG] XDMA write size set to %1 KB")
                            .arg(kb));
            } else {
                ui->plainTextEdit->appendPlainText(
                            QString("[WARN] Invalid XDMA write size: %1 KB, keep %2 KB")
                            .arg(kb)
                            .arg(m_videoPacketBatcher.batchBytes() / 1024));
            }
        }
    });

    ui->verticalLayout->insertWidget(3, panel);

    // 让封包聚合模块与 UI 初始值保持一致。
    m_videoPacketBatcher.setBatchBytes(m_xdmaChunkBytes);
}

// 在界面中动态创建 AXI lite 寄存器调试区：
// - 地址输入：支持十六进制（0x）或十进制；
// - 写值输入：用于 32bit 写寄存器；
// - 读值显示：展示最近一次读取结果；
// - 读/写按钮：通过 XDMA user 通道访问 AXI lite。
void Widget::initializeAxiLiteControls()
{
    // 该 panel 放在主界面布局中，提供 AXI lite 读写入口。
    // 采用“输入地址 + 读/写按钮 + 读回显示”的最小调试闭环。
    QWidget *panel = new QWidget(this);
    panel->setObjectName("axiLiteRegPanel");

    QHBoxLayout *row = new QHBoxLayout(panel);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    QLabel *addrLabel = new QLabel(
                QString::fromWCharArray(L"寄存器地址:"), panel);
    m_regAddrEdit = new QLineEdit(panel);
    m_regAddrEdit->setPlaceholderText("0x00000000");
    m_regAddrEdit->setText("0x00000000");
    m_regAddrEdit->setMaximumWidth(130);

    QLabel *writeLabel = new QLabel(
                QString::fromWCharArray(L"写入值:"), panel);
    m_regWriteValueEdit = new QLineEdit(panel);
    m_regWriteValueEdit->setPlaceholderText("0x00000000");
    m_regWriteValueEdit->setText("0x00000000");
    m_regWriteValueEdit->setMaximumWidth(130);

    QPushButton *readBtn = new QPushButton(
                QString::fromWCharArray(L"读寄存器"), panel);
    QPushButton *writeBtn = new QPushButton(
                QString::fromWCharArray(L"写寄存器"), panel);

    QLabel *readbackLabel = new QLabel(
                QString::fromWCharArray(L"读回值:"), panel);
    m_regReadValueEdit = new QLineEdit(panel);
    m_regReadValueEdit->setReadOnly(true);
    m_regReadValueEdit->setText("0x00000000");
    m_regReadValueEdit->setMaximumWidth(130);

    row->addWidget(addrLabel);
    row->addWidget(m_regAddrEdit);
    row->addWidget(writeLabel);
    row->addWidget(m_regWriteValueEdit);
    row->addWidget(readBtn);
    row->addWidget(writeBtn);
    row->addWidget(readbackLabel);
    row->addWidget(m_regReadValueEdit);
    row->addStretch(1);

    connect(readBtn, &QPushButton::clicked, this, [this]() {
        // 读寄存器流程：
        // 1) 解析地址；
        // 2) 调用 user 通道 read_device；
        // 3) 更新读回框并写日志。
        quint32 address = 0;
        if (!parseUiRegisterValue(m_regAddrEdit ? m_regAddrEdit->text() : QString(),
                                  address,
                                  QStringLiteral("register address"))) {
            return;
        }

        quint32 value = 0;
        if (!readUserRegister(address, value)) {
            return;
        }

        if (m_regReadValueEdit) {
            m_regReadValueEdit->setText(toHex32(value));
        }
        ui->plainTextEdit->appendPlainText(
                    QString("[AXIL] READ  addr=%1 -> value=%2")
                    .arg(toHex32(address))
                    .arg(toHex32(value)));
    });

    connect(writeBtn, &QPushButton::clicked, this, [this]() {
        // 写寄存器流程：
        // 1) 解析地址与写值；
        // 2) 调用 user 通道 write_device；
        // 3) 写日志确认本次写入参数。
        quint32 address = 0;
        quint32 value = 0;
        if (!parseUiRegisterValue(m_regAddrEdit ? m_regAddrEdit->text() : QString(),
                                  address,
                                  QStringLiteral("register address"))) {
            return;
        }
        if (!parseUiRegisterValue(m_regWriteValueEdit ? m_regWriteValueEdit->text() : QString(),
                                  value,
                                  QStringLiteral("register write value"))) {
            return;
        }

        if (!writeUserRegister(address, value)) {
            return;
        }

        ui->plainTextEdit->appendPlainText(
                    QString("[AXIL] WRITE addr=%1 <- value=%2")
                    .arg(toHex32(address))
                    .arg(toHex32(value)));
    });

    ui->verticalLayout->insertWidget(4, panel);
}

// 启动实时预览。
// 若已有活动预览则直接返回，避免重复创建 GStreamer 管线。
void Widget::startPreview()
{
    if (!m_previewWidget || !m_previewSession) {
        return;
    }

    if (m_previewSession->isRunning()) {
        return;
    }

    if (!m_useManualPreviewMode || !m_manualPreviewMode.isValid()) {
        refreshModeCombo();
    }
    if (!m_useManualPreviewMode || !m_manualPreviewMode.isValid()) {
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[WARN] Live preview is unavailable because no discrete YUYV V4L2 mode was found."));
        return;
    }

    ui->plainTextEdit->appendPlainText(
                QString("[INFO] Preview requested mode: %1")
                .arg(m_manualPreviewMode.displayText()));

    QString reason;
    if (!m_previewSession->start(m_manualPreviewMode, m_previewWidget, &reason)) {
        ui->plainTextEdit->appendPlainText(
                    QString("[ERROR] Linux preview start failed: %1")
                    .arg(reason));
    }
}

// 停止并释放 Linux 预览管线。
void Widget::stopPreview()
{
    if (m_previewSession) {
        m_previewSession->stop();
    }
}

void Widget::clearLiveVideoBuffers()
{
    m_videoPacketBatcher.clear();
    m_liveReadyBatches.clear();
}

void Widget::stopLiveVideoSending(const QString &reason)
{
    if (!m_liveVideoSending) {
        return;
    }

    const int droppedPayloadBytes = m_videoPacketBatcher.pendingPayloadBytes();

    int queuedBatchBytes = 0;
    for (const QByteArray &batch : m_liveReadyBatches) {
        queuedBatchBytes += batch.size();
    }
    const int cachedNotSentBytes = m_videoPacketBatcher.pendingBytes() + queuedBatchBytes;

    const qint64 elapsedMs = m_liveSendStartMs > 0
            ? qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - m_liveSendStartMs)
            : 0;

    m_liveVideoSending = false;
    m_liveSendStartMs = 0;
    if (m_previewSession) {
        m_previewSession->setRawFrameDeliveryEnabled(false);
    }
    ui->btnSendLiveVideo->setText(QString::fromUtf8("开始实时视频发送(封包+批量)"));

    const QString suffix = reason.isEmpty()
            ? QString()
            : QStringLiteral(", reason=%1").arg(reason);
    ui->plainTextEdit->appendPlainText(
                QString("[XDMA] Live camera streaming stopped. duration=%1 ms, sent batches=%2, cached-not-sent=%3 bytes, dropped-tail=%4 bytes%5")
                .arg(elapsedMs)
                .arg(m_liveSentBatches)
                .arg(cachedNotSentBytes)
                .arg(droppedPayloadBytes)
                .arg(suffix));

    clearLiveVideoBuffers();
}

bool Widget::selectedModeSupportsLiveStreaming(QString *reason) const
{
    if (m_useManualPreviewMode && m_manualPreviewMode.isValid()) {
        return true;
    }

    if (reason) {
        *reason = QStringLiteral("Live XDMA streaming requires a selectable discrete V4L2 YUYV mode.");
    }
    return false;
}

bool Widget::normalizeLiveYuyvFrame(const CapturedFrame &frame,
                                    const QSize &expectedResolution,
                                    QByteArray &payload,
                                    QString *reason) const
{
    payload.clear();

    const int expectedWidth = expectedResolution.width();
    const int expectedHeight = expectedResolution.height();
    if (expectedWidth <= 0 || expectedHeight <= 0) {
        if (reason) {
            *reason = QString("invalid expected YUYV resolution=%1x%2")
                    .arg(expectedWidth)
                    .arg(expectedHeight);
        }
        return false;
    }

    if (frame.pixelFormat != QVideoFrame::Format_YUYV) {
        if (reason) {
            *reason = QString("actualFormat=%1, expected=YUYV")
                    .arg(CameraProbe::pixelFormatToString(frame.pixelFormat));
        }
        return false;
    }

    if (frame.resolution != expectedResolution) {
        if (reason) {
            *reason = QString("actualSize=%1x%2, expected=%3x%4")
                    .arg(frame.resolution.width())
                    .arg(frame.resolution.height())
                    .arg(expectedWidth)
                    .arg(expectedHeight);
        }
        return false;
    }

    const int expectedLineBytes = expectedWidth * kYuyvBytesPerPixel;
    const int expectedFrameBytes = expectedLineBytes * expectedHeight;
    const int bytesPerLine = (!frame.bytesPerLines.isEmpty() && frame.bytesPerLines.first() > 0)
            ? frame.bytesPerLines.first()
            : expectedLineBytes;

    if (bytesPerLine < expectedLineBytes) {
        if (reason) {
            *reason = QString("bytesPerLine=%1, expected at least %2")
                    .arg(bytesPerLine)
                    .arg(expectedLineBytes);
        }
        return false;
    }

    const int requiredMappedBytes = bytesPerLine * expectedHeight;
    if (frame.payload.size() < requiredMappedBytes) {
        if (reason) {
            *reason = QString("payload=%1B, required=%2B")
                    .arg(frame.payload.size())
                    .arg(requiredMappedBytes);
        }
        return false;
    }

    if (bytesPerLine == expectedLineBytes) {
        payload = frame.payload.left(expectedFrameBytes);
        return payload.size() == expectedFrameBytes;
    }

    payload.resize(expectedFrameBytes);
    const char *src = frame.payload.constData();
    char *dst = payload.data();
    for (int y = 0; y < expectedHeight; ++y) {
        std::memcpy(dst + y * expectedLineBytes,
                    src + y * bytesPerLine,
                    expectedLineBytes);
    }

    ui->plainTextEdit->appendPlainText(
                QString("[RAW] Normalized stride frame: size=%1x%2, bytesPerLine=%3 -> payload=%4B")
                .arg(expectedWidth)
                .arg(expectedHeight)
                .arg(bytesPerLine)
                .arg(payload.size()));
    return true;
}

// ===== 模块：UI 操作入口（按钮槽）与采集回调 =====
// 说明：
// 1) 先放“用户可直接触发”的按钮入口，便于从上到下理解业务流程；
// 2) 再放采集/预览回调，形成完整的“入口 -> 回调 -> 底层发送”阅读路径。

// ----- 子模块：采集与视频发送按钮 -----
// 列出相机与模式信息。
// 该函数用于联调阶段快速确认：设备可见性、驱动是否返回模式列表。
void Widget::on_btnListModes_clicked()
{
    ui->plainTextEdit->clear();

    QStringList enumLogs;
    const QList<LinuxPreviewMode> previewModes = LinuxPreviewSession::enumerateModes(&enumLogs);
    ui->plainTextEdit->appendPlainText("=== V4L2 Preview Mode Summary ===");
    ui->plainTextEdit->appendPlainText(
                QString("discrete YUYV entries: %1").arg(previewModes.size()));
    for (const QString &line : enumLogs) {
        ui->plainTextEdit->appendPlainText(line);
    }
    const int showPreviewCount = qMin(previewModes.size(), 12);
    for (int i = 0; i < showPreviewCount; ++i) {
        ui->plainTextEdit->appendPlainText(
                    QString("  #%1 %2")
                    .arg(i + 1)
                    .arg(previewModes[i].displayText()));
    }
    if (previewModes.size() > showPreviewCount) {
        ui->plainTextEdit->appendPlainText(
                    QString("  ... %1 more").arg(previewModes.size() - showPreviewCount));
    }

    refreshModeCombo();

    const QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    ui->plainTextEdit->appendPlainText("=== Qt Single-frame Camera Summary ===");
    ui->plainTextEdit->appendPlainText(
                QString("camera count: %1").arg(cameras.size()));

    if (cameras.isEmpty()) {
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[WARN] No Qt camera is visible to CameraProbe."));
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

    ui->plainTextEdit->appendPlainText("=== Qt Single-frame Mode Summary ===");
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

    if (m_liveVideoSending) {
        stopLiveVideoSending(QStringLiteral("single-frame capture"));
    }
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
    // 优先发送内存中的“最近一次抓取成功帧”，确保总是使用最新缓存图像。
    QByteArray payload = m_lastCapturedFramePayload;
    QString label = m_lastCapturedFrameLabel;
    if (payload.isEmpty()) {
        // 兜底：兼容旧流程（仅记录了落盘路径时）。
        if (m_lastSavedRawFilePath.isEmpty()) {
            ui->plainTextEdit->appendPlainText(
                        QStringLiteral("[ERROR] No cached frame. Please click \"采一帧\" first."));
            return;
        }

        QFile file(m_lastSavedRawFilePath);
        if (!file.open(QIODevice::ReadOnly)) {
            ui->plainTextEdit->appendPlainText(
                        QString("[ERROR] Cannot open saved raw file: %1")
                        .arg(m_lastSavedRawFilePath));
            return;
        }

        payload = file.readAll();
        file.close();

        if (payload.isEmpty()) {
            ui->plainTextEdit->appendPlainText(
                        QString("[ERROR] Saved raw file is empty: %1")
                        .arg(m_lastSavedRawFilePath));
            return;
        }

        label = QString("saved raw %1").arg(m_lastSavedRawFilePath);
    }

    constexpr int kCachedFrameFixedPacketCount = 460;
    int packetCount = 0;
    const QByteArray packetStream =
            m_videoPacketBatcher.buildPacketStream(payload,
                                                   &packetCount,
                                                   kCachedFrameFixedPacketCount);
    if (packetStream.isEmpty()) {
        ui->plainTextEdit->appendPlainText(
                    QString("[ERROR] Packetization failed for cached frame: %1")
                    .arg(label));
        return;
    }

    ui->plainTextEdit->appendPlainText(
                QString("[PKG][DIRECT] %1 raw=%2B -> packets=%3 (%4B), fixed-min=%5")
                .arg(label)
                .arg(payload.size())
                .arg(packetCount)
                .arg(packetStream.size())
                .arg(kCachedFrameFixedPacketCount));

    sendXdmaPayload(packetStream,
                    QString("%1 [direct packet stream]").arg(label),
                    true,
                    false);
}

// 实时发送开关按钮。
// 开启后由 LinuxPreviewSession appsink 持续送出 raw YUYV；关闭后仅保留预览。
void Widget::on_btnSendLiveVideo_clicked()
{
    // 运行时开关路径：GStreamer appsink -> sendVideoPayloadWithBatching -> h2c_0。
    // 这里只切换“是否发送”，相机采集参数由当前 UI 模式决定。
    if (!m_liveVideoSending) {
        QString modeReason;
        if (!selectedModeSupportsLiveStreaming(&modeReason)) {
            ui->plainTextEdit->appendPlainText(QStringLiteral("[ERROR] Cannot start live streaming: ") + modeReason);
            return;
        }
        if (!m_previewSession || !m_previewSession->isRunning()) {
            startPreview();
        }
        if (!m_previewSession || !m_previewSession->isRunning()) {
            ui->plainTextEdit->appendPlainText(
                        QStringLiteral("[ERROR] Cannot start live streaming: Linux preview session is not active."));
            return;
        }

        clearLiveVideoBuffers();
        m_liveVideoSending = true;
        m_previewSession->setRawFrameDeliveryEnabled(true);
        m_liveSendStartMs = QDateTime::currentMSecsSinceEpoch();
        m_lastLiveSendMs = 0;
        m_liveSentBatches = 0;
        ui->btnSendLiveVideo->setText(QString::fromUtf8("停止实时视频发送(封包+批量)"));
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[XDMA] Live camera streaming to h2c_0 started."));
        return;
    }

    stopLiveVideoSending();
}

// ----- 子模块：XDMA 与测试按钮 -----
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
    // 手动触发软件侧封包/聚合自测：
    // 1) 验证 1024B 包头字段、length 与补零规则；
    // 2) 验证默认配置下 1024 包是否准确聚合为 1MiB。
    runPacketModuleSelfTest();
}

// XDMA 链路测试包按钮槽。
void Widget::on_btnSendLinkTestPacket_clicked()
{
    // 无相机依赖的固定测试包路径，用于快速验证 PC->FPGA H2C 链路。
    // 与“软件协议自测”分开，避免测试职责混用。
    sendXdmaTestPacket();
}

// ----- 子模块：采集与预览回调 -----
// 透传 CameraProbe 日志到界面。
void Widget::onProbeLog(const QString &msg)
{
    ui->plainTextEdit->appendPlainText("[LOG] " + msg);
}

// 单帧抓取成功回调：
// 1) 刷新“最近一次帧”内存缓存，供手动 XDMA 发送优先使用；
// 2) 保存 raw 到磁盘，便于落盘留存与离线分析；
// 3) 若格式是 YUYV，则额外导出 PNG 预览。
void Widget::onProbeSuccess(const CapturedFrame &frame)
{
    m_lastCapturedFramePayload = frame.payload;
    m_lastCapturedFrameLabel = QString("cached frame %1x%2 %3")
            .arg(frame.resolution.width())
            .arg(frame.resolution.height())
            .arg(CameraProbe::pixelFormatToString(frame.pixelFormat));
    ui->btnSendCapturedFrame->setEnabled(!m_lastCapturedFramePayload.isEmpty());

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

    m_lastSavedRawFilePath = fileName;
    ui->plainTextEdit->appendPlainText(
                QString("[INFO] Cached frame refreshed for manual XDMA send; saved raw file: %1")
                .arg(m_lastSavedRawFilePath));

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

// Linux 预览日志回调。
void Widget::onPreviewLog(const QString &msg)
{
    ui->plainTextEdit->appendPlainText(msg);
}

// V4L2 accepted mode 回调：记录驱动实际接受的采集参数。
void Widget::onAcceptedPreviewModeChanged(const LinuxAcceptedMode &mode)
{
    m_acceptedPreviewMode = mode;
    m_hasAcceptedPreviewMode = true;
    ui->plainTextEdit->appendPlainText(
                QString("[INFO] Preview accepted mode: %1")
                .arg(mode.displayText()));
}

void Widget::onRawPreviewFrameFailed(const QString &reason)
{
    ui->plainTextEdit->appendPlainText(QStringLiteral("[WARN] Raw preview frame failed: ") + reason);
    if (m_liveVideoSending) {
        stopLiveVideoSending(QStringLiteral("raw preview failure"));
    }
}

// 预览 raw 帧回调（实时发送主链路）：
// 1) 根据开关和节流条件决定是否发送；
// 2) 校验并归一化 raw YUYV stride；
// 3) 调用 sendVideoPayloadWithBatching 完成“封包+聚合+发送”；
// 4) 错误时自动停流并回退 UI 状态。
void Widget::onRawPreviewFrameAvailable(const CapturedFrame &frame)
{
    if (!m_liveVideoSending) {
        return;
    }

    // 发送节流：限制发送速率，避免 PCIe/H2C 被灌满，
    // 同时降低 GUI 线程压力。
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_liveStreamThrottleMs > 0
            && m_lastLiveSendMs > 0
            && (nowMs - m_lastLiveSendMs) < m_liveStreamThrottleMs) {
        return;
    }

    if (frame.payload.isEmpty()) {
        return;
    }

    QByteArray payload;
    QString reason;
    const QSize expectedResolution = m_hasAcceptedPreviewMode
            ? m_acceptedPreviewMode.resolution
            : m_manualPreviewMode.resolution;
    if (!normalizeLiveYuyvFrame(frame, expectedResolution, payload, &reason)) {
        ui->plainTextEdit->appendPlainText(QStringLiteral("[WARN] Drop raw live frame: ") + reason);
        return;
    }

    const bool ok = sendVideoPayloadWithBatching(payload,
                                                 QString("live raw YUYV %1x%2")
                                                 .arg(expectedResolution.width())
                                                 .arg(expectedResolution.height()),
                                                 false);

    if (!ok) {
        // 发送失败即停流，避免持续错误刷屏和驱动压力累积。
        m_liveVideoSending = false;
        m_liveSendStartMs = 0;
        if (m_previewSession) {
            m_previewSession->setRawFrameDeliveryEnabled(false);
        }
        clearLiveVideoBuffers();
        ui->btnSendLiveVideo->setText(QString::fromUtf8("开始实时视频发送(封包+批量)"));
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[ERROR] Live camera streaming stopped due to XDMA write failure."));
        return;
    }
}

// ===== 模块：传输与 XDMA 底层实现 =====
// 说明：
// 1) 该模块只承载“通道管理 + 发送执行 + 自测执行”；
// 2) 上层按钮/回调只调用本模块，不直接触碰驱动细节。

// ----- 子模块：XDMA 通道管理 -----
// 关闭 XDMA 句柄并复位会话状态。
void Widget::closeXdmaHandles()
{
    if (m_liveVideoSending) {
        stopLiveVideoSending(QStringLiteral("XDMA channel close"));
    } else {
        clearLiveVideoBuffers();
    }

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

    if (ui && ui->btnSendLinkTestPacket) {
        ui->btnSendLinkTestPacket->setEnabled(false);
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
    constexpr size_t kPathLength = 1024; // 可以考虑改成260 + 1 以适配 Windows MAX_PATH，但目前驱动似乎不会返回过长路径。

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

    ui->btnSendLinkTestPacket->setEnabled(true);
    ui->plainTextEdit->appendPlainText(QStringLiteral("[OK] XDMA open complete: user + h2c_0 ready."));
    return true;
}

bool Widget::parseUiRegisterValue(const QString &text,
                                  quint32 &outValue,
                                  const QString &fieldName)
{
    // 输入支持：
    // - 16进制：如 0x80、0x0C、0X1；
    // - 10进制：如 128。
    // toULongLong(base=0) 会按前缀自动识别进制。
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        ui->plainTextEdit->appendPlainText(
                    QString("[AXIL][ERROR] %1 is empty.").arg(fieldName));
        return false;
    }

    bool ok = false;
    const qulonglong raw = trimmed.toULongLong(&ok, 0);
    if (!ok || raw > 0xFFFFFFFFULL) {
        ui->plainTextEdit->appendPlainText(
                    QString("[AXIL][ERROR] Invalid %1: %2")
                    .arg(fieldName)
                    .arg(trimmed));
        return false;
    }

    outValue = static_cast<quint32>(raw);
    return true;
}

bool Widget::readUserRegister(quint32 address, quint32 &value)
{
    // AXI lite 32bit 寄存器访问要求 4 字节对齐。
    if ((address & 0x3u) != 0) {
        ui->plainTextEdit->appendPlainText(
                    QString("[AXIL][ERROR] Register address must be 4-byte aligned: %1")
                    .arg(toHex32(address)));
        return false;
    }

    HANDLE user = reinterpret_cast<HANDLE>(m_xdmaUserHandle);
    if (!isValidHandle(user)) {
        // 用户未手动点“打开XDMA通道并自检”时，提供自动兜底。
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[AXIL] user channel is not open, trying auto-open..."));
        if (!openXdmaAndSelfCheck()) {
            ui->plainTextEdit->appendPlainText(
                        QStringLiteral("[AXIL][ERROR] XDMA auto-open failed for register read."));
            return false;
        }
        user = reinterpret_cast<HANDLE>(m_xdmaUserHandle);
    }

    if (!isValidHandle(user)) {
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[AXIL][ERROR] user channel handle is invalid."));
        return false;
    }

    unsigned int raw = 0;
    // user 通道按地址偏移读 4 字节寄存器值。
    const int ret = read_device(user,
                                static_cast<long>(address),
                                4,
                                reinterpret_cast<BYTE *>(&raw));
    if (ret != 4) {
        ui->plainTextEdit->appendPlainText(
                    QString("[AXIL][ERROR] read_device failed at %1, ret=%2")
                    .arg(toHex32(address))
                    .arg(ret));
        return false;
    }

    value = static_cast<quint32>(raw);
    return true;
}

bool Widget::writeUserRegister(quint32 address, quint32 value)
{
    // AXI lite 32bit 寄存器访问要求 4 字节对齐。
    if ((address & 0x3u) != 0) {
        ui->plainTextEdit->appendPlainText(
                    QString("[AXIL][ERROR] Register address must be 4-byte aligned: %1")
                    .arg(toHex32(address)));
        return false;
    }

    HANDLE user = reinterpret_cast<HANDLE>(m_xdmaUserHandle);
    if (!isValidHandle(user)) {
        // 用户未手动点“打开XDMA通道并自检”时，提供自动兜底。
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[AXIL] user channel is not open, trying auto-open..."));
        if (!openXdmaAndSelfCheck()) {
            ui->plainTextEdit->appendPlainText(
                        QStringLiteral("[AXIL][ERROR] XDMA auto-open failed for register write."));
            return false;
        }
        user = reinterpret_cast<HANDLE>(m_xdmaUserHandle);
    }

    if (!isValidHandle(user)) {
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[AXIL][ERROR] user channel handle is invalid."));
        return false;
    }

    unsigned int raw = static_cast<unsigned int>(value);
    // user 通道按地址偏移写 4 字节寄存器值。
    const int ret = write_device(user,
                                 static_cast<long>(address),
                                 4,
                                 reinterpret_cast<BYTE *>(&raw));
    if (ret != 4) {
        ui->plainTextEdit->appendPlainText(
                    QString("[AXIL][ERROR] write_device failed at %1, ret=%2")
                    .arg(toHex32(address))
                    .arg(ret));
        return false;
    }

    return true;
}

// ----- 子模块：发送执行（底层/业务桥接） -----
// 通用 XDMA 发送函数。
// 输入 payload 为“原始字节流”，函数内部负责：
// - 自动打开 h2c_0（必要时）；
// - 对齐缓冲区申请与拷贝；
// - 默认分块 write_device 循环发送，forceSingleWrite 时单次发送；
// - 失败回滚与日志。
bool Widget::sendXdmaPayload(const QByteArray &payload,
                             const QString &label,
                             bool verbose,
                             bool forceSingleWrite)
{
    // XDMA 通用发送路径，供以下场景复用：
    // - 手动发送最近采集帧；
    // - 发送测试包；
    // - 实时视频流发送。
    // 流程：
    // 1) 如未打开通道则自动打开 XDMA；
    // 2) 将 payload 拷贝到对齐缓冲区（allocate_buffer）；
    // 3) 在 h2c_0 上写入（forceSingleWrite=true 时严格单次写入）。
    if (payload.isEmpty()) {
        ui->plainTextEdit->appendPlainText(QString("[ERROR] %1 is empty, skip XDMA send.").arg(label));
        return false;
    }

    HANDLE h2c = reinterpret_cast<HANDLE>(m_xdmaH2c0Handle);
    bool autoOpenedThisSend = false;
    if (!isValidHandle(h2c)) {
        ui->plainTextEdit->appendPlainText(QStringLiteral("[XDMA] h2c_0 is not open, trying auto-open..."));
        if (!openXdmaAndSelfCheck()) {
            ui->plainTextEdit->appendPlainText(QStringLiteral("[ERROR] XDMA auto-open failed."));
            return false;
        }
        h2c = reinterpret_cast<HANDLE>(m_xdmaH2c0Handle);
        autoOpenedThisSend = true;
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

    const auto writeSingle = [&](HANDLE handle) -> int {
        return write_device(handle,
                            0x00000000,
                            static_cast<DWORD>(totalBytes),
                            txBuffer);
    };

    int sent = 0;
    if (forceSingleWrite) {
        // 视频批次路径要求“一批一写”：
        // 若驱动返回值不是 totalBytes，视为失败并立即上报，避免
        // 上层误以为该批已经完整送达 FPGA。
        int written = writeSingle(h2c);
        if (written != totalBytes && autoOpenedThisSend) {
            ui->plainTextEdit->appendPlainText(
                        QString("[WARN] First write after auto-open failed for %1: ret=%2, retrying once...")
                        .arg(label)
                        .arg(written));
            if (openXdmaAndSelfCheck()) {
                h2c = reinterpret_cast<HANDLE>(m_xdmaH2c0Handle);
                if (isValidHandle(h2c)) {
                    written = writeSingle(h2c);
                }
            }
        }
        if (written != totalBytes) {
            free_buffer(txBuffer);
            ui->plainTextEdit->appendPlainText(
                        QString("[ERROR] XDMA single-write failed for %1: sent=%2/%3, ret=%4")
                        .arg(label)
                        .arg(qMax(0, written))
                        .arg(totalBytes)
                        .arg(written));
            return false;
        }
        sent = written;
    } else {
        // 历史兼容路径：
        // 分块发送可提升大包写入稳定性，也更便于定位失败位置。
        // 注意：视频主链路不会走这个分支。
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

// 视频数据发送链路：
// 原始视频字节流 -> 1024B 封包 -> 可配置批次聚合 -> XDMA 发送。
bool Widget::sendVideoPayloadWithBatching(const QByteArray &videoPayload,
                                          const QString &label,
                                          bool verbose,
                                          bool allowSendNow)
{
    if (videoPayload.isEmpty()) {
        ui->plainTextEdit->appendPlainText(QString("[ERROR] %1 is empty, skip video packetization.").arg(label));
        return false;
    }

    QVector<QByteArray> readyBatches;
    const VideoPacketBatcher::EnqueueResult result =
            m_videoPacketBatcher.enqueueVideoPayload(videoPayload, readyBatches);
    for (const QByteArray &batch : readyBatches) {
        m_liveReadyBatches.append(batch);
    }

    // [PKG] 日志用于观察三层关系：
    // - raw：输入原始视频字节数；
    // - packets：本次封包后的 1024B 包数量；
    // - emitted/cached：本次产出的完整批次、累计待发批次与缓存字节。
    if (verbose || !readyBatches.isEmpty()) {
        const int batchKB = m_videoPacketBatcher.batchBytes() / 1024;
        int queuedBatchBytes = 0;
        for (const QByteArray &batch : m_liveReadyBatches) {
            queuedBatchBytes += batch.size();
        }
        ui->plainTextEdit->appendPlainText(
                    QString("[PKG] %1 raw=%2B -> packets=%3 (%4B), produced=%5 x %6KB, queued=%7 x %6KB (%8B), cached=%9B, payload-tail=%10B")
                    .arg(label)
                    .arg(result.inputBytes)
                    .arg(result.packetCount)
                    .arg(result.packetBytes)
                    .arg(readyBatches.size())
                    .arg(batchKB)
                    .arg(m_liveReadyBatches.size())
                    .arg(queuedBatchBytes)
                    .arg(result.cachedBytes)
                    .arg(result.pendingPayloadBytes));
    }

    if (!allowSendNow || m_liveReadyBatches.isEmpty()) {
        return true;
    }

    const int totalBatches = m_liveReadyBatches.size();
    for (int i = 0; i < totalBatches; ++i) {
        // 仅在节流时间窗口到达时发送，并保持“一批次一次写”。
        const QByteArray batch = m_liveReadyBatches.first();
        const int batchKB = batch.size() / 1024;
        const bool ok = sendXdmaPayload(batch,
                                        QString("%1 [%2KB batch %3/%4]")
                                        .arg(label)
                                        .arg(batchKB)
                                        .arg(i + 1)
                                        .arg(totalBatches),
                                        false,
                                        true);
        if (!ok) {
            ui->plainTextEdit->appendPlainText(
                        QString("[ERROR] Failed to send batch for %1 at index=%2")
                        .arg(label)
                        .arg(i + 1));
            return false;
        }
        m_liveReadyBatches.remove(0);
        ++m_liveSentBatches;
        if ((m_liveSentBatches % 30) == 0) {
            ui->plainTextEdit->appendPlainText(
                        QString("[XDMA] Live stream progress: sent batches=%1")
                        .arg(m_liveSentBatches));
        }
    }

    m_lastLiveSendMs = QDateTime::currentMSecsSinceEpoch();
    return true;
}

// ----- 子模块：测试执行 -----
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

void Widget::runPacketModuleSelfTest()
{
    // 该函数只做纯软件协议自测，不访问 XDMA 设备：
    // 因此即便未打开硬件，也可以单独验证封包与聚合规则。
    QString report;
    const bool ok = VideoPacketBatcher::runSelfTest(&report);
    ui->plainTextEdit->appendPlainText(
                ok ? QString("[SELFTEST] %1").arg(report)
                   : QString("[SELFTEST][ERROR] %1").arg(report));
}
