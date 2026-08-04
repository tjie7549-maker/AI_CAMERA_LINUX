# 系统架构

## RV1106 发送端

```text
SC3336 -> ISP/RKAIQ -> VI(2304x1296) -> VPSS
  |- ch0: 中心裁剪 1296x1296 -> 720x720 -> VO/LCD
  |- ch1: 1280x720 NV12 -> H.264 -> RTSP /live/0
  `- ch2: 640x360 NV12 -> H.264 -> RTSP /live/1
```

LCD 分支通过 VPSS 中心裁剪铺满方形屏幕，不拉伸人物。发送端负责 ISP、采集、显示、编码和 RTSP；不承担 AI 请求。

## ROCK 2A 接收端

```text
RTSP /live/1 -> FFmpeg H.264 解码 -> yuvj420p AVFrame
             -> libswscale RGB24 -> JPEG / latest.jpg
             -> 独立 Python 进程 -> Qwen Vision -> latest_result.json
```

Python 监控只处理最新图片，最短请求间隔为 5 秒。API 请求不会阻塞 C++ 解码线程；失败只写入结果文件，不停止接收端。

ROCK 2A 可通过独立工具监控 `latest_result.json` 并监听 `0.0.0.0:9000`。
RV1106 客户端连接后，服务端将更新结果作为一行一个 JSON 的 TCP 消息发送：

```text
latest_result.json -> send_result_tcp.py :9000 -> RV1106 AiResultClient
```

服务端会过滤 API Key 等敏感字段；客户端断开后继续监听，不修改原结果文件。

## RV1106 Qt 智能界面

```text
ROCK 2A TCP :9000 -> QTcpSocket -> NDJSON 解析 -> 720x720 Qt Widgets LCD
```

Qt 界面显示摄像头占位区域、AI 在线状态、场景、人数、物体、告警、摘要、
API 延迟和 Token。TCP 断开后每 3 秒自动重连；15 秒无有效结果显示超时，
20 秒后显示离线。接收缓冲限制为 64 KiB，非法 JSON 不覆盖上一次有效结果。

界面使用应用目录中的 Droid Sans Fallback 显示中文。字体由部署脚本从 Ubuntu
的 `fonts-droid-fallback` 包复制，不存入仓库。

当前 Qt 通过 `linuxfb + QT_QPA_FB_DRM=1` 独占 LCD，因此不能与发送端 VO 同时
运行。本阶段摄像头画面尚未接入 Qt，切换模式时必须先正常退出当前 LCD 使用者。

## 网络

RV1106 与 ROCK 2A 通过网线直连，使用 `192.168.50.0/24`：

```text
RV1106：192.168.50.2
ROCK 2A：192.168.50.1
```

ROCK 2A 使用 Wi-Fi 保持外网访问，用于安装依赖和访问千问 API。
