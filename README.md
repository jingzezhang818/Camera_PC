# camera_pc_linux_gui

基于 Qt Widgets 的 Linux 上位机程序，功能对齐 `D:\Program\Qt Programs\camera_PC`。

## 项目结构

- `include/`
  - `xdmaDLL_public_linux.h`：Linux 下 XDMA 接口声明与兼容类型
- `src/`
  - `XDMA_MoreB_linux.cc`：XDMA Linux 实现（`/dev/xdma*`）
  - `main.cpp`：Qt 程序入口
  - `widget.h/.cpp/.ui`：主界面与业务流程
  - `cameraprobe.h/.cpp`：相机模式枚举与单帧抓取
  - `video_packet_batcher.h/.cpp`：1024B 协议封包与 1MiB 批量聚合

说明：已移除旧的 CLI 与重复模块，只保留 GUI 主线代码。

## 依赖

Ubuntu/WSL 示例：

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config \
  qtbase5-dev qtmultimedia5-dev qttools5-dev-tools \
  libqt5multimedia5-plugins libqt5multimediawidgets5 \
  fonts-noto-cjk fonts-wqy-microhei fonts-wqy-zenhei
```

## 编译

```bash
cmake -S . -B build
cmake --build build -j
```

## 运行

```bash
./build/camera_pc_gui
```

## 备注

ls /dev/video*
v4l2-ctl --list-devices


- `ready_state`、`reset` 的返回语义直接透传底层 XDMA 接口。
- TODO：`op_state`、`ddr_state` 的寄存器语义需以 FPGA 文档为准。
