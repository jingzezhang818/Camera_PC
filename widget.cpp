#include "widget.h"
#include "ui_widget.h"
#include "directshowpreviewsession.h"

#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QRegularExpression>
#include <QStringList>
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
#include <QVector>
#include <QSet>
#include <vector>
#include <cstring>

// XDMA 杈呭姪 API锛岀敱鍘傚晢 DLL 瀵煎嚭銆?
#include "xdmaDLL_public.h"

namespace {

constexpr int kYuyvBytesPerPixel = 2;

bool isExactPositiveFrameRate(double minFps, double maxFps);

bool isValidYuyvMode(const CameraModeInfo &mode)
{
    const QSize resolution = mode.settings.resolution();
    return mode.settings.pixelFormat() == QVideoFrame::Format_YUYV &&
            resolution.width() > 0 &&
            resolution.height() > 0;
}

int firstYuyvModeIndex(const QList<CameraModeInfo> &modes)
{
    for (int i = 0; i < modes.size(); ++i) {
        if (isValidYuyvMode(modes[i])) {
            return i;
        }
    }
    return -1;
}

QVideoFrame::PixelFormat pixelFormatFromModeTag(const QString &tag)
{
    const QString upper = tag.trimmed().toUpper();
    if (upper == QLatin1String("YUY2") || upper == QLatin1String("YUYV")) {
        return QVideoFrame::Format_YUYV;
    }
    if (upper == QLatin1String("UYVY")) {
        return QVideoFrame::Format_UYVY;
    }
    if (upper == QLatin1String("MJPG") ||
        upper == QLatin1String("MJPEG") ||
        upper == QLatin1String("JPG") ||
        upper == QLatin1String("JPEG")) {
        return QVideoFrame::Format_Jpeg;
    }
    if (upper == QLatin1String("NV12")) {
        return QVideoFrame::Format_NV12;
    }
    if (upper == QLatin1String("NV21")) {
        return QVideoFrame::Format_NV21;
    }
    if (upper == QLatin1String("YV12")) {
        return QVideoFrame::Format_YV12;
    }
    if (upper == QLatin1String("I420")) {
        return QVideoFrame::Format_YUV420P;
    }
    if (upper == QLatin1String("RGB24")) {
        return QVideoFrame::Format_RGB24;
    }
    if (upper == QLatin1String("RGB32")) {
        return QVideoFrame::Format_RGB32;
    }
    if (upper == QLatin1String("ARGB32")) {
        return QVideoFrame::Format_ARGB32;
    }
    return QVideoFrame::Format_Invalid;
}
bool parseDirectShowModeLine(const QString &line,
                             int &width,
                             int &height,
                             QString &formatTag,
                             double &fps,
                             double &minFps,
                             double &maxFps)
{
    static const QRegularExpression concreteRe(
                QStringLiteral("^\\s*mode\\s+#\\d+:\\s+(\\d+)x(\\d+)\\s+format=([^\\s]+)\\s+fps=(-?\\d+(?:\\.\\d+)?)\\s+range\\[(-?\\d+(?:\\.\\d+)?),(-?\\d+(?:\\.\\d+)?)\\]\\s*$"));
    QRegularExpressionMatch match = concreteRe.match(line);
    if (match.hasMatch()) {
        width = match.captured(1).toInt();
        height = match.captured(2).toInt();
        formatTag = match.captured(3);
        fps = match.captured(4).toDouble();
        minFps = match.captured(5).toDouble();
        maxFps = match.captured(6).toDouble();
        return width > 0 && height > 0;
    }

    static const QRegularExpression rangeRe(
                QStringLiteral("^\\s*mode\\s+#\\d+:\\s+(\\d+)x(\\d+)\\s+format=([^\\s]+)\\s+fps\\[(-?\\d+(?:\\.\\d+)?),(-?\\d+(?:\\.\\d+)?)\\]\\s*$"));
    match = rangeRe.match(line);
    if (!match.hasMatch()) {
        return false;
    }
    width = match.captured(1).toInt();
    height = match.captured(2).toInt();
    formatTag = match.captured(3);
    fps = 0.0;
    minFps = match.captured(4).toDouble();
    maxFps = match.captured(5).toDouble();
    return width > 0 && height > 0;
}

double concreteFpsFromDirectShow(double fps, double minFps, double maxFps)
{
    Q_UNUSED(minFps);
    Q_UNUSED(maxFps);
    return fps;
}

QList<CameraModeInfo> buildModesFromDirectShowLines(const QStringList &lines,
                                                    const QList<QCameraInfo> &cameras)
{
    QList<CameraModeInfo> out;
    QSet<QString> dedup;
    int currentCameraIndex = cameras.isEmpty() ? -1 : 0;
    QString currentCameraDescription;
    QString currentDirectShowDevicePath;
    static const QRegularExpression cameraRe(
                QStringLiteral("^\\s*\\[CAMERA\\]\\s+#(\\d+)\\s+(.*)\\s*$"));
    static const QRegularExpression devicePathRe(
                QStringLiteral("^\\s*devicePath:\\s*(.+)\\s*$"));
    for (const QString &line : lines) {
        const QRegularExpressionMatch cameraMatch = cameraRe.match(line);
        if (cameraMatch.hasMatch()) {
            currentCameraIndex = cameraMatch.captured(1).toInt();
            currentCameraDescription = cameraMatch.captured(2).trimmed();
            currentDirectShowDevicePath.clear();
            continue;
        }
        const QRegularExpressionMatch devicePathMatch = devicePathRe.match(line);
        if (devicePathMatch.hasMatch()) {
            currentDirectShowDevicePath = devicePathMatch.captured(1).trimmed();
            continue;
        }
        int width = 0;
        int height = 0;
        QString formatTag;
        double fps = 0.0;
        double minFps = 0.0;
        double maxFps = 0.0;
        if (!parseDirectShowModeLine(line, width, height, formatTag, fps, minFps, maxFps)) {
            continue;
        }
        if (fps <= 0.0) {
            continue;
        }
        const bool hasQtCameraInfo =
                currentCameraIndex >= 0 && currentCameraIndex < cameras.size();
        const QCameraInfo info = hasQtCameraInfo
                ? cameras[currentCameraIndex]
                : QCameraInfo();
        const QVideoFrame::PixelFormat pixelFormat = pixelFormatFromModeTag(formatTag);
        const double concreteFps = concreteFpsFromDirectShow(fps, minFps, maxFps);
        const QString key = QString("%1|%2x%3|%4|%5")
                .arg(currentCameraIndex)
                .arg(width)
                .arg(height)
                .arg(static_cast<int>(pixelFormat))
                .arg(concreteFps, 0, 'f', 3);
        if (dedup.contains(key)) {
            continue;
        }
        dedup.insert(key);

        QCameraViewfinderSettings settings;
        settings.setResolution(QSize(width, height));
        if (pixelFormat != QVideoFrame::Format_Invalid) {
            settings.setPixelFormat(pixelFormat);
        }
        if (concreteFps > 0.0) {
            settings.setMinimumFrameRate(concreteFps);
            settings.setMaximumFrameRate(concreteFps);
        }

        CameraModeInfo mode;
        mode.cameraIndex = currentCameraIndex;
        mode.cameraInfo = info;
        mode.description = hasQtCameraInfo && !info.description().isEmpty()
                ? info.description()
                : currentCameraDescription;
        mode.deviceName = hasQtCameraInfo && !info.deviceName().isEmpty()
                ? info.deviceName()
                : currentDirectShowDevicePath;
        mode.settings = settings;
        mode.directShowMode = true;
        mode.directShowDevicePath = currentDirectShowDevicePath;
        mode.directShowFormatTag = formatTag;
        mode.directShowFps = concreteFps;
        out.push_back(mode);
    }
    return out;
}
QString modeToComboText(const CameraModeInfo &mode)
{
    const QSize resolution = mode.settings.resolution();
    const QString prefix = QString("%1x%2 | %3 | ")
            .arg(resolution.width())
            .arg(resolution.height())
            .arg(CameraProbe::pixelFormatToString(mode.settings.pixelFormat()));

    if (isExactPositiveFrameRate(mode.settings.minimumFrameRate(),
                                 mode.settings.maximumFrameRate())) {
        return prefix + QString("fps=%1")
                .arg(mode.settings.maximumFrameRate(), 0, 'f', 3);
    }

    if (mode.settings.minimumFrameRate() > 0.0 ||
        mode.settings.maximumFrameRate() > 0.0) {
        return prefix + QString("fps[%1,%2]")
                .arg(mode.settings.minimumFrameRate(), 0, 'f', 3)
                .arg(mode.settings.maximumFrameRate(), 0, 'f', 3);
    }

    return prefix + QStringLiteral("fps=driver");
}

bool isExactPositiveFrameRate(double minFps, double maxFps)
{
    if (minFps <= 0.0 || maxFps <= 0.0) {
        return false;
    }

    const double delta = minFps - maxFps;
    return delta > -0.001 && delta < 0.001;
}

QCameraViewfinderSettings makeDriverRequestSettings(const QCameraViewfinderSettings &source)
{
    QCameraViewfinderSettings request;

    const QSize resolution = source.resolution();
    if (resolution.width() > 0 && resolution.height() > 0) {
        request.setResolution(resolution);
    }

    if (source.pixelFormat() != QVideoFrame::Format_Invalid) {
        request.setPixelFormat(source.pixelFormat());
    }

    if (isExactPositiveFrameRate(source.minimumFrameRate(),
                                 source.maximumFrameRate())) {
        request.setMinimumFrameRate(source.minimumFrameRate());
        request.setMaximumFrameRate(source.maximumFrameRate());
    }

    return request;
}

bool hasDriverRequestSettings(const QCameraViewfinderSettings &settings)
{
    const QSize resolution = settings.resolution();
    return (resolution.width() > 0 && resolution.height() > 0) ||
            settings.pixelFormat() != QVideoFrame::Format_Invalid ||
            settings.minimumFrameRate() > 0.0 ||
            settings.maximumFrameRate() > 0.0;
}

bool isValidHandle(HANDLE handle)
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

// 鍍忕礌鍒嗛噺閽充綅鍒?[0,255]锛岄伩鍏嶉鑹茶绠楁孩鍑恒€?
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

// 缁熶竴杈撳嚭 32bit 鍗佸叚杩涘埗鏂囨湰锛堝ぇ鍐欍€佸浐瀹?8 浣嶏級銆?// 鐢ㄤ簬瀵勫瓨鍣ㄨ鍐欐棩蹇椾笌鈥滆鍥炲€尖€濆睍绀猴紝鍑忓皯鏍煎紡涓嶄竴鑷撮棶棰樸€?
QString toHex32(quint32 value)
{
    return QString("0x%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
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

bool frameToPreviewImage(const CapturedFrame &frame, QImage &outImage)
{
    if (frame.pixelFormat == QVideoFrame::Format_Jpeg) {
        return outImage.loadFromData(frame.payload);
    }

    switch (frame.pixelFormat) {
    case QVideoFrame::Format_YUYV:
        return yuyvToRgbImage(frame, outImage);
    default:
        return false;
    }
}

} // 鍖垮悕鍛藉悕绌洪棿

Widget::Widget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Widget)
    , m_probe(new CameraProbe(this))
{
    // 鍒濆鍖?UI銆佹寜閽粯璁ょ姸鎬佸拰棰勮閾捐矾銆?
    ui->setupUi(this);
    // 杞欢鍗忚鑷祴涓嶄緷璧?XDMA锛屽彲鐩存帴鎵嬪姩瑙﹀彂銆?
    ui->btnSendTestPacket->setEnabled(true);
    // 纭欢閾捐矾娴嬭瘯鍖呬緷璧?XDMA 閫氶亾灏辩华锛屽垵濮嬬鐢ㄣ€?
    ui->btnSendLinkTestPacket->setEnabled(false);
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
    // 鏋愭瀯椤哄簭锛氬厛鍋滄暟鎹摼璺紙XDMA/棰勮锛夛紝鍐嶉噴鏀?UI銆?
    closeXdmaHandles();
    stopPreview();
    delete ui;
}

// 鍒濆鍖栧疄鏃堕瑙堬細鍒涘缓 Viewfinder銆佹彃鍏ュ竷灞€銆佹寕鎺?VideoProbe銆?
void Widget::initializePreview()
{
    m_previewWidget = new QWidget(this);
    m_previewWidget->setObjectName("cameraPreview");
    m_previewWidget->setMinimumHeight(280);
    m_previewWidget->setAttribute(Qt::WA_NativeWindow, true);
    m_previewWidget->setAttribute(Qt::WA_DontCreateNativeAncestors, true);
    m_previewWidget->setAutoFillBackground(true);
    m_previewWidget->setStyleSheet(QStringLiteral(
                                       "#cameraPreview { background: #000000; border: 1px solid #8a8a8a; }"));

    m_directShowPreview = new DirectShowPreviewSession(this);
    connect(m_directShowPreview, &DirectShowPreviewSession::logMessage,
            this, [this](const QString &msg) {
        ui->plainTextEdit->appendPlainText(QStringLiteral("[DSHOW] ") + msg);
    });
    connect(m_directShowPreview, &DirectShowPreviewSession::rawFrameFailed,
            this, [this](const QString &reason) {
        ui->plainTextEdit->appendPlainText(QStringLiteral("[WARN] ") + reason);
    });
    connect(m_directShowPreview, &DirectShowPreviewSession::rawFrameAvailable,
            this, &Widget::onRawFrameAvailable);

    ui->verticalLayout->insertWidget(1, m_previewWidget, 1);
    initializeModeControls();
    initializeTransferControls();
    initializeAxiLiteControls();

    startPreview();
}

// 鍦ㄧ晫闈腑鍔ㄦ€佸垱寤轰紶杈撹皟鍙傚尯锛?// - 鑺傛祦闂撮殧锛氭帶鍒跺疄鏃跺抚鍙戦€佹渶灏忛棿闅旓紱
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
    m_availableModes.clear();
    const QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    QStringList directShowLines;
    QString directShowReason;
    const bool directShowOk =
            CameraProbe::enumerateAllModesViaDirectShow(directShowLines, &directShowReason);
    if (directShowOk) {
        m_availableModes = buildModesFromDirectShowLines(directShowLines, cameras);
    }
    if (!directShowOk && m_availableModes.isEmpty()) {
        m_availableModes = CameraProbe::enumerateAllModes();
    }
    QList<CameraModeInfo> filtered;
    filtered.reserve(m_availableModes.size());
    for (const CameraModeInfo &mode : m_availableModes) {
        const bool hasResolution = mode.settings.resolution().width() > 0
                && mode.settings.resolution().height() > 0;
        const bool hasFormat = mode.settings.pixelFormat() != QVideoFrame::Format_Invalid;
        if (hasResolution || hasFormat) {
            filtered.push_back(mode);
        }
    }
    m_availableModes = filtered;
    for (const CameraModeInfo &mode : m_availableModes) {
        m_modeCombo->addItem(modeToComboText(mode));
    }
    if (m_modeCombo->count() == 0) {
        m_modeCombo->addItem(QStringLiteral("No explicit camera mode available"));
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
    int targetIndex = firstYuyvModeIndex(m_availableModes);
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
    if (targetIndex >= 0 && targetIndex < m_availableModes.size()) {
        m_manualPreviewMode = m_availableModes[targetIndex];
        m_useManualPreviewMode = true;
    }
}
void Widget::applySelectedModeFromCombo()
{
    if (!m_modeCombo || m_availableModes.isEmpty()) {
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[WARN] No selectable camera mode is available."));
        return;
    }
    const int index = m_modeCombo->currentIndex();
    if (index < 0 || index >= m_availableModes.size()) {
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[WARN] Please select a valid camera mode."));
        return;
    }
    if (m_liveVideoSending) {
        stopLiveVideoSending(QStringLiteral("camera mode switch"));
    }
    clearLiveVideoBuffers();
    m_manualPreviewMode = m_availableModes[index];
    m_useManualPreviewMode = true;
    ui->plainTextEdit->appendPlainText(
                QString("[INFO] Applying mode: %1")
                .arg(modeToComboText(m_manualPreviewMode)));
    stopPreview();
    startPreview();
}

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
    m_throttleSpin->setSingleStep(1);
    m_throttleSpin->setValue(static_cast<int>(m_liveStreamThrottleMs));

