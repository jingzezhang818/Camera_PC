#ifndef CAMERAPROBE_H
#define CAMERAPROBE_H

// CameraProbe 模块职责：
// - 枚举摄像头及可用模式；
// - 按指定模式启动一次性单帧抓取；
// - 将首帧封装为 CapturedFrame 结构并回调给上层。

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

// 单个相机模式描述：包括相机身份信息与对应 viewfinder 参数。
struct CameraModeInfo
{
    int cameraIndex = -1;
    QCameraInfo cameraInfo;
    QString description;
    QString deviceName;
    QCameraViewfinderSettings settings;
};

// 单帧抓取结果：
// - 元信息：设备名、分辨率、像素格式、时间戳；
// - 布局信息：plane 数、每行字节数、每 plane 映射大小；
// - 负载：原始帧字节数据。
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

// 用于承接 QCamera 输出帧的 Surface：
// - 在 one-shot 模式下只抓第一帧；
// - 抓到后立即回调并关闭等待状态。
class FrameGrabSurface : public QAbstractVideoSurface
{
    Q_OBJECT
public:
    explicit FrameGrabSurface(QObject *parent = nullptr);

    // 设置当前抓帧任务关联的相机元信息（描述、设备名）。
    void setExpectedMeta(const QString &desc, const QString &devName);

    // 打开一次性抓帧开关：下一帧有效帧到达即抓取。
    void armOneShot();

    // QAbstractVideoSurface 覆写：声明支持的像素格式并接收帧回调。
    QList<QVideoFrame::PixelFormat> supportedPixelFormats(
            QAbstractVideoBuffer::HandleType type = QAbstractVideoBuffer::NoHandle) const override;
    bool present(const QVideoFrame &frame) override;

signals:
    void logMessage(const QString &msg);
    void firstFrameCaptured(const CapturedFrame &frame);
    void frameCaptureFailed(const QString &reason);

private:
    // 标记当前是否在等待首帧。
    bool m_waitingFirstFrame = false;

    // 由上层传入，用于把帧与具体相机设备绑定。
    QString m_cameraDescription;
    QString m_deviceName;
};

// 外部使用的抓帧控制器：管理 QCamera 与 FrameGrabSurface 生命周期。
class CameraProbe : public QObject
{
    Q_OBJECT
public:
    explicit CameraProbe(QObject *parent = nullptr);
    ~CameraProbe() override;

    // ===== 静态模式查询与选择 =====
    // 像素格式枚举转可读字符串，便于日志展示。
    static QString pixelFormatToString(QVideoFrame::PixelFormat fmt);

    static QList<CameraModeInfo> enumerateAllModes();

    // 仅筛选 YUY2/YUYV 模式，供上层优先抓取。
    static QList<CameraModeInfo> enumerateYuy2Modes();

    // 按优先级寻找模式：
    // 1) 精确匹配宽高 + YUY2；
    // 2) 回退 640x480 YUY2；
    // 3) 回退第一条 YUY2。
    static bool findPreferredYuy2Mode(int width,
                                      int height,
                                      CameraModeInfo &outMode,
                                      QString *reason = nullptr);

    // ===== 抓拍会话生命周期 =====
    // 启动一次单帧抓取。
    bool startSingleFrameCapture(const CameraModeInfo &mode);

    // 停止抓取并释放内部对象。
    void stopCapture();

signals:
    void logMessage(const QString &msg);
    void captureSucceeded(const CapturedFrame &frame);
    void captureFailed(const QString &reason);

private slots:
    // ===== 运行时回调处理 =====
    // 相机错误回调。
    void onCameraError(QCamera::Error error);

    // Surface 内部事件回调。
    void onSurfaceLog(const QString &msg);
    void onSurfaceFrameCaptured(const CapturedFrame &frame);
    void onSurfaceFrameFailed(const QString &reason);

    // 首帧超时回调：防止无限等待。
    void onCaptureTimeout();

private:
    // 抓帧期间的相机对象。
    QCamera *m_camera = nullptr;

    // 承接相机输出帧并做 one-shot 采样的 surface。
    FrameGrabSurface *m_surface = nullptr;

    // 首帧超时计时器。
    QTimer *m_frameTimeout = nullptr;

    // 首帧等待时长（毫秒）。
    int m_frameTimeoutMs = 3000;
};

#endif // CAMERAPROBE_H
