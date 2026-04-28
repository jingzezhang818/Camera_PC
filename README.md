# Camera PC Linux GUI

基于 Qt Widgets 的 Linux 上位机程序，功能对齐 Windows 版本。

## 项目结构

```
camera_PC_linux_cli/
├── CMakeLists.txt          # CMake 构建配置
├── README.md               # 本文件
├── include/
│   ├── cameraprobe.h       # 相机模式枚举与单帧抓取
│   ├── video_packet_batcher.h  # 1024B 协议封包与 1MiB 批量聚合
│   ├── widget.h            # 主界面声明
│   └── xdmaDLL_public_linux.h  # Linux XDMA 接口声明与兼容类型
├── src/
│   ├── XDMA_MoreB_linux.cc # XDMA Linux 实现（/dev/xdma*）
│   ├── cameraprobe.cpp     # 相机模式枚举与单帧抓取实现
│   ├── main.cpp            # Qt 程序入口
│   ├── video_packet_batcher.cpp  # 协议封包与批量聚合实现
│   ├── widget.cpp          # 主界面与业务流程实现
│   └── widget.ui           # Qt UI 描述文件
└── build/                  # 编译输出目录（不提交）
```

## 从克隆到运行

### 1. 克隆代码

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
  fonts-noto-cjk fonts-wqy-microhei fonts-wqy-zenhei
```

### 3. 编译

```bash
cmake -S . -B build
cmake --build build -j
```

### 4. 运行

```bash
./build/camera_pc_gui
```

## 设备检查

```bash
ls /dev/video*
v4l2-ctl --list-devices
```

## 备注

- `ready_state`、`reset` 的返回语义直接透传底层 XDMA 接口
- TODO：`op_state`、`ddr_state` 的寄存器语义需以 FPGA 文档为准
