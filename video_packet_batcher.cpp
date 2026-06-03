#include "video_packet_batcher.h"

#include <QStringList>
#include <cstring>

// ===== 配置与构造 =====

VideoPacketBatcher::RouteFields VideoPacketBatcher::defaultRouteFields()
{
    // 当前项目暂无外部路由配置入口，先用固定默认值集中管理。
    return RouteFields{
        {0x00, 0x00, 0x00, 0x10, 0x20, 0x05}, //dest
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x02}, //sour
        {0x00, 0x01}
    };
}

VideoPacketBatcher::VideoPacketBatcher(const RouteFields &routeFields)
    : m_routeFields(routeFields)
{
    // 默认按 1MiB 预留，可减少持续流输入时的扩容开销。
    m_batchCache.reserve(m_batchBytes);
    m_payloadCache.reserve(kPayloadSize);
}

// ===== 运行时处理 =====

bool VideoPacketBatcher::setBatchBytes(int batchBytes)
{
    // 聚合批次必须按完整协议包对齐，避免打断 1024B 包边界。
    if (batchBytes < kPacketSize || (batchBytes % kPacketSize) != 0) {
        return false;
    }

    m_batchBytes = batchBytes;
    if (m_batchCache.capacity() < m_batchBytes) {
        m_batchCache.reserve(m_batchBytes);
    }
    return true;
}

int VideoPacketBatcher::batchBytes() const
{
    return m_batchBytes;
}

int VideoPacketBatcher::pendingBytes() const
{
    // 返回总缓存：协议包缓存 + 原始尾部缓存。
    return m_batchCache.size() + m_payloadCache.size();
}

int VideoPacketBatcher::pendingPayloadBytes() const
{
    return m_payloadCache.size();
}

void VideoPacketBatcher::discardPendingPayloadBytes()
{
    m_payloadCache.clear();
}

void VideoPacketBatcher::clear()
{
    m_batchCache.clear();
    m_payloadCache.clear();
}

QByteArray VideoPacketBatcher::buildPacketStream(const QByteArray &videoPayload,
                                                 int *packetCount,
                                                 int minPacketCount) const
{
    return packetizeVideoPayload(videoPayload, packetCount, minPacketCount);
}

QByteArray VideoPacketBatcher::packetizeVideoPayload(const QByteArray &videoPayload,
                                                     int *packetCount,
                                                     int minPacketCount) const
{
    if (packetCount) {
        *packetCount = 0;
    }

    if (videoPayload.isEmpty() && minPacketCount <= 0) {
        return {};
    }

    // 包数量向上取整：
    // - 每 1006B 生成 1 包；
    // - 最后一包不足 1006B 也必须生成。
    const int totalBytes = videoPayload.size();
    const int packetCountByPayload = (totalBytes + kPayloadSize - 1) / kPayloadSize;
    const int localPacketCount = qMax(packetCountByPayload, qMax(0, minPacketCount));

    QByteArray packetStream(localPacketCount * kPacketSize, '\0');
    const char *src = videoPayload.constData();
    int remaining = totalBytes;

    for (int i = 0; i < localPacketCount; ++i) {
        const int payloadLen = qMin(remaining, kPayloadSize);
        uchar *packet = reinterpret_cast<uchar *>(packetStream.data() + i * kPacketSize);

        // 固定同步头：EB 90。
        packet[0] = 0xEB;
        packet[1] = 0x90;

        // lengthH/lengthL：协议定义为“整包总长度（包头+payload）”，固定 1024(0x0400)。
        packet[2] = static_cast<uchar>((kPacketSize >> 8) & 0xFF);
        packet[3] = static_cast<uchar>(kPacketSize & 0xFF);

        // 包头路由字段偏移：
        // [4..9] dest(6B), [10..15] source(6B), [16..17] priority(2B)。
        std::memcpy(packet + 4, m_routeFields.dest.data(), m_routeFields.dest.size());
        std::memcpy(packet + 10, m_routeFields.source.data(), m_routeFields.source.size());
        std::memcpy(packet + 16, m_routeFields.priority.data(), m_routeFields.priority.size());

        if (payloadLen > 0) {
            // payload 固定起始偏移 18，拷贝 payloadLen 字节有效数据。
            std::memcpy(packet + kHeaderSize, src, static_cast<size_t>(payloadLen));
        }

        // packetStream 初始化为 0，因此 payload 剩余区域天然满足补零要求。
        src += payloadLen;
        remaining -= payloadLen;
    }

    if (packetCount) {
        *packetCount = localPacketCount;
    }

    return packetStream;
}

