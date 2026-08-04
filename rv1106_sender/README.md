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
