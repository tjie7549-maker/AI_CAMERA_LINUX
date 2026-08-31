# RV1106 发送端代码导览

`rv1106-sender/` 是 RV1106 设备侧代码：SC3336 采集、ISP、编码、共享预览、本地检测、Qt 触屏和相机控制均在此处。

```text
run_demo.sh
 ├─ camera-daemon：唯一控制仲裁者；停止 rkipc 后拉起媒体和 NPU
 │   ├─ media-sender：RKAIQ + VI + VPSS + VENC + RTSP + DMA-BUF 预览生产者
 │   └─ npu_detect：读取共享预览，运行 RKNN，向 ROCK 2A:9010 发检测 JSON
 └─ rv1106_ai_ui：读取共享预览；经 Unix Socket 向 daemon 请求调参
```

| 目录 | 入口 | 阅读重点 |
|---|---|---|
| `camera-daemon/` | `src/main.cpp` | `CameraDaemon::start/run/handle`：进程监管、Unix Socket 命令、AE/V4L2 控制 |
| `media-sender/` | `src/main.c` | `ai_cam_start()` 初始化 RKAIQ→VI→VPSS→VENC→RTSP；`ai_cam_preview.c` 发布 DMA-BUF |
| `qt-console/` | `src/main.cpp` | `PreviewShmReader` 显示预览；`DaemonClient` 请求调参；`CameraDebugDialog` 呈现调试 |
| `rtsp-preview-bridge/` | `rtsp_preview_bridge.c` | 兼容性桥接；默认原生展示链路不依赖它 |

预览通道：VPSS→RGA→`/ai_cam_preview` + `/tmp/ai_cam_preview.sock`→Qt/NPU。

控制通道：Qt→`camera-daemon.sock`→daemon→RKAIQ 控制 socket 或 `/dev/v4l-subdev2`。

边界：Qt 不直接访问传感器；只有 daemon 管理 `rkipc`；手动曝光只允许在调试模式且自动 AE 关闭后使用。
