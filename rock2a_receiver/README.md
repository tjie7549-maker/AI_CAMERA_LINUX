# ROCK 2A RTSP 接收与千问视觉识别

本工程运行在 ROCK 2A，接收 RV1106 的 H.264 子码流，解码为 RGB24，定时保存 JPEG，并以低频、独立的 Python 进程调用千问视觉模型。

```text
RV1106 /live/1
  -> H.264 RTSP over TCP
  -> libavformat + libavcodec
  -> yuvj420p AVFrame
  -> libswscale RGB24
  -> JPEG / latest.jpg
  -> Qwen Vision API
  -> latest_result.json
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
├── build/                  CMake 编译产物
├── artifacts/frames/       历史和后续测试图片
├── runtime/ai_cam/         最新图片、识别结果与运行日志
├── run_ai_monitor.sh       一键接收与识别脚本
├── run_ai_pipeline.sh      自动编译后持续启动识别的入口脚本
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

在 RV1106 已经运行推流的前提下，ROCK 2A 执行：

```sh
cd /home/radxa/AI_CAMERA_LINUX/rock2a_receiver
./run_ai_pipeline.sh
```

该脚本会在需要时配置 CMake，并始终重新编译接收端；随后持续运行，按 Ctrl+C 正常停止。它会：

1. 接收 `rtsp://192.168.50.2:554/live/1`。
2. 每 5 秒保存一张普通 JPEG 到 `artifacts/frames/ai_monitor/`。
3. 每 3 秒原子更新一次最新图片。
4. 每 5 秒最多调用一次千问 API。
5. 在当前终端显示最新的中文 JSON 识别结果。
6. C++ 接收结束后正常停止 Python 监控。

持续运行到 Ctrl+C：

```sh
./run_ai_monitor.sh --duration 0
```

`run_ai_monitor.sh` 保留为底层启动脚本，适合需要自定义测试时长的场景；`run_ai_pipeline.sh` 是日常一键入口。

覆盖 RTSP 地址：

```sh
./run_ai_monitor.sh --url rtsp://192.168.50.2:554/live/1
```

按 Ctrl+C 时，脚本会向 C++ 与 Python 子进程发送正常 `SIGINT`，不使用强制结束。

## 查看结果

```text
最新图片：
/home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/ai_cam/latest.jpg

最新识别结果：
/home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/ai_cam/latest_result.json

C++ 接收日志：
/home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/ai_cam/receiver.log

千问监控日志：
/home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/ai_cam/qwen_watch.log
```

格式化查看最新识别结果：

```sh
python3 -m json.tool \
  /home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/ai_cam/latest_result.json
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
- Python 只处理更新后的最新图片；旧图片可丢弃，不会积压请求。
- API 失败仅写入失败结果，不会停止视频接收。
- 当前版本不自动重连 RTSP；服务恢复后重新启动接收程序即可。

## 已验证结果

```text
RTSP 十分钟测试：600 秒，12027 帧，约 20 FPS，读帧/解码错误均为 0。
VO 缓冲告警：0 次。
千问五分钟测试：51 次请求，51 次成功，JSON 解析失败 0 次。
```

600 秒 JPEG 测试传入 `--interval-ms 1000`，生成 585 张，平均约 `1.026 秒/张`。抽帧仅在新解码帧到达时检查单调时钟，因此 1000 ms 是最小间隔，不是参数解析错误。
