#ifndef VIDEO_PACKET_BATCHER_H
#define VIDEO_PACKET_BATCHER_H

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>
#include <array>

// VideoPacketBatcher 职责：
// 1) 将原始视频字节流按协议封成固定 1024B 包；
// 2) 按可配置批次聚合输出待发送批次（默认 1MiB=1024 个包）；
// 3) 缓存未满一个批次的尾部，等待后续输入补齐。
class VideoPacketBatcher
{
public:
    // ===== 协议数据模型 =====
    // 路由字段定义：直接映射到协议包头固定偏移。
    struct RouteFields {
        std::array<quint8, 6> dest{};
        std::array<quint8, 6> source{};
        std::array<quint8, 2> priority{};
    };

    // enqueue 后统计信息，供日志和联调观察。
    struct EnqueueResult {
        int inputBytes = 0;
        int packetCount = 0;
        int packetBytes = 0;
        int emittedBatchCount = 0;
        int emittedBatchBytes = 0;
        int cachedBytes = 0;
    };

    // 协议常量：
    // 1024B = 2B同步头 + 2B长度 + 6B目的 + 6B源 + 2B优先级 + 1006B载荷。
    // 1MiB 聚合 = 1024 个协议包。
    static constexpr int kPacketSize = 1024;
    static constexpr int kPayloadSize = 1006;
    static constexpr int kHeaderSize = 18;
    static constexpr int kBatchBytes = 1024 * 1024;

    // ===== 配置与构造 =====
    // 默认路由字段集中管理入口，避免散落硬编码。
    static RouteFields defaultRouteFields();

    explicit VideoPacketBatcher(const RouteFields &routeFields = defaultRouteFields());

    // ===== 运行时处理 =====
    // 设置聚合批次大小（单位：字节）。
    // 要求：
    // 1) batchBytes >= 1024；
    // 2) batchBytes 为 1024 的整数倍，确保不打断协议包边界。
    // 返回 true 表示设置成功。
    bool setBatchBytes(int batchBytes);

    // 查询当前聚合批次大小（单位：字节）。
    int batchBytes() const;

    // 原始视频字节流 -> 协议包流 -> 固定批次输出（批次大小可配置）。
    // outBatches 只返回本次刚凑满的批次；不足一个批次的尾部保留在内部缓存。
    EnqueueResult enqueueVideoPayload(const QByteArray &videoPayload,
                                      QVector<QByteArray> &outBatches);

    // 查询当前缓存区尚未输出的字节数（范围 [0, 当前批次大小)）。
    int pendingBytes() const;

    // ===== 诊断与自测 =====
    // 纯软件自测（不依赖 XDMA 设备）：
    // 校验包格式、length 编码、补零，以及“默认 1MiB”聚合边界行为。
    static bool runSelfTest(QString *report = nullptr);

private:
    // 内部封包函数：按协议切分为 1024B 定长包流。
    // 每包 payload 有效长度写入 lengthH/lengthL，不足部分补 0。
    QByteArray packetizeVideoPayload(const QByteArray &videoPayload,
                                     int *packetCount = nullptr) const;

    RouteFields m_routeFields;
    int m_batchBytes = kBatchBytes;
    QByteArray m_batchCache;
};

#endif // VIDEO_PACKET_BATCHER_H
