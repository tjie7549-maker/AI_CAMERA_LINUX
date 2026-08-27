# RV1106 SC3336 智能视觉终端

本仓库只有一个项目：**RV1106 SC3336 智能识别与摄像头驱动调参终端**。它包含板端
采集、NPU 检测、Qt 触屏展示/参数调节，以及可选的 ROCK 2A 事件识别与千问视觉服务。
不包含人脸考勤项目或其 UI、后端和脚本。

```text
SC3336 -> RKAIQ/VI/VPSS -> Qt 实时预览 + RKNN 人形检测
                     \-> H.264 RTSP -> ROCK 2A 事件处理 -> 千问视觉识别
                         ^
          camera-daemon：手动 AE/AGC、曝光、增益、VBLANK、翻转与恢复自动快照
```

## 目录

```text
rv1106-smart-camera/
├── app/media-sender/    RV1106 RKMPI/RKAIQ 采集、RTSP 与本地 NPU 检测
├── app/camera-daemon/   相机控制仲裁与 SC3336 调试状态
├── app/qt-console/      RV1106 触屏智能展示与参数调节界面
├── rock2a-receiver/     可选的 RTSP 接收、事件引擎与千问视觉识别
├── kernel-patches/      可审查、可回退的 SC3336 驱动增强补丁
├── scripts/             构建、部署、启动和板端验证脚本
└── docs/                架构、测试、调参和交接文档
```

## 快速开始

```sh
cd rv1106-smart-camera
./scripts/build_app.sh
./scripts/deploy_app.sh --ip 172.32.0.93
```

板端启动：

```sh
sh /userdata/rv1106-smart-camera/scripts/run_demo.sh
```

构建和部署说明见 [项目 README](rv1106-smart-camera/README.md)；ROCK 2A 端的识别服务
说明见 [rock2a-receiver README](rv1106-smart-camera/rock2a-receiver/README.md)。
