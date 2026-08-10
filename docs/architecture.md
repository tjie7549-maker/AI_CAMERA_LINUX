# 系统架构

## RV1106 发送端

```text
SC3336 -> ISP/RKAIQ -> VI(2304x1296) -> VPSS
  |- ch0: 中心裁剪 1296x1296 -> 720x720 -> VO/LCD（独立 VO 模式）
  |- ch1: 1280x720 NV12 -> H.264 -> RTSP /live/0
  |- ch2: 640x360 NV12 -> H.264 -> RTSP /live/1
  `- ch3: 384x216 -> RGA RGB888 -> DMA-BUF -> Qt / NPU
```

LCD 分支通过 VPSS 中心裁剪铺满方形屏幕，不拉伸人物。发送端负责 ISP、采集、显示、编码和 RTSP；不承担 AI 请求。

## ROCK 2A 接收端

```text
RTSP /live/1 -> FFmpeg H.264 解码 -> yuvj420p AVFrame
             -> libswscale RGB24 -> JPEG / latest.jpg
             -> 独立 Python 进程 -> Qwen Vision -> latest_result.json
```

交互终端由 Qt 在 NPU 值守 active 且实时预览时，每 30 秒向 HTTP 9001 提交
一张当前帧；点击暂停后停止周期，后续只提交用户选择的冻结帧。NPU 原始输入
不上传云端，只通过 TCP 9010 发送值守状态。独立 headless watcher 仍保留用于
专项测试，但不得与交互终端周期同时启用。请求不会阻塞 C++ 解码线程；失败
不停止接收端。

ROCK 2A 可通过独立工具监控 `latest_result.json` 并监听 `0.0.0.0:9000`。
RV1106 客户端连接后，服务端将更新结果作为一行一个 JSON 的 TCP 消息发送：

```text
latest_result.json -> send_result_tcp.py :9000 -> RV1106 AiResultClient
```

服务端会过滤 API Key 等敏感字段；客户端断开后继续监听，不修改原结果文件。
云端/手动主结果发送后具有 10 秒显示优先期，NPU 本地展示结果在此期间暂缓发送。

## RV1106 Qt 智能界面

```text
ROCK 2A TCP :9000 -> QTcpSocket -> NDJSON 解析 -> 720x720 Qt Widgets LCD
```

Qt 界面显示 DMA-BUF 实时摄像头预览、AI 在线状态、场景、人数、物体、告警、摘要、
API 延迟和 Token。TCP 断开后每 3 秒自动重连；15 秒无有效结果显示超时，
20 秒后显示离线。接收缓冲限制为 64 KiB，非法 JSON 不覆盖上一次有效结果。

界面使用应用目录中的 Droid Sans Fallback 显示中文。字体由部署脚本从 Ubuntu
的 `fonts-droid-fallback` 包复制，不存入仓库。

当前 Qt 通过 `linuxfb + QT_QPA_FB_DRM=1` 独占 LCD，因此不能与发送端 VO 同时
运行。智能视觉模式使用 `--no-vo`，由发送端通过 CMA DMA-BUF 向 Qt 提供实时画面。

## RV1106 端侧 NPU

```text
DMA-BUF RGB888 -> npu_detect -> yolov5n-320 INT8
                |- 连续 3 次有人 -> 点亮 LCD 背光
                |- 连续 30 秒无人 -> 关闭 LCD 背光
                `- TCP 9010 -> ROCK 2A npu_result_server.py
                   |- npu_latest.json -> 本地值守状态
                   `- npu_display.json -> TCP 9000 -> Qt
```

端侧检测与云端识别可并存。`AI_BACKEND=cloud` 支持 Qt 的自动/手动千问识别；
`AI_BACKEND=local` 返回最近一次端侧 NPU 检测，并按 manual 结果保存。
摄像头、Qt 和 NPU 在背光休眠期间保持运行，唤醒不需要重新初始化 ISP/VI/VPSS。

## 网络

RV1106 与 ROCK 2A 通过网线直连，使用 `192.168.50.0/24`：

```text
RV1106：192.168.50.2
ROCK 2A：192.168.50.1
```

ROCK 2A 使用 Wi-Fi 保持外网访问，用于安装依赖和访问千问 API。