VideoPacketBatcher::EnqueueResult VideoPacketBatcher::enqueueVideoPayload(
        const QByteArray &videoPayload,
        QVector<QByteArray> &outBatches)
{
    EnqueueResult result;
    result.inputBytes = videoPayload.size();
    outBatches.clear();

    if (!videoPayload.isEmpty()) {
        m_payloadCache.append(videoPayload);
    }

    const int fullPacketCount = m_payloadCache.size() / kPayloadSize;
    if (fullPacketCount <= 0) {
        result.pendingPayloadBytes = m_payloadCache.size();
        result.cachedBytes = m_batchCache.size() + result.pendingPayloadBytes;
        return result;
    }

    const int sourceBytes = fullPacketCount * kPayloadSize;
    QByteArray packetStream(fullPacketCount * kPacketSize, '\0');
    const char *src = m_payloadCache.constData();

    for (int i = 0; i < fullPacketCount; ++i) {
        uchar *packet = reinterpret_cast<uchar *>(packetStream.data() + i * kPacketSize);

        packet[0] = 0xEB;
        packet[1] = 0x90;
        packet[2] = static_cast<uchar>((kPacketSize >> 8) & 0xFF);
        packet[3] = static_cast<uchar>(kPacketSize & 0xFF);

        std::memcpy(packet + 4, m_routeFields.dest.data(), m_routeFields.dest.size());
        std::memcpy(packet + 10, m_routeFields.source.data(), m_routeFields.source.size());
        std::memcpy(packet + 16, m_routeFields.priority.data(), m_routeFields.priority.size());
        std::memcpy(packet + kHeaderSize,
                    src + i * kPayloadSize,
                    static_cast<size_t>(kPayloadSize));
    }

    m_payloadCache.remove(0, sourceBytes);

    result.packetCount = fullPacketCount;
    result.packetBytes = packetStream.size();

    for (int offset = 0; offset < packetStream.size(); offset += kPacketSize) {
        // 以完整协议包粒度入缓存，确保聚合不会打断包边界。
        m_batchCache.append(packetStream.constData() + offset, kPacketSize);
        while (m_batchCache.size() >= m_batchBytes) {
            ++result.emittedBatchCount;
            // 每凑满一个批次输出一次，剩余字节继续缓存。
            outBatches.append(m_batchCache.left(m_batchBytes));
            m_batchCache.remove(0, m_batchBytes);
        }
    }

    result.emittedBatchBytes = result.emittedBatchCount * m_batchBytes;
    result.pendingPayloadBytes = m_payloadCache.size();
    result.cachedBytes = m_batchCache.size() + result.pendingPayloadBytes;
    return result;
}

// ===== 诊断与自测 =====

