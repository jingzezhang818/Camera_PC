# camera_PC

`camera_PC` 是一个基于 Qt 的摄像头采集与 XDMA 联调项目。核心目标是将摄像头原始视频流在 PC 侧先按协议封包，再按可配置批次稳定写入 FPGA，同时提供 user 通道 AXI-Lite 寄存器读写能力。

## 1. 项目思路

项目按四层拆分，避免业务流程和底层硬件细节耦合：

1. 采集层（`cameraprobe.*`）
- 枚举相机模式。
- one-shot 单帧采集。

2. 协议层（`video_packet_batcher.*`）
- 原始视频字节流切分为固定 `1024B` 协议包。
- 包格式固定：`EB 90 | lengthH | lengthL | dest(6) | source(6) | priority(2) | payload(1006)`。

3. 发送层（`widget.cpp`）
- 协议包按批次聚合（默认 `1MiB`，可配置）。
- 每个批次走 `h2c_0` 单次写入。

4. 控制层（`widget.ui + widget.*`）
- UI 按钮负责联调流程。
- 运行时调参和 AXI-Lite 寄存器读写面板负责快速调试。

## 2. 核心功能

1. 摄像头模式枚举、单帧抓取、预览。
2. 视频数据协议封包（1024B 固定包）。
3. 批次聚合发送（默认 1MiB，可通过 UI 改写入大小）。
4. XDMA 通道打开与 `ready_state` 自检。
5. XDMA 链路测试包发送。
6. 协议封包模块手动自测。
7. AXI-Lite 寄存器读写（通过 user 通道）。

## 3. 协议与发送规则

### 3.1 固定 1024B 协议包

- 每 `1006B` 原始视频数据生成 1 包。
- `lengthH/lengthL` 填写整包总长度（包头+payload），固定为 `0x0400`。
- 最后一包不足 `1006B` 也会生成，剩余 payload 自动补零。

### 3.2 批次聚合写入

- 协议包按“当前批次大小”累计后发送。
- 默认批次 `1MiB`。
- 批次大小由 UI 的 `写入大小(KB)` 控件控制。
- 批次大小必须是 `1024B` 的整数倍，保证不打断协议包边界。

## 4. UI 功能分组

### 4.1 采集与视频发送

- `列出模式`
- `采一帧`
- `发送缓存帧(封包+批量)`
- `开始/停止实时视频发送(封包+批量)`

### 4.2 XDMA 与测试

- `打开XDMA通道并自检`
- `发送XDMA链路测试包`
- `运行协议封包自测`

### 4.3 运行时调参

- `节流间隔(ms)`：控制实时发送最小间隔。
- `写入大小(KB)`：控制视频主链路每次向 XDMA 写入的批次大小。

### 4.4 AXI-Lite 寄存器读写（新增）

- `寄存器地址`：支持 `0x..` 或十进制输入。
- `写入值`：支持 `0x..` 或十进制输入。
- `读寄存器`：读取 user 通道指定地址 32bit 值，显示在 `读回值`。
- `写寄存器`：向 user 通道指定地址写入 32bit 值。

约束：
- 地址必须 4 字节对齐。
- 输入范围是 `uint32`。
- 若 user 通道未打开，会自动尝试执行 XDMA 打开与自检。

## 5. 调用链

### 5.1 视频发送链路

`相机帧` -> `sendVideoPayloadWithBatching()` -> `VideoPacketBatcher::enqueueVideoPayload()` -> `sendXdmaPayload(..., forceSingleWrite=true)` -> `h2c_0`

### 5.2 寄存器读链路

`读寄存器按钮` -> `parseUiRegisterValue()` -> `readUserRegister()` -> `read_device(user, addr, 4, ...)`

### 5.3 寄存器写链路

`写寄存器按钮` -> `parseUiRegisterValue()` -> `writeUserRegister()` -> `write_device(user, addr, 4, ...)`

## 6. 推荐联调流程

1. 点 `打开XDMA通道并自检`，确认 user/h2c_0 就绪。
2. 点 `发送XDMA链路测试包`，确认基础链路可写。
3. 点 `运行协议封包自测`，确认封包与聚合逻辑。
4. 设置 `写入大小(KB)`。
5. 开始 `采一帧` + `发送缓存帧(封包+批量)`，或启动实时发送。
6. 需要寄存器调试时，直接在 AXI-Lite 面板读写地址。

## 7. 日志速查

- `[OK] XDMA open complete: user + h2c_0 ready.`
- `[CFG] XDMA write size set to ... KB`
- `[PKG] ... raw=... packets=... emitted=... x ...KB, cached=...`
- `[SELFTEST] PASS ...`
- `[AXIL] READ addr=... -> value=...`
- `[AXIL] WRITE addr=... <- value=...`
- `[AXIL][ERROR] ...`

## 8. 代码结构

- `widget.h / widget.cpp`：主流程、UI、XDMA、寄存器读写。
- `cameraprobe.h / cameraprobe.cpp`：相机枚举与单帧采集。
- `video_packet_batcher.h / video_packet_batcher.cpp`：封包与批次聚合。
- `widget.ui`：主界面布局。
- `driver/`：XDMA 动态库与导入库。

## 9. 备注

- 默认路由字段在 `VideoPacketBatcher::defaultRouteFields()` 集中配置。
- 寄存器读写走 user 通道，不经过 h2c_0 视频发送路径。
