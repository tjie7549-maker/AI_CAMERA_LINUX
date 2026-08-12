# ROCK 2A RTSP 接收与千问视觉识别

本工程运行在 ROCK 2A，接收 RV1106 的 H.264 子码流和 NPU track 元数据，维护
对象事件、选择最佳帧、按事件策略调用千问，并用 SQLite/文件系统持久化。

```text
RV1106 /live/1
  -> H.264 RTSP over TCP
  -> libavformat + libavcodec
  -> yuvj420p AVFrame
  -> libswscale RGB24
  -> bounded JPEG ring -> event best image
  -> event engine -> SQLite -> Qwen Vision API
  -> event/recognition NDJSON -> TCP 192.168.50.1:9000
  -> RV1106 Qt UI
```

## 当前网络

```text
RV1106 网线地址：192.168.50.2
ROCK 2A 网线地址：192.168.50.1
子码流地址：rtsp://192.168.50.2:554/live/1
子码流参数：H.264，640x360，20 FPS
```

运行 ROCK 2A 接收端前，RV1106 必须已启动 LCD 与 RTSP 程序。

## 目录结构

```text
rock2a_receiver/
├── src/                    C++ RTSP 接收、解码与 JPEG 写入
├── include/                C++ 头文件
├── tools/qwen_vision/      千问视觉 Python 模块
├── tests/                  无硬件事件/数据库/HTTP 测试
├── config/                 supervisor 配置示例
├── build/                  CMake 编译产物
├── artifacts/frames/       历史和后续测试图片
├── runtime/ai_cam/         最新图片、识别结果与运行日志
│   ├── events.db           SQLite 事件库
│   ├── frame_ring/         最近 24 张 RTSP 候选图
│   ├── event_best/         每个事件的稳定最佳图
│   └── saved_results/events/  用户保存的事件
├── runtime/result_cache/   按 auto/manual 缓存的精确识别输入与结果
├── runtime/saved_results/  用户确认保存的图片、JSON、元数据与索引
├── run_ai_monitor.sh       一键接收与识别脚本
├── run_ai_pipeline.sh      无 Qt 的 headless 自动识别测试入口
├── run_manual_ai_pipeline.sh  Qt 自动/手动交互识别入口
├── run_linked_ai_camera.sh 双板联动入口
└── .venv-qwen/             独立 Python 虚拟环境
```

`artifacts/` 与 `runtime/ai_cam/` 均在 `.gitignore` 中，不应提交图片、识别结果、日志或 API 配置。

## 首次准备

安装 C++ 构建和 FFmpeg 开发依赖：

```sh
sudo apt update
sudo apt install -y build-essential cmake pkg-config \
  ffmpeg libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
  python3-venv
```

编译接收程序：

```sh
cd /home/radxa/AI_CAMERA_LINUX/rock2a_receiver
mkdir -p build
cd build
cmake ..
cmake --build . -j"$(nproc)"
```

创建千问 Python 环境：

```sh
cd /home/radxa/AI_CAMERA_LINUX/rock2a_receiver
python3 -m venv .venv-qwen
. .venv-qwen/bin/activate
python -m pip install --upgrade pip
pip install -r tools/qwen_vision/requirements.txt
```

千问环境变量文件必须位于：

```text
/home/radxa/.config/ai_cam/qwen.env
```

其中配置 `DASHSCOPE_API_KEY`、`DASHSCOPE_BASE_URL` 和 `QWEN_MODEL`。该文件不进入工程目录，也不会被脚本打印。

## 一键运行

推荐使用双板联动入口。它通过 ROCK 2A 到 RV1106 的专用 SSH 密钥启动远端摄像头、RTSP
和 Qt，再启动本地 AI 管线；按一次 Ctrl+C 会先停止 ROCK 2A，再远程正常停止 RV1106：

```sh
cd /home/radxa/AI_CAMERA_LINUX/rock2a_receiver
./run_linked_ai_camera.sh
```

