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

## 编译运行依赖

项目使用 CMake + Qt5 Widgets + Qt5 Multimedia + GStreamer + V4L2。不同 Linux 发行版包名可能略有差异，下面以 Ubuntu/Debian 系为例。

### 必需依赖包

| 类型 | 依赖包 |
| --- | --- |
| C/C++ 构建工具 | `build-essential`、`cmake`、`pkg-config` |
| Qt5 开发包 | `qtbase5-dev`、`qtbase5-dev-tools`、`qttools5-dev-tools`、`qtmultimedia5-dev` |
| Qt5 运行插件 | `libqt5multimedia5`、`libqt5multimediawidgets5`、`libqt5multimedia5-plugins`、`libqt5multimediagsttools5` |
| GStreamer 开发包 | `libgstreamer1.0-dev`、`libgstreamer-plugins-base1.0-dev`、`libglib2.0-dev` |
| GStreamer 运行插件 | `gstreamer1.0-tools`、`gstreamer1.0-plugins-base`、`gstreamer1.0-plugins-good`、`gstreamer1.0-plugins-bad`、`gstreamer1.0-x` |
| V4L2 工具/运行库 | `v4l-utils`、`libv4l-0` |
| 中文字体 | `fonts-noto-cjk`、`fonts-wqy-microhei`、`fonts-wqy-zenhei` |

一条命令安装：

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config \
  qtbase5-dev qtbase5-dev-tools qttools5-dev-tools qtmultimedia5-dev \
  libqt5multimedia5 libqt5multimediawidgets5 libqt5multimedia5-plugins \
  libqt5multimediagsttools5 \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libglib2.0-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-x \
  v4l-utils libv4l-0 \
  fonts-noto-cjk fonts-wqy-microhei fonts-wqy-zenhei
```

### 依赖说明

- `Qt5::Widgets`：主界面、按钮、日志窗口和参数控件。
- `Qt5::Multimedia` / `Qt5::MultimediaWidgets`：保留的单帧抓取路径。
- `GStreamer`：Linux 实时预览、双 FPS 统计、raw YUYV appsink 数据链路。
- `gstreamer1.0-x`：提供 `ximagesink` / `xvimagesink` 等 X11 预览 sink。
- `gstreamer1.0-plugins-bad`：部分平台上的 `fpsdisplaysink` / GL sink 等插件可能来自该包。
- `v4l-utils`：用于检查 `/dev/video*` 和摄像头支持的 V4L2 模式。
- 中文字体包：避免中文按钮、日志和标签显示成方块。

## 快速开始

### 1. 获取代码

```bash
git clone https://github.com/jingzezhang818/Camera_PC.git
cd Camera_PC
git checkout linux-support
```

### 2. 编译

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

### 3. 加载 XDMA 驱动并运行 GUI

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
# 摄像头节点
ls /dev/video*
v4l2-ctl --list-devices
v4l2-ctl --list-formats-ext -d /dev/video0

# GStreamer 插件
gst-inspect-1.0 v4l2src appsink videoconvert fpsdisplaysink \
  ximagesink xvimagesink waylandsink glimagesink autovideosink fakesink

# XDMA 节点
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
- 预览 sink 会依次尝试 `ximagesink`、`waylandsink`、`xvimagesink`、`glimagesink`、`autovideosink`；如果显示 sink 都无法进入 `PLAYING`，最后会用 `fakesink` 保住 raw appsink/XDMA 链路，并在日志中提示预览画面为空。

`采一帧` 仍暂时保留 Qt `CameraProbe` 路径，和实时预览/实时发送链路解耦。

如果龙芯机器出现 `failed to set GStreamer pipeline to PLAYING`，先看程序日志里的 `[GST][WARN] sink=...` 和 `[GST][ERROR] source=...`。也可以用下面命令单独验证插件和摄像头链路，分辨是显示 sink 失败还是 V4L2/caps 协商失败：

```bash
gst-inspect-1.0 v4l2src appsink videoconvert ximagesink xvimagesink autovideosink fakesink

# 把 /dev/video0、分辨率和帧率替换成 UI 日志里的 accepted mode
gst-launch-1.0 -v v4l2src device=/dev/video0 ! \
  "video/x-raw,format=YUY2,width=640,height=480,framerate=30/1" ! \
  videoconvert ! ximagesink
```
