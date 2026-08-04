# AI Camera Linux

RV1106 摄像头发送端与 ROCK 2A 接收、视觉识别端的单仓库工程。

```text
SC3336 -> RV1106 ISP/VI/VPSS/VENC -> H.264 RTSP
                                      |
                                      `-> ROCK 2A FFmpeg -> RGB24/JPEG -> Qwen Vision
                                                                 |
                                          RV1106 Qt UI <- TCP JSON
```

## 目录

```text
rv1106_sender/     Luckfox Pico RV1106：LCD 预览、H.264 双码流与 RTSP 服务
rock2a_receiver/   ROCK 2A：RTSP 解码、JPEG 抽帧、latest.jpg 与千问视觉识别
rv1106_ai_ui/      RV1106：720x720 Qt 中文界面与 AI JSON TCP 客户端
docs/               架构和联调说明
```

## 当前网络

```text
RV1106：192.168.50.2
ROCK 2A：192.168.50.1
子码流：rtsp://192.168.50.2:554/live/1
```

## 构建与运行

RV1106 发送端使用 Luckfox SDK 交叉编译：

```sh
cd rv1106_sender
make
```

ROCK 2A 接收与识别端的构建、配置和一键运行方式见 `rock2a_receiver/README.md`。
系统架构与两端职责见 [docs/architecture.md](docs/architecture.md)。

RV1106 Qt 界面使用 SDK 的交叉 qmake 编译：

```sh
cd rv1106_ai_ui
./scripts/build.sh
./scripts/deploy.sh
```

Qt 与 RKMPI VO 不能同时占用 LCD。智能视觉模式中使用 no-VO 发送端，由 Qt 独占 LCD；发送端通过 CMA DMA-BUF 把实时预览帧传给 Qt。

日常启动只需两个终端：

```sh
# ROCK 2A：RTSP 接收、千问识别、TCP 结果回传
cd /home/radxa/AI_CAMERA_LINUX/rock2a_receiver
./run_ai_pipeline.sh

# RV1106：摄像头、RTSP、Qt LCD
cd /root/userdata/ai_camera
./run_ai_terminal.sh
```

两边均按 Ctrl+C 正常停止。ROCK 2A 脚本自动监听 `0.0.0.0:9000`，Qt 默认连接 `192.168.50.1:9000`。

## 仓库规则

- 两端各自独立构建，不共享二进制产物。
- Qt 构建产物和中文字体文件不提交到 Git；部署脚本从 Ubuntu 系统字体目录复制字体。
- API Key 仅存放在 ROCK 2A 的 `~/.config/ai_cam/qwen.env`，不提交到 Git。
- 测试图片、识别 JSON、日志、虚拟环境和编译产物均被忽略。
