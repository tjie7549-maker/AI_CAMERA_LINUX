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

## 网络

RV1106 与 ROCK 2A 通过网线直连，使用 `192.168.50.0/24`：

```text
RV1106：192.168.50.2
ROCK 2A：192.168.50.1
```

ROCK 2A 使用 Wi-Fi 保持外网访问，用于安装依赖和访问千问 API。
