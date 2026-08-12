# RV1106 发送端

本工程运行在 Luckfox Pico RV1106 上，完成摄像头采集、本地 LCD 低延迟显示和 H.264 编码输出。
该端负责本地 LCD 预览、H.264 双码流编码和 RTSP 服务；ROCK 2A 接收端位于仓库根目录的
`rock2a_receiver/`。

## 当前媒体管线

```text
ISP -> VI (2304x1296 NV12) -> VPSS
                              |- ch0: 中心裁剪为 1296x1296 -> 缩放到 720x720 -> LCD
                              |- ch1: 保持 16:9 -> 1280x720 NV12 -> H.264 VENC -> RTSP /live/0
                              |- ch2: 保持 16:9 -> 640x360 NV12 -> H.264 VENC -> RTSP /live/1
                              `- ch3: 384x216 NV12 -> RGA -> 两块 CMA DMA-BUF -> Qt 实时预览
```

LCD 分支使用中心裁剪，人物不会因 16:9 画面直接压缩到正方形屏幕而变形；两个编码分支均保留完整的 16:9 画面。在 Qt 实时预览模式中，VO 被显式关闭，由 Qt 独占 DRM/LCD，因此不会与 VO 争抢屏幕。

RTSP 地址：

```text
rtsp://192.168.50.2:554/live/0  主码流，1280x720，25 FPS
rtsp://192.168.50.2:554/live/1  子码流，640x360，20 FPS
```

## 工程结构

```text
include/ai_cam.h       共享配置、运行状态、模块接口
src/main.c             CLI 参数解析与信号处理
src/ai_cam_app.c       管线编排、帧转发、统计与统一退出
src/ai_cam_isp.c       ISP 生命周期
src/ai_cam_vi.c        VI 生命周期
src/ai_cam_vpss.c      LCD 裁剪分支与编码分支
src/ai_cam_vo.c        LCD/VO 生命周期
src/ai_cam_venc.c      H.264 编码、VPSS 绑定、码流写文件与 RTSP 发送
src/ai_cam_rtsp.c      RTSP 服务与 /live/0、/live/1 会话生命周期
src/ai_cam_preview.c   VPSS ch3 、RGA、CMA DMA-BUF 预览生产者
include/preview_shm_protocol.h  Qt 与发送端共享的预览协议
run_simple_isp_vi_to_lcd_rv1106.sh  板端运行脚本
run_ai_headless_preview.sh  Qt 预览的 no-VO 发送端脚本
README.md              工程说明
```

模块依次初始化 ISP、SYS、VI、VPSS、VENC、RTSP 与帧转发；随后启动 VO。这样 LCD 等待首次
vsync 时，编码与 RTSP 不会被阻塞。退出时先停止线程，再关闭 RTSP、解绑并回收媒体资源。

## 编译

SDK 默认位于仓库外的相邻路径 `../../luckfox-pico`：

```sh
make
```

使用其他 SDK 路径时：

```sh
make SDK_DIR=/path/to/luckfox-pico
```

编译产物：

```text
rv1106_sender/out/simple_vi_get_frame_send_vo_rv1106
rv1106_sender/out/run_simple_isp_vi_to_lcd_rv1106.sh
rv1106_sender/out/run_ai_sender.sh
rv1106_sender/out/run_ai_terminal.sh
```

## 板端运行

智能视觉终端使用统一部署目录 `/root/userdata/ai_camera/`，一键启动摄像头、RTSP 和 Qt LCD：

```sh
cd /root/userdata/ai_camera
./run_ai_terminal.sh
```

脚本先启动 no-VO 发送端，等待 DMA-BUF 预览套接字就绪后再启动 Qt。Ctrl+C 时先正常停止 Qt，再向发送端发送 `SIGINT`，不使用强制结束。日志统一位于 `/root/userdata/ai_camera/logs/`。
在 Ubuntu 上调试中可以使用统一部署脚本：

```sh
cd rv1106_sender
./deploy_ai_terminal.sh
```

## 兼容启动方式

AI 识别流程的发送端一键启动：

```sh
cd /root/userdata
./run_ai_sender.sh
```

默认启动 2304x1296 摄像头、180 度旋转的 720x720 LCD 和双路 RTSP。传入参数可覆盖默认值，例如
`./run_ai_sender.sh --rotation 0`。按 Ctrl+C 正常停止媒体管线。

底层运行脚本会停止 `rkipc`，使用默认的 ISP、LCD 与 RTSP 参数，并将本地 H.264 文件输出关闭：

```sh
cd /root/userdata
./run_simple_isp_vi_to_lcd_rv1106.sh
```

正常长期运行时用 Ctrl+C 退出。

默认参数为：IQ 文件目录 `/oem/usr/share/iqfiles`、摄像头 `2304x1296`、LCD
`720x720`、旋转 `180`、ISP/VO 均为 `0`、H.264 输出为 `/dev/null`。原有的旋转角度
传参仍可使用：

```sh
./run_simple_isp_vi_to_lcd_rv1106.sh 90
```

也可用命名参数覆盖默认值，例如保存主码流和修改 LCD 旋转：

```sh
./run_simple_isp_vi_to_lcd_rv1106.sh --rotation 90 --output /root/userdata/test.h264
```

## 150 帧 H.264 验证

使用 `-n 150` 编码 150 帧后自动退出，不需要 Ctrl+C：

```sh
cd /root/userdata
killall rkipc
sleep 4
LD_LIBRARY_PATH=/oem/usr/lib ./simple_vi_get_frame_send_vo_rv1106 \
  -a /oem/usr/share/iqfiles \
  -w 2304 -h 1296 -W 720 -H 720 -r 180 -I 0 -l 0 -d 0 \
  -o /root/userdata/test.h264 -n 150
