# Camera PC Linux GUI

基于 Qt Widgets 的 Linux 上位机程序，目标是与 Windows 版本保持功能一致。

当前开发分支：`linux-support`

## 代码结构

```text
Camera_PC/
├── CMakeLists.txt
├── README.md
├── load_xdma.sh
├── include/
│   ├── cameraprobe.h
│   ├── video_packet_batcher.h
│   ├── widget.h
│   └── xdmaDLL_public_linux.h
└── src/
    ├── XDMA_MoreB_linux.cc
    ├── cameraprobe.cpp
    ├── main.cpp
    ├── video_packet_batcher.cpp
    ├── widget.cpp
    └── widget.ui
```

## 快速开始

### 1. 获取代码并切到 Linux 分支

```bash
git clone https://github.com/jingzezhang818/Camera_PC.git
cd Camera_PC
git checkout linux-support
```

### 2. 安装依赖（Ubuntu/WSL）

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config \
  qtbase5-dev qtmultimedia5-dev qttools5-dev-tools \
  libqt5multimedia5-plugins libqt5multimediawidgets5 \
  v4l-utils \
  fonts-noto-cjk fonts-wqy-microhei fonts-wqy-zenhei
```

### 3. 编译

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

### 4. 加载 XDMA 驱动并运行 GUI

```bash
./load_xdma.sh
./build/camera_pc_gui
```

说明：
- `load_xdma.sh` 会在需要时自动通过 `sudo` 提权，不需要手动 `sudo su`。
- 默认以 MSI 模式加载驱动，等价于 `./load_xdma.sh 1`。

## XDMA 驱动前置条件

`load_xdma.sh` 默认会查找以下目录：

```text
../dma_ip_drivers/XDMA/linux-kernel/tests
```

请确认该目录存在，并且其中包含 `load_driver.sh`。

## 常用检查命令

```bash
ls /dev/video*
v4l2-ctl --list-devices
ls /dev/xdma*
```

## 备注

- `ready_state`、`reset` 返回值语义目前直接透传底层 XDMA 接口。
- `op_state`、`ddr_state` 的寄存器语义仍需与 FPGA 文档最终对齐。
Linux 枚举/预览方案
可以做，而且建议不要继续依赖 Qt 的 QCameraViewfinderSettings 作为唯一模式来源。Linux 侧建议新增一个 V4L2/GStreamer 预览会话，结构对齐 Win 的 DirectShow session：

用 V4L2 直接枚举设备：

VIDIOC_ENUM_FMT
VIDIOC_ENUM_FRAMESIZES
VIDIOC_ENUM_FRAMEINTERVALS
对 DISCRETE 帧率列出具体 fps。
对 STEPWISE/CONTINUOUS 只显示范围/步进，不能伪造成确定支持的离散 fps。
应用模式时用 V4L2 精确请求：

VIDIOC_S_FMT 设置宽高和 YUYV/MJPG。
VIDIOC_S_PARM 设置 timeperframe。
再用 VIDIOC_G_FMT、VIDIOC_G_PARM 读取驱动实际接受的分辨率和帧率。
如果驱动调整了参数，UI 日志明确显示 requested 和 accepted 的差异。
预览建议用 GStreamer：

v4l2src ! capsfilter ! tee
一路到 native video sink 做预览，比如 xvimagesink/waylandsink/ximagesink fallback。
一路到 appsink 拿 raw YUYV callback，继续走现有 XDMA 发送链路。
预览窗口 resize 只改 sink 显示区域，不改采集分辨率和帧率。
UI 显示实际状态：

显示 requested mode：用户选择的模式。
显示 accepted mode：V4L2 实际接受的宽高/格式/fps。
显示 measured fps：appsink/raw callback 按 1 秒滑动窗口统计出来的真实到帧率。
这样 Linux 可以做到和 Win 类似：模式从驱动枚举来，应用后读取实际接受值，预览和 raw callback 双分支，XDMA 协议保持不变。