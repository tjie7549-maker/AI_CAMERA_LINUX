# RV1106 SC3336 智能视觉终端

这是一个 RV1106 + SC3336 端云协同项目：RV1106 负责相机采集、触屏交互、本地人形检测和参数调试；ROCK 2A 可选地负责事件聚合与 Qwen 视觉识别。

```text
SC3336 → RKAIQ / VI / VPSS
             ├─→ DMA-BUF 共享预览 → Qt 小屏 + RKNN 人形检测
             └─→ H.264 RTSP → ROCK 2A 事件引擎 → Qwen Vision → Qt 回显
                    ↑
        camera-daemon：相机所有权、rkipc 切换、AE/AGC/V4L2 调参、看门狗
```

## 从哪里开始读

1. 运行入口：`rv1106-smart-camera/scripts/run_demo.sh`。
2. 相机控制：`rv1106-sender/camera-daemon/src/camera_daemon.cpp`。
3. 图像数据：`rv1106-sender/media-sender/src/ai_cam_app.c`、`ai_cam_preview.c`。
4. 本地检测：`rv1106-sender/media-sender/src/npu_detect.c`。
5. 云端事件：`rock2a-receiver/tools/qwen_vision/event_engine.py`。

## 目录

```text
rv1106-smart-camera/
├── rv1106-sender/       RV1106 发送端：相机、daemon、Qt、本地 NPU 与 RTSP
├── rock2a-receiver/     ROCK 2A 接收端：RTSP、事件、存储、Qwen 与结果回传
├── configs/             板端路径、阈值和网络配置
├── scripts/             构建、部署、运行、自启动和诊断入口
├── docs/                架构、接口、复现、测试与交接资料
└── kernel-patches/      可审查、可回退的 SC3336 驱动补丁
```

## 最短启动路径

```sh
cd rv1106-smart-camera
./scripts/build_app.sh
./scripts/deploy_app.sh --ip 172.32.0.93
# 板端：sh /userdata/rv1106-smart-camera/scripts/run_demo.sh
```

完整构建、调参、自启动和数据流见 [项目 README](rv1106-smart-camera/README.md)。
