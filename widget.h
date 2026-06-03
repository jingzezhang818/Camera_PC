#ifndef WIDGET_H
#define WIDGET_H

// 涓荤獥鍙ｇ被锛?
// - 璐熻矗鐩告満棰勮涓庡崟甯ф姄鍙栵紱
// - 璐熻矗 XDMA 閫氶亾鎵撳紑銆佽嚜妫€涓庡彂閫侊紱
// - 璐熻矗瀹炴椂棰勮甯у埌 XDMA 鐨勬祦寮忓彂閫侊紱
// - 璐熻矗 UI 浜や簰涓庢棩蹇楄緭鍑恒€?

#include <QWidget>
#include <QCamera>
#include <QByteArray>
#include <QVector>
#include "cameraprobe.h"
#include "video_packet_batcher.h"

class DirectShowPreviewSession;
class QSpinBox;
class QLineEdit;
class QLabel;
class QComboBox;
class QPushButton;

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
    // ===== 鎸夐挳妲斤細閲囬泦涓庤棰戝彂閫?=====
    void on_btnListModes_clicked();
    void on_btnGrabOneFrame_clicked();
    void on_btnSendCapturedFrame_clicked();
    void on_btnSendLiveVideo_clicked();

    // ===== 鎸夐挳妲斤細XDMA 涓庢祴璇?=====
    void on_btnOpenXdma_clicked();
    void on_btnSendLinkTestPacket_clicked();
    void on_btnSendTestPacket_clicked();
    void on_btnClearLog_clicked();

    // ===== 鍥炶皟妲斤細CameraProbe =====
    void onProbeLog(const QString &msg);
    void onProbeSuccess(const CapturedFrame &frame);
    void onProbeFailed(const QString &reason);

    // ===== 鍥炶皟妲斤細棰勮閾捐矾 =====
    void onRawFrameAvailable(const CapturedFrame &frame);