默认 `event` 模式启动 RTSP receiver、事件服务、NPU receiver、HTTP 9001 和 TCP
sender。自动云端识别由事件状态变化驱动，Qt 固定 30 秒周期默认关闭；暂停帧手动
识别保持不变。不要同时启动 headless watcher 或 Qt 兼容周期，以免重复调用。

该脚本不存储 RV1106 登录密码。首次部署后，ROCK 2A 的 `~/.ssh/config` 中需要存在
`rv1106-ai-camera` 主机别名。

也可以分别启动。此时必须先启动 RV1106 并等待 RTSP 就绪，再启动 ROCK 2A：

在 RV1106 已经运行推流的前提下，ROCK 2A 执行：

```sh
cd /home/radxa/AI_CAMERA_LINUX/rock2a_receiver
AI_CAMERA_MODE=event ./run_rock2a_supervisor.sh
```

该脚本持续运行到 Ctrl+C，并完成：

1. 接收 `rtsp://192.168.50.2:554/live/1` 并维护 500 ms/24 张候选环。
2. 在 `192.168.50.1:9010` 接收 bbox/track 元数据。
3. 创建和结束事件，SQLite 落盘，并按 3/120/300 秒策略调用云端。
4. `192.168.50.1:9001` 保留冻结帧手动识别。
5. `192.168.50.1:9000` 回传事件和识别结果，`9011` 提供 API/metrics。
6. Ctrl+C 时由 supervisor 正常停止全部组件。

统一 track 状态文件为 `runtime/ai_cam/npu_latest.json`，其中保留 schema、bbox、
track 和时间戳，并补充旧消费者需要的值守字段；Qt 使用低频
`runtime/ai_cam/npu_display.json`。NPU 原始帧不上传云端；人员离开并达到 RV1106
无人超时后，Qt 收到 idle 状态并停止新的自动请求。
云端或手动结果发送后具有 10 秒显示优先期，期间 NPU 展示更新不会覆盖结果。

无 Qt 的 headless 自动识别专项测试才使用：

```sh
./run_ai_pipeline.sh
```

该入口由 `watch_latest_image.py` 读取 `latest.jpg`，不应与智能终端交互入口同时运行。

持续运行到 Ctrl+C：

```sh
./run_ai_monitor.sh --duration 0
```

`run_ai_monitor.sh` 保留为底层 headless 测试脚本，适合需要自定义测试时长的场景。

覆盖 RTSP 地址：

```sh
./run_ai_monitor.sh --url rtsp://192.168.50.2:554/live/1
```

按 Ctrl+C 时，脚本会向子进程发送正常 `SIGTERM`，不使用强制结束。

## 查看结果

```text
最新图片：
/home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/ai_cam/latest.jpg

最新识别结果：
/home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/ai_cam/latest_result.json

最新事件 / 最新事件识别：
/home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/ai_cam/event_latest.json
/home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/ai_cam/event_recognition_latest.json

事件数据库 / 最佳图：
/home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/ai_cam/events.db
/home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/ai_cam/event_best/

C++ 接收日志：
/home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/ai_cam/receiver.log

千问监控日志：
/home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/ai_cam/qwen_watch.log

TCP 结果服务日志：
/home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/ai_cam/result_tcp.log
```

格式化查看最新识别结果：

```sh
python3 -m json.tool \
  /home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/ai_cam/latest_result.json
```

## 交互识别与保存结果服务

兼容脚本 `run_manual_ai_pipeline.sh` 仍提供旧手动链路，并默认绑定
`192.168.50.1:9001`：

```text
POST /recognize    接收 RV1106 实时帧或暂停帧 JPEG 并返回识别 JSON
POST /save-result  根据 source 与 request_id 保存已有识别结果
GET  /health       服务健康检查
```

每次自动或手动识别都会原子写入以下缓存文件：

