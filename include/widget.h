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
#include "video_packet_batcher.h"

class QSpinBox;
class QLineEdit;

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
    // ===== 按钮槽：采集与视频发送 =====
    void on_btnListModes_clicked();
    void on_btnGrabOneFrame_clicked();
    void on_btnSendCapturedFrame_clicked();
    void on_btnSendLiveVideo_clicked();

    // ===== 按钮槽：XDMA 与测试 =====
    void on_btnOpenXdma_clicked();
    void on_btnSendLinkTestPacket_clicked();
    void on_btnSendTestPacket_clicked();

    // ===== 回调槽：CameraProbe =====
    void onProbeLog(const QString &msg);
    void onProbeSuccess(const CapturedFrame &frame);
    void onProbeFailed(const QString &reason);

    // ===== 回调槽：预览链路 =====
    void onPreviewCameraError(QCamera::Error error);
    void onPreviewFrameProbed(const QVideoFrame &frame);

private:
    // ===== 模块：生命周期与预览初始化 =====
    // 初始化实时预览（QCameraViewfinder + QVideoProbe）。
    void initializePreview();

    // 初始化“节流间隔/写入大小”调参控件，并绑定运行时参数。
    void initializeTransferControls();
    // 初始化 AXI lite 寄存器读写控件（地址/写值输入、读值显示、读写按钮）。
    // 该区域是“寄存器调试入口”，与视频发送链路解耦。
    void initializeAxiLiteControls();

    // 启动/停止实时预览相机。
    void startPreview();
    void stopPreview();

    // ===== 模块：视频业务发送（封包 + 聚合） =====
    // 视频发送专用入口：
    // 原始视频流 -> 1024B 协议封包 -> 可配置批次聚合 -> sendXdmaPayload(single write)。
    bool sendVideoPayloadWithBatching(const QByteArray &videoPayload,
                                      const QString &label,
                                      bool verbose = true);

    // 软件自测入口（纯内存，不依赖 XDMA 设备），改为“手动触发”。
    void runPacketModuleSelfTest();

    // ===== 模块：XDMA 底层通道与发送 =====
    // 关闭 XDMA 句柄，确保资源释放与状态复位。
    void closeXdmaHandles();
    // XDMA 通道会话生命周期：
    // 1) 枚举设备并打开 user + h2c_0；
    // 2) 调用 ready_state 做轻量自检；
    // 3) 将负载写入 h2c_0；
    // 4) 程序退出或重连时关闭句柄。
    bool openXdmaAndSelfCheck();
    // 底层 XDMA 发送函数（复用既有接口）：
    // - forceSingleWrite=false：沿用历史分块发送逻辑，兼容已有测试包/普通发送；
    // - forceSingleWrite=true：用于视频批次路径，要求一次 write_device 完整写入。
    bool sendXdmaPayload(const QByteArray &payload,
                         const QString &label,
                         bool verbose = true,
                         bool forceSingleWrite = false);
    bool sendXdmaTestPacket();
    // AXI lite 寄存器读写（通过 user 通道）：
    // - 地址与数据均按 32bit 处理；
    // - 地址要求 4 字节对齐；
    // - 如 user 通道未打开，内部会尝试自动打开 XDMA。
    // 返回值约定：
    // - true：访问成功；
    // - false：参数非法、通道不可用或底层 read/write 失败。
    bool readUserRegister(quint32 address, quint32 &value);
    bool writeUserRegister(quint32 address, quint32 value);
    // 解析 UI 输入的寄存器地址/数值（支持 0x 前缀或十进制）。
    // 解析结果限制在 uint32 范围内，超界或格式错误会返回 false 并写日志。
    bool parseUiRegisterValue(const QString &text,
                              quint32 &outValue,
                              const QString &fieldName);

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

    // 发送调参：节流间隔和写入批次大小都支持界面实时调整。
    // m_liveStreamThrottleMs：控制最小发送间隔（毫秒）。
    // m_xdmaChunkBytes：
    // 1) 视频主链路中，控制每次向 XDMA 写入的批次大小；
    // 2) 非单写路径中，仍作为 write_device 的最大分块字节数。
    qint64 m_liveStreamThrottleMs = 40;
    int m_xdmaChunkBytes = 1024 * 1024;

    // 对应的界面调参控件指针。
    QSpinBox *m_throttleSpin = nullptr;
    QSpinBox *m_chunkSizeSpin = nullptr;
    // AXI lite 寄存器调试区控件：
    // - m_regAddrEdit：寄存器地址输入；
    // - m_regWriteValueEdit：写寄存器数据输入；
    // - m_regReadValueEdit：读寄存器结果显示（只读）。
    QLineEdit *m_regAddrEdit = nullptr;
    QLineEdit *m_regWriteValueEdit = nullptr;
    QLineEdit *m_regReadValueEdit = nullptr;

    // 最近一次采集帧缓存，用于手动一键发送。
    QByteArray m_lastCapturedFramePayload;
    QString m_lastCapturedFrameLabel;

    // 视频流封包+聚合模块（1024B 包 -> 可配置批次，默认 1MiB）。
    VideoPacketBatcher m_videoPacketBatcher;
};

#endif // 头文件保护宏 WIDGET_H
