# camera_PC

`camera_PC` 是一个基于 Qt 的相机采集 + XDMA 发送工具，用于把相机原始视频流按固定协议封包后，批量写入 FPGA 侧 `h2c_0`，并提供 `user` 通道 AXI-Lite 寄存器读写能力。

## 1. 项目目标

- 在 PC 侧完成视频 payload 协议封包。
- 以可配置批次（默认 1 MiB）聚合后一次写入 XDMA。
- 在同一界面完成链路自检、协议自测与寄存器联调。

## 2. 主要功能

- 相机模式枚举、单帧采集、实时预览。
- 原始视频流封包为固定 `1024B` 协议包。
- 协议包按批次聚合发送（批次大小可在 UI 调整）。
- `user + h2c_0` 通道打开与 ready 状态自检。
- XDMA 链路测试包发送。
- 协议模块内存自测（不依赖硬件）。
- AXI-Lite 寄存器读写（通过 `user` 通道）。

## 3. 协议与批量发送

### 3.1 固定 1024B 协议包

协议包格式：

`EB 90 | lengthH | lengthL | dest(6) | source(6) | priority(2) | payload(1006)`

规则：

- 每累计满 `1006B` 原始 payload 才生成 `1` 个协议包。
- `length` 固定为 `0x0400`（1024）。
- 不再在每次输入末尾强制补零收尾。
- 不足 `1006B` 的尾部原始字节会缓存到下一次输入继续拼接。

### 3.2 批量聚合与节流

- 协议包先聚合为“完整批次”，默认 `1 MiB`。
- 批次大小由 UI 的 `写入大小(KB)` 控制，必须是 `1024B` 的整数倍。
- 实时模式下“节流”只限制 XDMA 写入频率，不丢弃输入帧数据。
- 已产出的完整批次会进入待发队列，在允许发送窗口内按“一批次一次 write”发送。

## 4. 实时发送行为说明

- 开始实时发送：进入封包 + 批量发送流程。
- 停止实时发送时会：
- 统计并输出已发送批次数。
- 清理未满 `1006B` 的 payload 尾部缓存（记为 `dropped-tail`）。
- 输出当前未发缓存字节，便于联调排查。

## 5. 构建环境

- Qt：`core gui widgets multimedia multimediawidgets`
- C++ 标准：`C++17`
- 平台：Windows（`camera_PC.pro` 已配置 `driver/` 下 XDMA 库链接）

建议使用 Qt Creator 打开 `camera_PC.pro` 构建运行。

## 6. 推荐联调流程

1. 点击 `打开XDMA通道并自检`，确认 `user/h2c_0` 就绪。
2. 点击 `发送XDMA链路测试包`，确认基础写通路可用。
3. 点击 `运行协议封包自测`，确认封包逻辑正常。
4. 设置 `写入大小(KB)` 与 `节流间隔(ms)`。
5. 执行 `采一帧 + 发送缓存帧(封包+批量)`，或直接开启实时发送。
6. 需要寄存器联调时，使用 AXI-Lite 面板读写目标地址。

## 7. 关键日志

- `[OK] XDMA open complete: user + h2c_0 ready.`
- `[CFG] XDMA write size set to ... KB`
- `[PKG] ... raw=... packets=... produced=... queued=... cached=... payload-tail=...`
- `[SELFTEST] PASS ...`
- `[AXIL] READ addr=... -> value=...`
- `[AXIL] WRITE addr=... <- value=...`
- `[AXIL][ERROR] ...`

## 8. 代码结构

- `widget.h / widget.cpp`：UI、主流程、XDMA、实时发送与寄存器读写。
- `cameraprobe.h / cameraprobe.cpp`：相机模式枚举与帧采集。
- `video_packet_batcher.h / video_packet_batcher.cpp`：协议封包与批量聚合。
- `widget.ui`：界面布局。
- `driver/`：XDMA 相关头文件与库。
- `doc/寄存器列表.docx`：寄存器文档。
