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
│   ├── linuxpreviewsession.h
│   ├── video_packet_batcher.h
│   ├── widget.h
│   └── xdmaDLL_public_linux.h
└── src/
    ├── XDMA_MoreB_linux.cc
    ├── cameraprobe.cpp
    ├── linuxpreviewsession.cpp
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
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
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

## Linux 预览链路

Linux 实时预览使用 `LinuxPreviewSession`，不再依赖 Qt `QCameraViewfinder` / `QVideoProbe`：

```text
V4L2 枚举/设置模式
  -> GStreamer v4l2src + capsfilter(YUY2) + tee
     -> native video sink 预览
     -> appsink raw YUYV callback
        -> VideoPacketBatcher
        -> XDMA h2c_0
```

模式选择规则：

- 只把 V4L2 `DISCRETE` 的 `YUYV/YUY2` 模式加入下拉框。
- `STEPWISE/CONTINUOUS` 帧率只写日志，不作为确定 fps 选项展示。
- 应用模式时先请求 `VIDIOC_S_FMT` / `VIDIOC_S_PARM`，再读取 `VIDIOC_G_FMT` / `VIDIOC_G_PARM`。
- UI 日志会分别显示 requested mode 和 driver accepted mode；如果驱动接受的格式或分辨率不是请求值，预览会拒绝启动。

`采一帧` 仍暂时保留 Qt `CameraProbe` 路径，和实时预览/实时发送链路解耦。