    QLabel *chunkLabel = new QLabel(
                QString::fromWCharArray(L"\u5199\u5165\u5927\u5C0F(KB):"), panel);
    m_chunkSizeSpin = new QSpinBox(panel);
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
        // 杩愯鏃舵洿鏂拌妭娴佸弬鏁帮紝鏃犻渶閲嶅惎棰勮鎴栭噸寤虹浉鏈恒€?
        m_liveStreamThrottleMs = qMax<qint64>(0, value);
        if (ui && ui->plainTextEdit) {
            ui->plainTextEdit->appendPlainText(
                        QString("[CFG] Live throttle interval set to %1 ms")
                        .arg(m_liveStreamThrottleMs));
        }
    });

    connect(m_chunkSizeSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int value) {
        // 杩愯鏃舵洿鏂板啓鍏ユ壒娆″弬鏁帮紝鍚庣画鍙戦€佺珛鍗崇敓鏁堛€?
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

    ui->verticalLayout->insertWidget(2, panel);

    // 璁╁皝鍖呰仛鍚堟ā鍧椾笌 UI 鍒濆鍊间繚鎸佷竴鑷淬€?
    m_videoPacketBatcher.setBatchBytes(m_xdmaChunkBytes);
}

// 鍦ㄧ晫闈腑鍔ㄦ€佸垱寤?AXI lite 瀵勫瓨鍣ㄨ皟璇曞尯锛?// - 鍦板潃杈撳叆锛氭敮鎸佸崄鍏繘鍒讹紙0x锛夋垨鍗佽繘鍒讹紱
// - 鍐欏€艰緭鍏ワ細鐢ㄤ簬 32bit 鍐欏瘎瀛樺櫒锛?// - 璇诲€兼樉绀猴細灞曠ず鏈€杩戜竴娆¤鍙栫粨鏋滐紱
// - 璇?鍐欐寜閽細閫氳繃 XDMA user 閫氶亾璁块棶 AXI lite銆?
void Widget::initializeAxiLiteControls()
{
    // 璇?panel 鏀惧湪涓荤晫闈㈠竷灞€涓紝鎻愪緵 AXI lite 璇诲啓鍏ュ彛銆?    // 閲囩敤鈥滆緭鍏ュ湴鍧€ + 璇?鍐欐寜閽?+ 璇诲洖鏄剧ず鈥濈殑鏈€灏忚皟璇曢棴鐜€?
    QWidget *panel = new QWidget(this);
    panel->setObjectName("axiLiteRegPanel");

    QHBoxLayout *row = new QHBoxLayout(panel);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    QLabel *addrLabel = new QLabel(
                QString::fromWCharArray(L"\u5BC4\u5B58\u5668\u5730\u5740:"), panel);
    m_regAddrEdit = new QLineEdit(panel);
    m_regAddrEdit->setPlaceholderText("0x00000000");
    m_regAddrEdit->setText("0x00000000");
    m_regAddrEdit->setMaximumWidth(130);

    QLabel *writeLabel = new QLabel(
                QString::fromWCharArray(L"\u5199\u5165\u503C:"), panel);
    m_regWriteValueEdit = new QLineEdit(panel);
    m_regWriteValueEdit->setPlaceholderText("0x00000000");
    m_regWriteValueEdit->setText("0x00000000");
    m_regWriteValueEdit->setMaximumWidth(130);

    QPushButton *readBtn = new QPushButton(
                QString::fromWCharArray(L"\u8BFB\u5BC4\u5B58\u5668"), panel);
    QPushButton *writeBtn = new QPushButton(
                QString::fromWCharArray(L"\u5199\u5BC4\u5B58\u5668"), panel);

    QLabel *readbackLabel = new QLabel(
                QString::fromWCharArray(L"\u8BFB\u56DE\u503C:"), panel);
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
        // 璇诲瘎瀛樺櫒娴佺▼锛?        // 1) 瑙ｆ瀽鍦板潃锛?        // 2) 璋冪敤 user 閫氶亾 read_device锛?        // 3) 鏇存柊璇诲洖妗嗗苟鍐欐棩蹇椼€?
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
        // 鍐欏瘎瀛樺櫒娴佺▼锛?        // 1) 瑙ｆ瀽鍦板潃涓庡啓鍊硷紱
        // 2) 璋冪敤 user 閫氶亾 write_device锛?        // 3) 鍐欐棩蹇楃‘璁ゆ湰娆″啓鍏ュ弬鏁般€?
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

    ui->verticalLayout->insertWidget(3, panel);
}

