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

Qt 与 RKMPI VO 不能同时占用 LCD。运行 `rv1106_ai_ui` 前必须正常停止发送端，
退出 Qt 后再执行 `./run_ai_sender.sh` 恢复摄像头显示和 RTSP。

日常启动时，RV1106 在 `/root/userdata` 执行 `./run_ai_sender.sh`；ROCK 2A 在
`/home/radxa/AI_CAMERA_LINUX/rock2a_receiver` 执行 `./run_ai_pipeline.sh`。

## 仓库规则

- 两端各自独立构建，不共享二进制产物。
- Qt 构建产物和中文字体文件不提交到 Git；部署脚本从 Ubuntu 系统字体目录复制字体。
- API Key 仅存放在 ROCK 2A 的 `~/.config/ai_cam/qwen.env`，不提交到 Git。
- 测试图片、识别 JSON、日志、虚拟环境和编译产物均被忽略。