```

电脑端拉取并检查码流：

```sh
scp root@192.168.50.2:/root/userdata/test.h264 /tmp/rv1106_test.h264
ffprobe -v error -count_frames \
  -show_entries stream=codec_name,profile,width,height,r_frame_rate,nb_read_frames \
  -of default=noprint_wrappers=1 /tmp/rv1106_test.h264
ffplay -f h264 -framerate 25 /tmp/rv1106_test.h264
```

已验证的预期结果为：H.264 High Profile、1280x720、25 FPS、150 帧。

## RTSP 验证

板端启动程序后检查监听端口：

```sh
netstat -lntp | grep 554 || ss -lntp | grep 554
```

电脑或 ROCK 2A 拉取主码流：

```sh
ffplay -fflags nobuffer -flags low_delay -framedrop \
  rtsp://192.168.50.2:554/live/0
```

子码流地址为 `rtsp://192.168.50.2:554/live/1`。

## Qt 实时预览模式

该模式用于让 Qt 界面显示实时摄像头画面，启动时必须关闭 VO：

```sh
cd /root/userdata
./run_ai_headless_preview.sh --output /dev/null
```

默认预览为 384x216 RGB888，目标 15 FPS。帧像素存在两块 RK MMZ/CMA DMA-BUF 中；`/ai_cam_preview` 仅保存帧序号和尺寸等元数据，不承载像素。两个 DMA-BUF 文件描述符通过 `/tmp/ai_cam_preview.sock` 发给 Qt 接收端。

已验证：150 帧冒烟测试中 RGA 无报错，实际预览约 14.5--15.5 FPS，两路 RTSP 仍分别稳定于约 25 FPS 和 20 FPS。使用 Ctrl+C 停止发送端，程序会先停止预览线程，再依次释放编码、VPSS、VI 和 ISP。

## 本地 NPU 人形检测（端侧推理，可选云端）

RV1106 内置 RKNPU（0.5 TOPS int8），可在摄像头本地运行 yolov5n
(320x320, int8)，把“依赖云端 API”变成“端侧推理 + 云端可选”。

### 链路

```
ai_cam 预览 DMA-BUF (/tmp/ai_cam_preview.sock)
  -> npu_detect（letterbox 320x320 + 人形 bbox + IoU tracker）
  -> TCP 9010 -> ROCK 2A npu_result_server.py
  -> event_service.py -> event_id / 最佳帧 / SQLite / 云端策略
  -> send_result_tcp.py -> TCP 9000 -> Qt 事件展示
```

NPU 输入图像只在 RV1106 本地使用；TCP 9010 发送归一化 bbox、置信度、frame_id、
时间戳、临时 track_id 与值守状态，不发送原始图像。track_id 只表示连续帧关联，
不是人脸身份。

