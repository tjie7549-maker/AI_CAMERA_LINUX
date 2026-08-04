# RV1106 智能视觉终端界面

该工程为 Luckfox Pico Ultra（RV1106）的 720x720 Qt Widgets 智能视觉终端。
程序通过 TCP 从 ROCK 2A 接收一行一个 JSON 的 AI 识别结果，并更新场景、
人数、物体、告警、摘要、Token 和延迟信息。

## 当前限制

- 静态界面和 AI 中文结果均使用 Droid Sans Fallback 显示。
- 字体来自 Ubuntu 的 `fonts-droid-fallback`（Apache-2.0），部署脚本从系统字体
  目录复制到板端，不把字体文件存入本工程或 Git。
- 视频区域通过本地 DMA-BUF 接收 384x216 RGB888 摄像头预览，再按 16:9 等比例绘制到界面。
- Qt 与 RKMPI VO 不能同时占用 LCD。发送端必须以 `--no-vo --preview-shm /ai_cam_preview` 启动，由 Qt 独占 DRM/LCD。

## 编译

```sh
cd /home/summary/linux/rv1106_ai_ui
./scripts/build.sh
```

## 部署

```sh
./scripts/deploy.sh
```

部署脚本只上传可执行程序、`run.sh` 和本说明，不上传 SDK、字体或密钥。

## 板端运行

```sh
cd /root/userdata/rv1106_ai_ui
./run.sh
```

默认连接 `192.168.50.1:9000`，可使用环境变量修改：

```sh
SERVER_IP=192.168.50.1 SERVER_PORT=9000 ./run.sh
```

直接运行程序时支持：

```sh
./rv1106_ai_ui --server-ip 192.168.50.1 --server-port 9000 \
  --preview-shm /ai_cam_preview --preview-timeout-ms 1000
./rv1106_ai_ui --help
```

按 `Ctrl+C` 正常退出。

## 实时预览启动顺序

先在另一个终端启动发送端：

```sh
cd /root/userdata
./run_ai_headless_preview.sh --output /dev/null
```

再启动 Qt：

```sh
cd /root/userdata/rv1106_ai_ui
./run.sh
```

发送端通过 `/tmp/ai_cam_preview.sock` 发送两块 CMA DMA-BUF 的文件描述符；`/ai_cam_preview` 只保存帧元数据。Qt 在读取完整帧后复制到 QImage 绘制，不做 CPU 颜色空间转换。启动成功的日志包含 `#Preview: received 2 DMA-BUF frames` 和 `#Preview: state=online`。

## TCP 消息

服务端每次发送一个 JSON 对象并以换行符结束。接收缓冲上限为 64 KiB，
非法 JSON 只更新错误状态并保留上一次有效结果，TCP 断开后每 3 秒重连。
有效结果 15 秒内显示 `AI ONLINE`，15 至 20 秒显示 `AI STALE`，超过
20 秒显示 `AI OFFLINE`。
