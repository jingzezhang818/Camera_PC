#ifndef WIDGET_H
#define WIDGET_H

// 主窗口类：
// - 负责相机预览与单帧抓取；
// - 负责 XDMA 通道打开、自检与发送；
// - 负责实时预览帧到 XDMA 的流式发送；
// - 负责 UI 交互与日志输出。

#include <QWidget>
#include <QCamera>
#include <QCameraViewfinder>
#include <QVideoProbe>
#include <QByteArray>
#include "cameraprobe.h"

class QSpinBox;

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
    // UI 按钮槽：列出模式、抓帧、发送缓存帧、实时流开关、打开 XDMA、自检/测试包。
    void on_btnListModes_clicked();
    void on_btnGrabOneFrame_clicked();
    void on_btnSendCapturedFrame_clicked();
    void on_btnSendLiveVideo_clicked();
    void on_btnOpenXdma_clicked();
    void on_btnSendTestPacket_clicked();

    // CameraProbe 回调槽：日志、成功、失败。
    void onProbeLog(const QString &msg);
    void onProbeSuccess(const CapturedFrame &frame);
    void onProbeFailed(const QString &reason);

    // 预览相机和预览探针回调。
    void onPreviewCameraError(QCamera::Error error);
    void onPreviewFrameProbed(const QVideoFrame &frame);

private:
    // 初始化实时预览（QCameraViewfinder + QVideoProbe）。
    void initializePreview();

    // 初始化“节流间隔/分包大小”调参控件，并绑定运行时参数。
    void initializeTransferControls();

    // 启动/停止实时预览相机。
    void startPreview();
    void stopPreview();

    // XDMA 通道会话生命周期：
    // 1) 枚举设备并打开 user + h2c_0；
    // 2) 调用 ready_state 做轻量自检；
    // 3) 将负载写入 h2c_0；
    // 4) 程序退出或重连时关闭句柄。
    bool openXdmaAndSelfCheck();
    bool sendXdmaPayload(const QByteArray &payload, const QString &label, bool verbose = true);
    bool sendXdmaTestPacket();

    // 关闭 XDMA 句柄，确保资源释放与状态复位。
    void closeXdmaHandles();

    // Qt Designer 生成的 UI 对象。
    Ui::Widget *ui;

    // 单帧抓取探针（与实时预览链路解耦）。
    CameraProbe *m_probe = nullptr;

    // 实时预览链路对象。
    QCamera *m_previewCamera = nullptr;
    QCameraViewfinder *m_viewfinder = nullptr;
    QVideoProbe *m_videoProbe = nullptr;

    // 抓取单帧前会暂停预览，抓取结束后根据该标记恢复。
    bool m_restartPreviewAfterCapture = false;

    // XDMA 运行时会话字段。
    QString m_xdmaDevicePath;
    void *m_xdmaUserHandle = nullptr;
    void *m_xdmaH2c0Handle = nullptr;

    // 实时流发送状态（预览帧 -> h2c_0）。
    bool m_liveVideoSending = false;
    qint64 m_lastLiveSendMs = 0;
    int m_liveSentFrames = 0;

    // 发送调参：节流间隔和分包大小都支持界面实时调整。
    // m_liveStreamThrottleMs：控制最小发送间隔（毫秒）。
    // m_xdmaChunkBytes：控制每次 write_device 的最大分块字节数。
    qint64 m_liveStreamThrottleMs = 40;
    int m_xdmaChunkBytes = 1024 * 1024;

    // 对应的界面调参控件指针。
    QSpinBox *m_throttleSpin = nullptr;
    QSpinBox *m_chunkSizeSpin = nullptr;

    // 最近一次采集帧缓存，用于手动一键发送。
    QByteArray m_lastCapturedFramePayload;
    QString m_lastCapturedFrameLabel;
};

#endif // 头文件保护宏 WIDGET_H