// 鍚姩瀹炴椂棰勮銆?// 鑻ュ凡鏈夋椿鍔ㄩ瑙堝垯鐩存帴杩斿洖锛岄伩鍏嶉噸澶嶅垱寤虹浉鏈哄璞°€?
void Widget::startPreview()
{
    if (!m_previewWidget || !m_directShowPreview) {
        return;
    }

    if (m_directShowPreview->isRunning()) {
        return;
    }

    CameraModeInfo selected;
    QString report;
    if (m_useManualPreviewMode) {
        selected = m_manualPreviewMode;
        report = QStringLiteral("Manual preview mode selected from combo box.");
    } else {
        if (!findDefaultLiveYuyvMode(selected, &report)) {
            ui->plainTextEdit->appendPlainText(QStringLiteral("[ERROR] Live raw preview requires a valid YUYV camera mode. ") + report);
            return;
        }
    }
    if (!report.isEmpty()) {
        ui->plainTextEdit->appendPlainText(QStringLiteral("[INFO] ") + report);
    }

    QString startReason;
    if (!m_directShowPreview->start(selected, m_previewWidget, &startReason)) {
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[ERROR] DirectShow preview start failed: ") + startReason);
        return;
    }

    m_directShowPreview->setRawFrameDeliveryEnabled(m_liveVideoSending);
    m_waitingPreviewSingleFrame = false;

    ui->plainTextEdit->appendPlainText(
                QString("[INFO] Camera preview requested: camera=%1, dev=%2, resolution=%3x%4, format=%5, fps=%6")
                .arg(selected.description)
                .arg(selected.deviceName)
                .arg(selected.settings.resolution().width())
                .arg(selected.settings.resolution().height())
                .arg(CameraProbe::pixelFormatToString(selected.settings.pixelFormat()))
                .arg(selected.settings.maximumFrameRate(), 0, 'f', 3));
}

