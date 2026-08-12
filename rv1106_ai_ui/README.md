# RV1106 智能视觉终端界面

该工程为 Luckfox Pico Ultra（RV1106）的 720x720 Qt Widgets 智能视觉终端。
程序通过 TCP 从 ROCK 2A 接收一行一个 JSON 的对象事件和 AI 识别结果，显示
事件 ID/状态、当前与最大人数、track 数、持续时间、云端状态、最佳帧、告警、
摘要、Token 和延迟。生产自动识别由 ROCK 事件服务调度；Qt 只负责展示和交互。

## 当前限制

- 静态界面和 AI 中文结果均使用 Droid Sans Fallback 显示。
- 字体来自 Ubuntu 的 `fonts-droid-fallback`（Apache-2.0），部署脚本从系统字体
  目录复制到板端，不把字体文件存入本工程或 Git。
- 视频区域通过本地 DMA-BUF 接收 384x216 RGB888 摄像头预览，再按 16:9 等比例绘制到界面。
- Qt 与 RKMPI VO 不能同时占用 LCD。发送端必须以 `--no-vo --preview-shm /ai_cam_preview` 启动，由 Qt 独占 DRM/LCD。

## 编译

```sh
cd /home/summary/linux/ai_cam/rv1106_ai_ui
./scripts/build.sh
```

## 部署

```sh
./scripts/deploy.sh
```

部署脚本只上传可执行程序、`run.sh` 和本说明，不上传 SDK、字体或密钥。

## 板端运行

```sh
cd /root/userdata/ai_camera
./run_ai_terminal.sh
```

该总控脚本以 `--no-vo` 启动媒体发送端，并让 Qt 独占 LCD。不要在智能视觉模式
同时启动 VO 预览程序。

默认连接 `192.168.50.1:9000`，可使用环境变量修改：

```sh
SERVER_IP=192.168.50.1 SERVER_PORT=9000 ./run.sh
```

旧固定间隔识别默认关闭。只在回退测试中设置
`ENABLE_LEGACY_AUTO_RECOGNITION=1`；间隔仍可用
`AUTO_RECOGNITION_INTERVAL_MS` 修改。

直接运行程序时支持：

```sh
./rv1106_ai_ui --server-ip 192.168.50.1 --server-port 9000 \
  --preview-shm /ai_cam_preview --preview-timeout-ms 1000 \
  --event-api-url http://192.168.50.1:9011
# 仅兼容回退：追加 --enable-legacy-auto-recognition
./rv1106_ai_ui --help
```

按 `Ctrl+C` 正常退出。触摸界面也支持暂停、识别、保存结果与双击确认退出：退出
由 `run_ai_terminal.sh` 正常清理媒体资源；通过 ROCK 2A 联动脚本启动时，AI 服务
也会随之停止。

## 触摸操作

- 实时预览：事件区域随 `event.new/update/end` 更新，高频 track 不覆盖云端语义摘要。
- “暂停/继续”：暂停冻结 LCD 当前帧；后台事件引擎仍在 ROCK 2A 独立运行。
- “识别/重新识别”：暂停后只上传冻结帧至
  `http://192.168.50.1:9001/recognize`，不会与自动请求并发。
- “保存结果”：优先向 HTTP 9011 保存当前 event_id；无事件时回退到 HTTP 9001
  保存手动 request_id。图片和记录只写 ROCK 2A。
- “退出”：首次点击显示“确认退出”，3 秒内第二次点击才执行正常退出。

## 实时预览启动顺序

先在另一个终端启动发送端：

```sh
cd /root/userdata/ai_camera
./rv1106_sender/run_ai_headless_preview.sh --output /dev/null
```

再启动 Qt：

```sh
cd /root/userdata/ai_camera/rv1106_ai_ui
./run.sh
```

发送端通过 `/tmp/ai_cam_preview.sock` 发送两块 CMA DMA-BUF 的文件描述符；`/ai_cam_preview` 只保存帧元数据。Qt 在读取完整帧后复制到 QImage 绘制，不做 CPU 颜色空间转换。启动成功的日志包含 `#Preview: received 2 DMA-BUF frames` 和 `#Preview: state=online`。

## TCP 消息

服务端每次发送一个 JSON 对象并以换行符结束。接收缓冲上限为 64 KiB，
非法 JSON 只更新错误状态并保留上一次有效结果，TCP 断开后每 3 秒重连。
有效结果 15 秒内显示 `AI ONLINE`，15 至 20 秒显示 `AI STALE`，超过
20 秒显示 `AI OFFLINE`。

Qt 同时解析旧普通识别 JSON 和 schema v1 的 `event.new/update/end`、
`recognition.result`、`health`。未知字段忽略，未知 schema 拒绝。TCP 重连成功后
会查询 `http://192.168.50.1:9011/events?state=active&limit=1` 恢复当前事件；
`event.end` 后保留最后结果，不立即清空。