bool VideoPacketBatcher::runSelfTest(QString *report)
{
    QStringList errors;
    const RouteFields route = defaultRouteFields();

    // 样本 1：1500B，流式输入不在每次 enqueue 末尾补零。
    // 应只生成 1 个完整包，并保留 494B 原始尾部缓存。
    QByteArray sample(1500, '\0');
    for (int i = 0; i < sample.size(); ++i) {
        sample[i] = static_cast<char>(i & 0xFF);
    }

    VideoPacketBatcher streamBatcher(route);
    QVector<QByteArray> streamBatches;
    const auto streamResult = streamBatcher.enqueueVideoPayload(sample, streamBatches);
    if (streamResult.packetCount != 1) {
        errors << QString("stream packet count mismatch, expect=1 actual=%1")
                  .arg(streamResult.packetCount);
    }
    if (streamResult.pendingPayloadBytes != 494) {
        errors << QString("stream pending payload mismatch, expect=494 actual=%1")
                  .arg(streamResult.pendingPayloadBytes);
    }
    if (!streamBatches.isEmpty()) {
        errors << QString("stream should not emit batch, actual=%1")
                  .arg(streamBatches.size());
    }
    streamBatcher.clear();
    if (streamBatcher.pendingBytes() != 0 || streamBatcher.pendingPayloadBytes() != 0) {
        errors << QString("clear cache mismatch, pending=%1 payloadTail=%2")
                  .arg(streamBatcher.pendingBytes())
                  .arg(streamBatcher.pendingPayloadBytes());
    }

    // 样本 1.1：仅验证“完整 payload”封包格式（length 固定 0x0400）。
    VideoPacketBatcher packetizer(route);
    int packetCount = 0;
    const QByteArray packetized =
            packetizer.packetizeVideoPayload(QByteArray(kPayloadSize, '\x5A'), &packetCount);
    if (packetCount != 1) {
        errors << QString("packet count mismatch, expect=1 actual=%1").arg(packetCount);
    }
    if (packetized.size() != kPacketSize) {
        errors << QString("packetized bytes mismatch, expect=%1 actual=%2")
                  .arg(kPacketSize)
                  .arg(packetized.size());
    }
    if (packetized.size() >= kPacketSize) {
        const uchar *pkt0 = reinterpret_cast<const uchar *>(packetized.constData());
        const int len0 = (static_cast<int>(pkt0[2]) << 8) | static_cast<int>(pkt0[3]);
        if (!(pkt0[0] == 0xEB && pkt0[1] == 0x90)) {
            errors << QStringLiteral("frame header mismatch, expect EB 90");
        }
        if (len0 != kPacketSize) {
            errors << QString("packet0 length mismatch, expect=%1 actual=%2")
                      .arg(kPacketSize)
                      .arg(len0);
        }

        if (std::memcmp(pkt0 + 4, route.dest.data(), route.dest.size()) != 0) {
            errors << QStringLiteral("dest field mismatch");
        }

        if (std::memcmp(pkt0 + 10, route.source.data(), route.source.size()) != 0) {
            errors << QStringLiteral("source field mismatch");
        }

        if (std::memcmp(pkt0 + 16, route.priority.data(), route.priority.size()) != 0) {
            errors << QStringLiteral("priority field mismatch");
        }

        for (int i = 0; i < kPayloadSize; ++i) {
            if (pkt0[kHeaderSize + i] != static_cast<uchar>(0x5A)) {
                errors << QString("packet payload mismatch at byte=%1").arg(i);
                break;
            }
        }
    }

    // 样本 2：聚合边界。连续 1024 次“刚好 1 包”输入，必须只输出 1 个 1MiB 批次。
    VideoPacketBatcher aggregator(route);
    QVector<QByteArray> batches;
    QByteArray payload1006(kPayloadSize, 'Z');

    for (int i = 0; i < 1024; ++i) {
        const auto result = aggregator.enqueueVideoPayload(payload1006, batches);
        if (i < 1023 && result.emittedBatchCount != 0) {
            errors << QString("unexpected batch emitted at index=%1").arg(i);
            break;
        }
    }

    if (batches.size() != 1) {
        errors << QString("batch count mismatch, expect=1 actual=%1").arg(batches.size());
    } else if (batches.first().size() != kBatchBytes) {
        errors << QString("batch bytes mismatch, expect=%1 actual=%2")
                  .arg(kBatchBytes)
                  .arg(batches.first().size());
    }

    if (aggregator.pendingBytes() != 0) {
        errors << QString("pending cache mismatch, expect=0 actual=%1")
                  .arg(aggregator.pendingBytes());
    }

    const bool ok = errors.isEmpty();
    if (report) {
        if (ok) {
            *report = QStringLiteral("PASS packet-format, cache clear, and 1MiB aggregation");
        } else {
            *report = QStringLiteral("FAIL: ") + errors.join(QStringLiteral("; "));
        }
    }
    return ok;
}