void Widget::stopPreview()
{
    m_waitingPreviewSingleFrame = false;
    if (m_directShowPreview) {
        m_directShowPreview->stop();
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
    if (m_directShowPreview) {
        m_directShowPreview->setRawFrameDeliveryEnabled(false);
    }
    ui->btnSendLiveVideo->setText(QString::fromWCharArray(L"\u5F00\u59CB\u5B9E\u65F6\u89C6\u9891\u53D1\u9001(\u5C01\u5305+\u6279\u91CF)"));

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
    CameraModeInfo mode;
    QString modeReport;
    if (m_useManualPreviewMode) {
        mode = m_manualPreviewMode;
    } else if (!findDefaultLiveYuyvMode(mode, &modeReport)) {
        if (reason) {
            *reason = modeReport;
        }
        return false;
    }

    if (isValidYuyvMode(mode)) {
        return true;
    }

    const QSize resolution = mode.settings.resolution();
    const QVideoFrame::PixelFormat format = mode.settings.pixelFormat();
    if (reason) {
        *reason = QString("Live XDMA streaming supports YUYV raw only, current mode is %1x%2 %3.")
                .arg(resolution.width())
                .arg(resolution.height())
                .arg(CameraProbe::pixelFormatToString(format));
    }
    return false;
}

bool Widget::findDefaultLiveYuyvMode(CameraModeInfo &outMode, QString *report) const
{
    QList<CameraModeInfo> modes = m_availableModes;
    if (modes.isEmpty()) {
        modes = CameraProbe::enumerateYuy2Modes();
    }
    QStringList available;

    for (const CameraModeInfo &mode : modes) {
        const QSize resolution = mode.settings.resolution();
        if (mode.settings.pixelFormat() == QVideoFrame::Format_YUYV) {
            available << QString("%1 dev=%2 %3x%4 fps=[%5,%6]")
                         .arg(mode.description)
                         .arg(mode.deviceName)
                         .arg(resolution.width())
                         .arg(resolution.height())
                         .arg(mode.settings.minimumFrameRate())
                         .arg(mode.settings.maximumFrameRate());
        }

        if (isValidYuyvMode(mode)) {
            outMode = mode;
            if (report) {
                *report = QString("Default YUYV mode selected: %1x%2 fps=[%3,%4].")
                        .arg(resolution.width())
                        .arg(resolution.height())
                        .arg(mode.settings.minimumFrameRate())
                        .arg(mode.settings.maximumFrameRate());
            }
            return true;
        }
    }

    if (report) {
        if (available.isEmpty()) {
            *report = QStringLiteral("No valid YUYV modes were reported by the camera driver.");
        } else {
            *report = QStringLiteral("Available YUYV modes: ") + available.join(QStringLiteral(" | "));
        }
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

// ===== 妯″潡锛歎I 鎿嶄綔鍏ュ彛锛堟寜閽Ы锛変笌閲囬泦鍥炶皟 =====
// 璇存槑锛?// 1) 鍏堟斁鈥滅敤鎴峰彲鐩存帴瑙﹀彂鈥濈殑鎸夐挳鍏ュ彛锛屼究浜庝粠涓婂埌涓嬬悊瑙ｄ笟鍔℃祦绋嬶紱
// 2) 鍐嶆斁閲囬泦/棰勮鍥炶皟锛屽舰鎴愬畬鏁寸殑鈥滃叆鍙?-> 鍥炶皟 -> 搴曞眰鍙戦€佲€濋槄璇昏矾寰勩€?
// ----- 瀛愭ā鍧楋細閲囬泦涓庤棰戝彂閫佹寜閽?-----
// 鍒楀嚭鐩告満涓庢ā寮忎俊鎭€?// 璇ュ嚱鏁扮敤浜庤仈璋冮樁娈靛揩閫熺‘璁わ細璁惧鍙鎬с€侀┍鍔ㄦ槸鍚﹁繑鍥炴ā寮忓垪琛ㄣ€?
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

    ui->plainTextEdit->appendPlainText("=== Mode Summary ===");
    refreshModeCombo();

    QStringList directShowLines;
    QString directShowReason;
    if (CameraProbe::enumerateAllModesViaDirectShow(directShowLines, &directShowReason)) {
        ui->plainTextEdit->appendPlainText(QString("[INFO] %1").arg(directShowReason));
        for (const QString &line : directShowLines) {
            ui->plainTextEdit->appendPlainText(line);
        }
        return;
    }

    if (!directShowReason.isEmpty()) {
        ui->plainTextEdit->appendPlainText(
                    QString("[WARN] DirectShow enumeration unavailable: %1").arg(directShowReason));
    }

    const auto modes = CameraProbe::enumerateAllModes();
    QList<CameraModeInfo> validModes;
    validModes.reserve(modes.size());
    ui->plainTextEdit->appendPlainText(
                QString("enumerated entries (Qt fallback): %1").arg(modes.size()));

    for (const auto &m : modes) {
        const bool hasResolution = m.settings.resolution().width() > 0
                && m.settings.resolution().height() > 0;
        const bool hasPixelFormat = m.settings.pixelFormat() != QVideoFrame::Format_Invalid;
        const bool hasFps = m.settings.minimumFrameRate() > 0.0
                || m.settings.maximumFrameRate() > 0.0;
        if (hasResolution || hasPixelFormat || hasFps) {
            validModes.push_back(m);
        }
    }

    ui->plainTextEdit->appendPlainText(
                QString("valid entries (Qt fallback): %1").arg(validModes.size()));

    if (validModes.isEmpty()) {
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[INFO] No explicit camera mode list was returned by either DirectShow or Qt. Preview/capture will use camera default mode."));
        return;
    }

    ui->plainTextEdit->appendPlainText("all valid fallback modes:");
    for (int i = 0; i < validModes.size(); ++i) {
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
}

// 鍗曞抚鎶撳彇鍏ュ彛锛氫紭鍏堣姹?YUY2 鎸囧畾鍒嗚鲸鐜囷紝澶辫触鍒欏洖閫€榛樿妯″紡銆?
void Widget::on_btnGrabOneFrame_clicked()
{
    if (m_directShowPreview && m_directShowPreview->isRunning()) {
        if (!m_directShowPreview->isRawCaptureActive()) {
            ui->plainTextEdit->appendPlainText(
                        QStringLiteral("[ERROR] Cannot capture one frame: DirectShow raw callback branch is not available."));
            return;
        }
        if (m_waitingPreviewSingleFrame) {
            ui->plainTextEdit->appendPlainText(
                        QStringLiteral("[WARN] Single-frame capture is already waiting for the next preview raw frame."));
            return;
        }

        m_waitingPreviewSingleFrame = true;
        m_restartPreviewAfterCapture = false;
        m_directShowPreview->requestOneRawFrame();
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[INFO] Single-frame capture will use the next raw frame from the active DirectShow preview session."));
        QTimer::singleShot(2000, this, [this]() {
            if (!m_waitingPreviewSingleFrame) {
                return;
            }
            m_waitingPreviewSingleFrame = false;
            ui->plainTextEdit->appendPlainText(
                        QStringLiteral("[ERROR] Timeout waiting for one raw frame from DirectShow preview session."));
        });
        return;
    }

    const QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    if (cameras.isEmpty()) {
        ui->plainTextEdit->appendPlainText(QStringLiteral("[ERROR] No camera found."));
        return;
    }

    CameraModeInfo selected;
    QString reason;
    if (m_useManualPreviewMode) {
        selected = m_manualPreviewMode;
        reason = QStringLiteral("Single-frame capture uses the selected camera mode.");
        ui->plainTextEdit->appendPlainText(QStringLiteral("[INFO] ") + reason);
    } else if (CameraProbe::findPreferredYuy2Mode(640, 480, selected, &reason)) {
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
                QString("[INFO] 鍑嗗閲囬泦: camera=%1, dev=%2, resolution=%3x%4, format=%5, fps=[%6,%7]")
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

// 鍙戦€佲€滈噰涓€甯р€濈紦瀛樺埌鐨勬渶杩戣礋杞姐€?
void Widget::on_btnSendCapturedFrame_clicked()
{
    // 瑙ｈ€﹁矾寰勶細閲囬泦涓庡彂閫佸垎绂汇€?
    // 鏈寜閽缁堜粠鈥滄渶杩戜竴娆′繚瀛樼殑 raw 鏂囦欢鈥濋噸璇绘暟鎹苟鍙戦€侊紝閬垮厤鍙楀唴瀛樻€佸奖鍝嶃€?
    if (m_lastCapturedRawPath.isEmpty()) {
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[ERROR] No captured frame cached. Please capture one frame first."));
        return;
    }

    QFile rawFile(m_lastCapturedRawPath);
    if (!rawFile.open(QIODevice::ReadOnly)) {
        ui->plainTextEdit->appendPlainText(
                    QString("[ERROR] Cannot open cached raw file: %1").arg(m_lastCapturedRawPath));
        return;
    }
    const QByteArray rawPayload = rawFile.readAll();
    rawFile.close();
    if (rawPayload.isEmpty()) {
        ui->plainTextEdit->appendPlainText(
                    QString("[ERROR] Cached raw file is empty: %1").arg(m_lastCapturedRawPath));
        return;
    }

    const QString label = m_lastCapturedFrameLabel.isEmpty()
            ? QStringLiteral("cached camera frame")
            : m_lastCapturedFrameLabel;
    constexpr int kCachedFrameFixedPacketCount = 460;
    int packetCount = 0;
    const QByteArray packetStream = m_videoPacketBatcher.buildPacketStream(rawPayload,
                                                                           &packetCount,
                                                                           kCachedFrameFixedPacketCount);
    if (packetStream.isEmpty()) {
        ui->plainTextEdit->appendPlainText(
                    QString("[ERROR] Packetization failed for cached raw: %1")
                    .arg(m_lastCapturedRawPath));
        return;
    }

    ui->plainTextEdit->appendPlainText(
                QString("[PKG][DIRECT] %1 raw=%2B -> packets=%3 (%4B), fixed-min=%5, source=%6")
                .arg(label)
                .arg(rawPayload.size())
                .arg(packetCount)
                .arg(packetStream.size())
                .arg(kCachedFrameFixedPacketCount)
                .arg(m_lastCapturedRawPath));

    const bool ok = sendXdmaPayload(packetStream,
                                    QString("%1 [direct packet stream]").arg(label),
                                    true,
                                    false);
    if (!ok) {
        ui->plainTextEdit->appendPlainText(
                    QString("[ERROR] Direct cached-frame send failed: %1")
                    .arg(m_lastCapturedRawPath));
    }
}

// 瀹炴椂鍙戦€佸紑鍏虫寜閽€?// 寮€鍚悗鐢?onRawFrameAvailable 鎸夎妭娴侀棿闅旈噰鏍峰苟鍙戦€侊紱鍏抽棴鍚庝粎淇濈暀棰勮涓嶅彂閫併€?
void Widget::on_btnSendLiveVideo_clicked()
{
    // Runtime path: DirectShow raw callback -> YUYV normalize -> sendVideoPayloadWithBatching -> h2c_0.
    // 杩欓噷鍙垏鎹⑩€滄槸鍚﹀彂閫佲€濓紝涓嶆敼鍙樼浉鏈洪噰闆嗗弬鏁般€?
    if (!m_liveVideoSending) {
        QString modeReason;
        if (!selectedModeSupportsLiveStreaming(&modeReason)) {
            ui->plainTextEdit->appendPlainText(QStringLiteral("[ERROR] Cannot start live streaming: ") + modeReason);
            return;
        }
        if (!m_directShowPreview || !m_directShowPreview->isRunning()) {
            startPreview();
        }
        if (!m_directShowPreview || !m_directShowPreview->isRunning()) {
            ui->plainTextEdit->appendPlainText(
                        QStringLiteral("[ERROR] Cannot start live streaming: DirectShow preview is not active."));
            return;
        }
        if (!m_directShowPreview->isRawCaptureActive()) {
            ui->plainTextEdit->appendPlainText(
                        QStringLiteral("[ERROR] Cannot start live streaming: DirectShow raw callback branch is not available."));
            return;
        }

        clearLiveVideoBuffers();
        m_liveVideoSending = true;
        m_directShowPreview->setRawFrameDeliveryEnabled(true);
        m_liveSendStartMs = QDateTime::currentMSecsSinceEpoch();
        m_lastLiveSendMs = 0;
        m_liveSentBatches = 0;
        ui->btnSendLiveVideo->setText(QString::fromWCharArray(L"\u505C\u6B62\u5B9E\u65F6\u89C6\u9891\u53D1\u9001(\u5C01\u5305+\u6279\u91CF)"));
        ui->plainTextEdit->appendPlainText(
                    QStringLiteral("[XDMA] Live camera streaming to h2c_0 started."));
        return;
    }

    stopLiveVideoSending();
}

// ----- 瀛愭ā鍧楋細XDMA 涓庢祴璇曟寜閽?-----
// 鎵嬪姩鎵撳紑 XDMA 骞舵墽琛?ready_state 鑷銆?
void Widget::on_btnOpenXdma_clicked()
{
    // 鎵嬪姩鎵撳紑+鑷鍏ュ彛锛屼究浜庤仈璋冨拰涓婄數楠岃瘉銆?
    // 瀹炴椂鍙戦€佸拰鎵嬪姩鍙戦€佽矾寰勪篃鏀寔鑷姩鎵撳紑鍏滃簳銆?
    openXdmaAndSelfCheck();
}

// 鍙戦€佹祴璇曞寘鎸夐挳妲姐€?
void Widget::on_btnSendTestPacket_clicked()
{
    // 鎵嬪姩瑙﹀彂杞欢渚у皝鍖?鑱氬悎鑷祴锛?    // 1) 楠岃瘉 1024B 鍖呭ご瀛楁銆乴ength 涓庤ˉ闆惰鍒欙紱
    // 2) 楠岃瘉榛樿閰嶇疆涓?1024 鍖呮槸鍚﹀噯纭仛鍚堜负 1MiB銆?
    runPacketModuleSelfTest();
}

void Widget::on_btnClearLog_clicked()
{
    if (ui && ui->plainTextEdit) {
        ui->plainTextEdit->clear();
    }
}

// XDMA 閾捐矾娴嬭瘯鍖呮寜閽Ы銆?
void Widget::on_btnSendLinkTestPacket_clicked()
{
    // 鏃犵浉鏈轰緷璧栫殑鍥哄畾娴嬭瘯鍖呰矾寰勶紝鐢ㄤ簬蹇€熼獙璇?PC->FPGA H2C 閾捐矾銆?
    // 涓庘€滆蒋浠跺崗璁嚜娴嬧€濆垎寮€锛岄伩鍏嶆祴璇曡亴璐ｆ贩鐢ㄣ€?
    sendXdmaTestPacket();
}

// ----- 瀛愭ā鍧楋細閲囬泦涓庨瑙堝洖璋?-----
// 閫忎紶 CameraProbe 鏃ュ織鍒扮晫闈€?
void Widget::onProbeLog(const QString &msg)
{
    ui->plainTextEdit->appendPlainText("[LOG] " + msg);
}

// 鍗曞抚鎶撳彇鎴愬姛鍥炶皟锛?// 1) 淇濆瓨 raw锛?// 2) 缂撳瓨 payload 渚涙墜鍔?XDMA 鍙戦€侊紱
// 3) 鑻ユ牸寮忔槸 YUYV锛屽垯棰濆瀵煎嚭 PNG 棰勮銆?
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

    m_lastCapturedRawPath = QFileInfo(file).absoluteFilePath();
    m_lastCapturedFrameLabel = QString("captured frame %1x%2 %3")
            .arg(frame.resolution.width())
            .arg(frame.resolution.height())
            .arg(CameraProbe::pixelFormatToString(frame.pixelFormat));
    ui->btnSendCapturedFrame->setEnabled(true);
    ui->plainTextEdit->appendPlainText(
                QString("[INFO] Frame cached for manual XDMA send: %1 (%2 bytes)")
                .arg(m_lastCapturedRawPath)
                .arg(frame.payload.size()));

    QImage image;
    if (frameToPreviewImage(frame, image)) {
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
                    QString("[INFO] PNG preview is not available for current format=%1")
                    .arg(CameraProbe::pixelFormatToString(frame.pixelFormat)));
    }

    if (m_restartPreviewAfterCapture) {
        // 鎶撳抚缁撴潫鍚庡欢杩熸仮澶嶉瑙堬紝闄嶄綆閲嶅叆椋庨櫓銆?
        m_restartPreviewAfterCapture = false;
        QTimer::singleShot(120, this, [this]() {
            startPreview();
        });
    }
}

// 鍗曞抚鎶撳彇澶辫触鍥炶皟锛氳褰曞師鍥犲苟鎸夐渶鎭㈠棰勮銆?
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

// Raw 帧回调：
// 1) DirectShow renderer 负责预览显示，本函数不做 RGB 转换或 UI 缩放；
// 2) 单帧采集请求复用当前预览 session 的下一帧 raw 数据；
// 3) 实时发送开启后，继续走既有 YUYV normalize + XDMA 封包发送路径。
void Widget::onRawFrameAvailable(const CapturedFrame &frame)
{
    if (m_waitingPreviewSingleFrame) {
        m_waitingPreviewSingleFrame = false;
        onProbeSuccess(frame);
        if (!m_liveVideoSending) {
            return;
        }
    }

    if (!m_liveVideoSending) {
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_liveStreamThrottleMs > 0
            && m_lastLiveSendMs > 0
            && (nowMs - m_lastLiveSendMs) < m_liveStreamThrottleMs) {
        return;
    }

    QByteArray payload;
    QString reason;
    CameraModeInfo liveMode;
    QString modeReason;
    if (m_useManualPreviewMode) {
        liveMode = m_manualPreviewMode;
    } else if (!findDefaultLiveYuyvMode(liveMode, &modeReason)) {
        ui->plainTextEdit->appendPlainText(QStringLiteral("[WARN] Drop raw live frame: ") + modeReason);
        return;
    }

    const QSize expectedResolution = liveMode.settings.resolution();
    if (!normalizeLiveYuyvFrame(frame, expectedResolution, payload, &reason)) {
        ui->plainTextEdit->appendPlainText(QStringLiteral("[WARN] Drop raw live frame: ") + reason);
        return;
    }

    m_lastLiveSendMs = nowMs;
    const bool ok = sendVideoPayloadWithBatching(payload,
                                                 QString("live raw YUYV %1x%2")
                                                 .arg(expectedResolution.width())
                                                 .arg(expectedResolution.height()),
                                                 false,
                                                 true);

    if (!ok) {
        const qint64 elapsedMs = m_liveSendStartMs > 0
                ? qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - m_liveSendStartMs)
                : 0;
        m_liveVideoSending = false;
        m_liveSendStartMs = 0;
        if (m_directShowPreview) {
            m_directShowPreview->setRawFrameDeliveryEnabled(false);
        }
        clearLiveVideoBuffers();
        ui->btnSendLiveVideo->setText(QString::fromWCharArray(L"\u5F00\u59CB\u5B9E\u65F6\u89C6\u9891\u53D1\u9001(\u5C01\u5305+\u6279\u91CF)"));
        ui->plainTextEdit->appendPlainText(
                    QString("[ERROR] Live raw YUYV streaming stopped due to XDMA write failure. duration=%1 ms")
                    .arg(elapsedMs));
        return;
    }
}
// ===== 妯″潡锛氫紶杈撲笌 XDMA 搴曞眰瀹炵幇 =====
// 璇存槑锛?// 1) 璇ユā鍧楀彧鎵胯浇鈥滈€氶亾绠＄悊 + 鍙戦€佹墽琛?+ 鑷祴鎵ц鈥濓紱
// 2) 涓婂眰鎸夐挳/鍥炶皟鍙皟鐢ㄦ湰妯″潡锛屼笉鐩存帴瑙︾椹卞姩缁嗚妭銆?
// ----- 瀛愭ā鍧楋細XDMA 閫氶亾绠＄悊 -----
// 鍏抽棴 XDMA 鍙ユ焺骞跺浣嶄細璇濈姸鎬併€?
void Widget::closeXdmaHandles()
{
    // 閲嶈繛鎴栭€€鍑烘椂閮借鍏抽棴涓ょ被鍙ユ焺銆?    // 灏佽搴撳唴閮ㄩ€氬父鐢?CreateFile 鎵撳紑閫氶亾锛?    // 鍥犳姣忎釜鎴愬姛鎵撳紑鐨勯€氶亾閮藉繀椤诲搴?CloseHandle銆?
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

// 鎵撳紑 XDMA 骞跺仛鍩虹鑷锛?// 1) 鏋氫妇璁惧锛?// 2) 鎵撳紑 user 閫氶亾锛?// 3) 鎵撳紑 h2c_0 閫氶亾锛?// 4) 璇诲彇 ready_state銆?
bool Widget::openXdmaAndSelfCheck()
{
    // 閲嶅紑绛栫暐锛氬厛娓呯悊鏃у彞鏌勶紝鍐嶅仛涓€娆″畬鏁存墦寮€娴佺▼銆?
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

    // 1) 鏋氫妇 GUID_DEVINTERFACE_XDMA 瀵瑰簲鐨勫熀纭€璁惧璺緞銆?
    // 灏佽鍑芥暟浼氭妸姣忎釜璁惧璺緞鍐欏叆浼犲叆鐨勫瓧绗︾紦鍐插尯銆?
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

    // 2) 鍏堟墦寮€ user锛堟帶鍒?BAR锛夐€氶亾銆?
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

    // 3) 鎵撳紑 h2c_0 娴佸彂閫侀€氶亾锛岀敤浜庝富鏈哄埌 FPGA 鐨勬暟鎹紶杈撱€?
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

    // 4) 璋冪敤 ready_state 鍋氳嚜妫€锛堝叿浣撹涔夌敱鍘傚晢 API 瀹氫箟锛夈€?
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
    // 杈撳叆鏀寔锛?    // - 16杩涘埗锛氬 0x80銆?x0C銆?X1锛?    // - 10杩涘埗锛氬 128銆?    // toULongLong(base=0) 浼氭寜鍓嶇紑鑷姩璇嗗埆杩涘埗銆?
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
    // AXI lite 32bit 瀵勫瓨鍣ㄨ闂姹?4 瀛楄妭瀵归綈銆?
    if ((address & 0x3u) != 0) {
        ui->plainTextEdit->appendPlainText(
                    QString("[AXIL][ERROR] Register address must be 4-byte aligned: %1")
                    .arg(toHex32(address)));
        return false;
    }

    HANDLE user = reinterpret_cast<HANDLE>(m_xdmaUserHandle);
    if (!isValidHandle(user)) {
        // 鐢ㄦ埛鏈墜鍔ㄧ偣鈥滄墦寮€XDMA閫氶亾骞惰嚜妫€鈥濇椂锛屾彁渚涜嚜鍔ㄥ厹搴曘€?
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
    // user 閫氶亾鎸夊湴鍧€鍋忕Щ璇?4 瀛楄妭瀵勫瓨鍣ㄥ€笺€?
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
    // AXI lite 32bit 瀵勫瓨鍣ㄨ闂姹?4 瀛楄妭瀵归綈銆?
    if ((address & 0x3u) != 0) {
        ui->plainTextEdit->appendPlainText(
                    QString("[AXIL][ERROR] Register address must be 4-byte aligned: %1")
                    .arg(toHex32(address)));
        return false;
    }

    HANDLE user = reinterpret_cast<HANDLE>(m_xdmaUserHandle);
    if (!isValidHandle(user)) {
        // 鐢ㄦ埛鏈墜鍔ㄧ偣鈥滄墦寮€XDMA閫氶亾骞惰嚜妫€鈥濇椂锛屾彁渚涜嚜鍔ㄥ厹搴曘€?
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
    // user 閫氶亾鎸夊湴鍧€鍋忕Щ鍐?4 瀛楄妭瀵勫瓨鍣ㄥ€笺€?
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

// ----- 瀛愭ā鍧楋細鍙戦€佹墽琛岋紙搴曞眰/涓氬姟妗ユ帴锛?-----
// 閫氱敤 XDMA 鍙戦€佸嚱鏁般€?// 杈撳叆 payload 涓衡€滃師濮嬪瓧鑺傛祦鈥濓紝鍑芥暟鍐呴儴璐熻矗锛?// - 鑷姩鎵撳紑 h2c_0锛堝繀瑕佹椂锛夛紱
// - 瀵归綈缂撳啿鍖虹敵璇蜂笌鎷疯礉锛?// - 榛樿鍒嗗潡 write_device 寰幆鍙戦€侊紝forceSingleWrite 鏃跺崟娆″彂閫侊紱
// - 澶辫触鍥炴粴涓庢棩蹇椼€?
bool Widget::sendXdmaPayload(const QByteArray &payload,
                             const QString &label,
                             bool verbose,
                             bool forceSingleWrite)
{
    // XDMA 閫氱敤鍙戦€佽矾寰勶紝渚涗互涓嬪満鏅鐢細
    // - 鎵嬪姩鍙戦€佹渶杩戦噰闆嗗抚锛?    // - 鍙戦€佹祴璇曞寘锛?    // - 瀹炴椂瑙嗛娴佸彂閫併€?    // 娴佺▼锛?    // 1) 濡傛湭鎵撳紑閫氶亾鍒欒嚜鍔ㄦ墦寮€ XDMA锛?    // 2) 灏?payload 鎷疯礉鍒板榻愮紦鍐插尯锛坅llocate_buffer锛夛紱
    // 3) 鍦?h2c_0 涓婂啓鍏ワ紙forceSingleWrite=true 鏃朵弗鏍煎崟娆″啓鍏ワ級銆?
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
    if (forceSingleWrite) {
        // 瑙嗛鎵规璺緞瑕佹眰鈥滀竴鎵逛竴鍐欌€濓細
        // 鑻ラ┍鍔ㄨ繑鍥炲€间笉鏄?totalBytes锛岃涓哄け璐ュ苟绔嬪嵆涓婃姤锛岄伩鍏?        // 涓婂眰璇互涓鸿鎵瑰凡缁忓畬鏁撮€佽揪 FPGA銆?
        const int written = write_device(h2c,
                                         0x00000000,
                                         static_cast<DWORD>(totalBytes),
                                         txBuffer);
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
        // 鍘嗗彶鍏煎璺緞锛?        // 鍒嗗潡鍙戦€佸彲鎻愬崌澶у寘鍐欏叆绋冲畾鎬э紝涔熸洿渚夸簬瀹氫綅澶辫触浣嶇疆銆?        // 娉ㄦ剰锛氳棰戜富閾捐矾涓嶄細璧拌繖涓垎鏀€?
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

// 瑙嗛鏁版嵁鍙戦€侀摼璺細
// 鍘熷瑙嗛瀛楄妭娴?-> 1024B 灏佸寘 -> 鍙厤缃壒娆¤仛鍚?-> XDMA 鍙戦€併€?
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

    // [PKG] 鏃ュ織鐢ㄤ簬瑙傚療涓夊眰鍏崇郴锛?    // - raw锛氳緭鍏ュ師濮嬭棰戝瓧鑺傛暟锛?    // - packets锛氭湰娆″皝鍖呭悗鐨?1024B 鍖呮暟閲忥紱
    // - emitted/cached锛氭湰娆′骇鍑虹殑瀹屾暣鎵规銆佺疮璁″緟鍙戞壒娆′笌缂撳瓨瀛楄妭銆?
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
        // 浠呭湪鑺傛祦鏃堕棿绐楀埌杈炬椂鍙戦€侊紝骞朵繚鎸佲€滀竴鎵规涓€娆″啓鈥濄€?
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

// ----- 瀛愭ā鍧楋細娴嬭瘯鎵ц -----
// 鍙戦€佸浐瀹氭祴璇曞寘锛岀敤浜庨獙璇?PC->FPGA H2C 閫氶亾杩為€氭€с€?
bool Widget::sendXdmaTestPacket()
{
    // 4KB 鍥哄畾妯″紡娴嬭瘯鍖咃紝鐢ㄤ簬閾捐矾杩為€氭€ч獙璇併€?    // 澶撮儴 [0..3]="XDMA"锛?[4..7]=搴忓彿銆?
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
    // 璇ュ嚱鏁板彧鍋氱函杞欢鍗忚鑷祴锛屼笉璁块棶 XDMA 璁惧锛?    // 鍥犳鍗充究鏈墦寮€纭欢锛屼篃鍙互鍗曠嫭楠岃瘉灏佸寘涓庤仛鍚堣鍒欍€?
    QString report;
    const bool ok = VideoPacketBatcher::runSelfTest(&report);
    ui->plainTextEdit->appendPlainText(
                ok ? QString("[SELFTEST] %1").arg(report)
                   : QString("[SELFTEST][ERROR] %1").arg(report));
}