```text
runtime/result_cache/<auto|manual>/<request_id>/image.jpg
runtime/result_cache/<auto|manual>/<request_id>/result.json
runtime/result_cache/<auto|manual>/<request_id>/metadata.json
```

Qt 点击“保存结果”后，服务会复制为：

```text
runtime/saved_results/<auto|manual>/<YYYY-MM-DD>/<HHMMSS_request_id>/
```

其中包含精确上传图片、完整识别 JSON 和元数据；`index.jsonl` 使用文件锁维护幂等索引，
同一来源和请求 ID 不会重复保存。缓存最多保留 100 条，并清理 24 小时前的记录。

Qt 通过 `X-Recognition-Source: auto|manual` 区分旧来源。生产事件模式下只保留
冻结帧 `manual` 请求；设置 `ENABLE_LEGACY_AUTO_RECOGNITION=1` 才恢复旧 `auto`
周期。当前事件保存到 `runtime/ai_cam/saved_results/events/<event_id>/`。

## 事件服务

`event_service.py` 默认监听 `192.168.50.1:9011`，提供 `/health`、`/events`、
`/events/<id>`、`/events/<id>/image`、`/events/<id>/save` 和 `/metrics`。消息体
限制 64 KiB，ID 严格校验；最佳帧与 NPU 帧仅按时间近似匹配并明确标记
`frame_match=approximate`。完整接口见 `../docs/event-api.md`。

配置模板位于 `config/ai-camera.example.env`。部署时可复制为
`/etc/ai-camera.env`，systemd 和手动启动的 supervisor 都会读取同一份配置。
也可以通过 `AI_CAMERA_CONFIG_FILE` 指定其他绝对路径。推荐把模式文件写为：

```sh
echo event > /home/radxa/.config/ai_cam/mode
```

## 手动运行

仅验证 RTSP 解码与 JPEG：

```sh
cd /home/radxa/AI_CAMERA_LINUX/rock2a_receiver
./build/rock2a_rtsp_receiver \
  --url rtsp://192.168.50.2:554/live/1 \
  --output artifacts/frames/manual \
  --interval-ms 1000 \
  --duration 30
```

启用 `latest.jpg`，供 Python 或后续 Qt 读取：

```sh
./build/rock2a_rtsp_receiver \
  --url rtsp://192.168.50.2:554/live/1 \
  --output artifacts/frames/manual \
  --interval-ms 5000 \
  --latest-image runtime/ai_cam/latest.jpg \
  --latest-interval-ms 3000 \
  --duration 300
```

`latest.jpg` 先写入同目录临时文件，`flush`、`close` 后通过 `rename` 原子替换；`latest_result.json` 也以临时文件、`fsync`、`rename` 的方式更新。因此读取程序不会看到半张 JPEG 或半个 JSON。

## 运行行为

- C++ 接收端只负责 RTSP、H.264 解码、RGB24 转换和 JPEG 写入。
- 千问调用在独立 Python 进程中执行，不会阻塞 FFmpeg 解码线程。
- RTSP 候选环严格限制为 24 张，事件候选缓存限制为 32 张；最佳图另行提升保存。
- 事件云端请求单事件串行、有限重试，云端失败不影响事件结束。
- API 失败仅写入失败结果，不会停止视频接收。
- 当前版本不自动重连 RTSP；服务恢复后重新启动接收程序即可。RTSP 建连失败会停止
  Python 监控并在终端显示接收日志，不会停留在等待识别结果的状态。

## 历史媒体链路验证结果

```text
RTSP 十分钟测试：600 秒，12027 帧，约 20 FPS，读帧/解码错误均为 0。
VO 缓冲告警：0 次。
千问五分钟测试：51 次请求，51 次成功，JSON 解析失败 0 次。
```

600 秒 JPEG 测试传入 `--interval-ms 1000`，生成 585 张，平均约 `1.026 秒/张`。抽帧仅在新解码帧到达时检查单调时钟，因此 1000 ms 是最小间隔，不是参数解析错误。