Qt 的事件区域显示当前人数、track 数和事件状态；高频本地检测不会覆盖刚返回的
云端场景与摘要。普通人员出现只表示检测成立，当前人形模型没有危险行为或禁区
规则，不会仅因检测到人员触发告警。

### NPU 本地值守

实时 NPU 同时负责 LCD 背光值守，默认连续检测到人员 3 次后唤醒，连续无人
30 秒后休眠。休眠只向 `bl_power` 写入 powerdown，不停止摄像头、RTSP、Qt
或 NPU；因此重新检测到人员后不需要重建媒体管线。NPU 正常退出时会强制恢复
背光。可通过 supervisor 环境变量调整：

```sh
NPU_WAKE_HITS=3
NPU_IDLE_SECONDS=30
NPU_BACKLIGHT_PATH=/sys/class/backlight/backlight/bl_power
TRACK_IOU_THRESHOLD=0.3
TRACK_MAX_MISSED=4
TRACK_MIN_HITS=3
```

只有达到 `TRACK_MIN_HITS` 的 confirmed track 才参与上报和背光判定。网络断开时
TCP 建连/发送保持非阻塞，本地推理与背光不会被 ROCK 2A 拖住。检测框从模型的
320x320 letterbox 坐标去除 padding 后反算为 384x216 的 0..1 坐标。

### 板端运行（RV1106）

```sh
mkdir -p /root/userdata/npu_detect
# 交叉编译：arm-rockchip830-linux-uclibcgnueabihf-gcc src/npu_detect.c
#   -I <rknn_model_zoo>/3rdparty/rknpu2/include -lrknnmrt -lm
cp npu_detect yolov5n_320.rknn librknnmrt.so /root/userdata/npu_detect/
sh run_npu_detect.sh          # 独立启动
```

`run_rv1106_supervisor.sh` 已内置 `start_npu`：检测到
`/root/userdata/npu_detect/npu_detect` 与模型存在时自动拉起，随链路一起
看门狗/重启；模型缺失时自动跳过。

模型转换（在 PC/虚拟机的 rknn-toolkit2 1.6.0 环境）：

```sh
# airockchip/yolov5: python export.py --rknpu --weight yolov5n.pt --imgsz 320
# luckfox_pico_rknn_example: convert.py <onnx> <dataset> out.rknn Yolov5
sh convert_yolov5n_320.sh
```

注意：板端运行库为 librknnmrt 1.6.0，转换工具必须用 rknn-toolkit2 1.6.0；
RV1106 不支持板端 Python 推理（lite2 只有 aarch64 包），一律使用 C API。

### 云端可选开关（ROCK 2A）

`manual_recognize_server.py` 新增 `--backend cloud|local`（默认 cloud，
可用环境变量 `AI_BACKEND` 覆盖）：

- `cloud`：事件服务自动识别最佳 RTSP 候选帧；暂停后按钮仍可手动识别冻结帧。
- `local`：手动识别直接返回最新 NPU 检测结果，不调用云端。

supervisor 启动命令中通过 `AI_BACKEND=local` 切换即可。local 模式返回的是
`npu_latest.json` 中最近一次端侧检测结果；上传的冻结 JPEG 仍会与结果一起按
`source=manual` 缓存和保存，但当前协议不保证 NPU 检测帧与冻结帧完全相同。

生产 `event` 模式由 ROCK 事件服务调度自动请求，Qt 的旧 30 秒周期默认关闭。
仅回退测试时设置 `ENABLE_LEGACY_AUTO_RECOGNITION=1`。无 Qt 的 headless watcher
仍可专项测试，但不得与事件云端调度或 Qt 兼容周期同时启用。

NPU 检测程序使用独立构建目标，不会参与媒体发送程序链接：

```sh
make       # 只构建媒体发送程序和脚本
make npu   # 单独构建 out/npu_detect
make tracker-test  # 主机侧纯 C IoU/tracker 测试
```

### 基准（2026-08-07，yolov5n 320 int8）

- 纯推理：平均 18.3 ms/次，约 54.7 FPS（NPU 独占）。
- 与发送端/Qt 共存：推理 53.8 FPS，发送端 CPU/RSS 与 Qt CPU/RSS 无变化，
  预览保持约 14.5 FPS，可用内存波动约 1 MB、无持续增长。
- 冒烟验证：bus.jpg 检出 3 人（与官方 demo 一致）；实时预览无人时正确输出 0 人。
