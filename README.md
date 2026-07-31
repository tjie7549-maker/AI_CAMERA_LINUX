# RV1106 AI Camera

本工程运行在 Luckfox Pico RV1106 上，完成摄像头采集、本地 LCD 低延迟显示和 H.264 编码输出。
后续将在此基础上增加网络推流，由 ROCK 2A 接收视频并进行 AI 识别。

## 当前媒体管线

```text
ISP -> VI (2304x1296 NV12) -> VPSS
                              |- ch0: 中心裁剪为 1296x1296 -> 缩放到 720x720 -> LCD
                              |- ch1: 保持 16:9 -> 1280x720 NV12 -> H.264 VENC -> RTSP /live/0
                              `- ch2: 保持 16:9 -> 640x360 NV12 -> H.264 VENC -> RTSP /live/1
```

LCD 分支使用中心裁剪，人物不会因 16:9 画面直接压缩到正方形屏幕而变形；两个编码分支均保留完整的 16:9 画面。

RTSP 地址：

```text
rtsp://172.32.0.93:554/live/0  主码流，1280x720，2 Mbps
rtsp://172.32.0.93:554/live/1  子码流，640x360，1 Mbps
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
run_simple_isp_vi_to_lcd_rv1106.sh  板端运行脚本
README.md              工程说明
```

模块依次初始化 ISP、SYS、VI、VPSS、VENC、RTSP 与帧转发；随后启动 VO。这样 LCD 等待首次
vsync 时，编码与 RTSP 不会被阻塞。退出时先停止线程，再关闭 RTSP、解绑并回收媒体资源。

## 编译

SDK 默认位于相邻目录 `../luckfox-pico`：

```sh
make
```

使用其他 SDK 路径时：

```sh
make SDK_DIR=/path/to/luckfox-pico
```

编译产物：

```text
out/simple_vi_get_frame_send_vo_rv1106
out/run_simple_isp_vi_to_lcd_rv1106.sh
```

## 板端运行

运行脚本会停止 `rkipc`，使用默认的 ISP、LCD 与 RTSP 参数，并将本地 H.264 文件输出关闭：

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
scp root@172.32.0.93:/root/userdata/test.h264 /tmp/rv1106_test.h264
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
  rtsp://172.32.0.93:554/live/0
```

子码流地址为 `rtsp://172.32.0.93:554/live/1`。