private:
    // ===== 妯″潡锛氱敓鍛藉懆鏈熶笌棰勮鍒濆鍖?=====
    // 鍒濆鍖栧疄鏃?raw YUYV 棰勮銆?
    void initializePreview();

    // 鍒濆鍖栤€滆妭娴侀棿闅?鍐欏叆澶у皬鈥濊皟鍙傛帶浠讹紝骞剁粦瀹氳繍琛屾椂鍙傛暟銆?
    void initializeTransferControls();
    void initializeModeControls();
    // 鍒濆鍖?AXI lite 瀵勫瓨鍣ㄨ鍐欐帶浠讹紙鍦板潃/鍐欏€艰緭鍏ャ€佽鍊兼樉绀恒€佽鍐欐寜閽級銆?    // 璇ュ尯鍩熸槸鈥滃瘎瀛樺櫒璋冭瘯鍏ュ彛鈥濓紝涓庤棰戝彂閫侀摼璺В鑰︺€?
    void initializeAxiLiteControls();

    // 鍚姩/鍋滄瀹炴椂棰勮鐩告満銆?
    void startPreview();
    void stopPreview();
    void refreshModeCombo();
    void applySelectedModeFromCombo();
    void stopLiveVideoSending(const QString &reason = QString());
    void clearLiveVideoBuffers();
    bool selectedModeSupportsLiveStreaming(QString *reason = nullptr) const;
    bool findDefaultLiveYuyvMode(CameraModeInfo &outMode, QString *report) const;
    bool normalizeLiveYuyvFrame(const CapturedFrame &frame,
                                const QSize &expectedResolution,
                                QByteArray &payload,
                                QString *reason) const;

    // ===== 妯″潡锛氳棰戜笟鍔″彂閫侊紙灏佸寘 + 鑱氬悎锛?=====
    // 瑙嗛鍙戦€佷笓鐢ㄥ叆鍙ｏ細
    // 鍘熷瑙嗛娴?-> 1024B 鍗忚灏佸寘 -> 鍙厤缃壒娆¤仛鍚?-> sendXdmaPayload(single write)銆?
    bool sendVideoPayloadWithBatching(const QByteArray &videoPayload,
                                      const QString &label,
                                      bool verbose = true,
                                      bool allowSendNow = true);

    // 杞欢鑷祴鍏ュ彛锛堢函鍐呭瓨锛屼笉渚濊禆 XDMA 璁惧锛夛紝鏀逛负鈥滄墜鍔ㄨЕ鍙戔€濄€?
    void runPacketModuleSelfTest();

    // ===== 妯″潡锛歑DMA 搴曞眰閫氶亾涓庡彂閫?=====
    // 鍏抽棴 XDMA 鍙ユ焺锛岀‘淇濊祫婧愰噴鏀句笌鐘舵€佸浣嶃€?
    void closeXdmaHandles();
    // XDMA 閫氶亾浼氳瘽鐢熷懡鍛ㄦ湡锛?    // 1) 鏋氫妇璁惧骞舵墦寮€ user + h2c_0锛?    // 2) 璋冪敤 ready_state 鍋氳交閲忚嚜妫€锛?    // 3) 灏嗚礋杞藉啓鍏?h2c_0锛?    // 4) 绋嬪簭閫€鍑烘垨閲嶈繛鏃跺叧闂彞鏌勩€?
    bool openXdmaAndSelfCheck();
    // 搴曞眰 XDMA 鍙戦€佸嚱鏁帮紙澶嶇敤鏃㈡湁鎺ュ彛锛夛細
    // - forceSingleWrite=false锛氭部鐢ㄥ巻鍙插垎鍧楀彂閫侀€昏緫锛屽吋瀹瑰凡鏈夋祴璇曞寘/鏅€氬彂閫侊紱
    // - forceSingleWrite=true锛氱敤浜庤棰戞壒娆¤矾寰勶紝瑕佹眰涓€娆?write_device 瀹屾暣鍐欏叆銆?
    bool sendXdmaPayload(const QByteArray &payload,
                         const QString &label,
                         bool verbose = true,
                         bool forceSingleWrite = false);
    bool sendXdmaTestPacket();
    // AXI lite 瀵勫瓨鍣ㄨ鍐欙紙閫氳繃 user 閫氶亾锛夛細
    // - 鍦板潃涓庢暟鎹潎鎸?32bit 澶勭悊锛?    // - 鍦板潃瑕佹眰 4 瀛楄妭瀵归綈锛?    // - 濡?user 閫氶亾鏈墦寮€锛屽唴閮ㄤ細灏濊瘯鑷姩鎵撳紑 XDMA銆?    // 杩斿洖鍊肩害瀹氾細
    // - true锛氳闂垚鍔燂紱
    // - false锛氬弬鏁伴潪娉曘€侀€氶亾涓嶅彲鐢ㄦ垨搴曞眰 read/write 澶辫触銆?
    bool readUserRegister(quint32 address, quint32 &value);
    bool writeUserRegister(quint32 address, quint32 value);
    // 瑙ｆ瀽 UI 杈撳叆鐨勫瘎瀛樺櫒鍦板潃/鏁板€硷紙鏀寔 0x 鍓嶇紑鎴栧崄杩涘埗锛夈€?    // 瑙ｆ瀽缁撴灉闄愬埗鍦?uint32 鑼冨洿鍐咃紝瓒呯晫鎴栨牸寮忛敊璇細杩斿洖 false 骞跺啓鏃ュ織銆?
    bool parseUiRegisterValue(const QString &text,
                              quint32 &outValue,
                              const QString &fieldName);

    // Qt Designer 鐢熸垚鐨?UI 瀵硅薄銆?
    Ui::Widget *ui;

    // 鍗曞抚鎶撳彇鎺㈤拡锛堜笌瀹炴椂棰勮閾捐矾瑙ｈ€︼級銆?
    CameraProbe *m_probe = nullptr;

    // 瀹炴椂棰勮閾捐矾瀵硅薄銆?
    DirectShowPreviewSession *m_directShowPreview = nullptr;
    QWidget *m_previewWidget = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QPushButton *m_applyModeBtn = nullptr;
    QList<CameraModeInfo> m_availableModes;
    bool m_useManualPreviewMode = false;
    CameraModeInfo m_manualPreviewMode;
    bool m_waitingPreviewSingleFrame = false;

    // 鎶撳彇鍗曞抚鍓嶄細鏆傚仠棰勮锛屾姄鍙栫粨鏉熷悗鏍规嵁璇ユ爣璁版仮澶嶃€?
    bool m_restartPreviewAfterCapture = false;

    // XDMA 杩愯鏃朵細璇濆瓧娈点€?
    QString m_xdmaDevicePath;
    void *m_xdmaUserHandle = nullptr;
    void *m_xdmaH2c0Handle = nullptr;

    // 瀹炴椂娴佸彂閫佺姸鎬侊紙棰勮甯?-> h2c_0锛夈€?
    bool m_liveVideoSending = false;
    qint64 m_liveSendStartMs = 0;
    qint64 m_lastLiveSendMs = 0;
    int m_liveSentBatches = 0;
    QVector<QByteArray> m_liveReadyBatches;

    // 鍙戦€佽皟鍙傦細鑺傛祦闂撮殧鍜屽啓鍏ユ壒娆″ぇ灏忛兘鏀寔鐣岄潰瀹炴椂璋冩暣銆?
    // m_liveStreamThrottleMs锛氭帶鍒舵渶灏忓彂閫侀棿闅旓紙姣锛夈€?
    // m_xdmaChunkBytes锛?
    // 1) 瑙嗛涓婚摼璺腑锛屾帶鍒舵瘡娆″悜 XDMA 鍐欏叆鐨勬壒娆″ぇ灏忥紱
    // 2) 闈炲崟鍐欒矾寰勪腑锛屼粛浣滀负 write_device 鐨勬渶澶у垎鍧楀瓧鑺傛暟銆?
    qint64 m_liveStreamThrottleMs = 40;
    int m_xdmaChunkBytes = 1024 * 1024;

    // 瀵瑰簲鐨勭晫闈㈣皟鍙傛帶浠舵寚閽堛€?
    QSpinBox *m_throttleSpin = nullptr;
    QSpinBox *m_chunkSizeSpin = nullptr;
    // AXI lite 瀵勫瓨鍣ㄨ皟璇曞尯鎺т欢锛?    // - m_regAddrEdit锛氬瘎瀛樺櫒鍦板潃杈撳叆锛?    // - m_regWriteValueEdit锛氬啓瀵勫瓨鍣ㄦ暟鎹緭鍏ワ紱
    // - m_regReadValueEdit锛氳瀵勫瓨鍣ㄧ粨鏋滄樉绀猴紙鍙锛夈€?
    QLineEdit *m_regAddrEdit = nullptr;
    QLineEdit *m_regWriteValueEdit = nullptr;
    QLineEdit *m_regReadValueEdit = nullptr;

    // 鏈€杩戜竴娆♀€滈噰涓€甯р€濇垚鍔熶繚瀛樼殑 raw 鏂囦欢璺緞涓庢爣绛撅紝鐢ㄤ簬鎵嬪姩涓€閿彂閫併€?
    QString m_lastCapturedRawPath;
    QString m_lastCapturedFrameLabel;

    // 瑙嗛娴佸皝鍖?鑱氬悎妯″潡锛?024B 鍖?-> 鍙厤缃壒娆★紝榛樿 1MiB锛夈€?
    VideoPacketBatcher m_videoPacketBatcher;
};

#endif // 澶存枃浠朵繚鎶ゅ畯 WIDGET_H
