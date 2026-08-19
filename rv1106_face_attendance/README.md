# RV1106 独立人脸考勤页面

此工程与 `rv1106_ai_ui` 完全独立，不修改也不启动后者。它从发送端的本地 Unix socket 按需取一张高分辨率 NV12 ROI，将其编码成 JPEG 后上传到 ROCK 2A 的 `POST /v1/face/verify`。

## 发送端启动

在原媒体进程额外添加（通常与 `--no-vo`、现有 preview 参数一起使用）：

```sh
--face-snapshot-socket /tmp/rv1106_face_snapshot.sock --face-width 720 --face-height 720
```

这个选项会启用 VPSS ch4：从 2304×1296 中心裁成正方形再缩放到 720×720，仅在页面请求时复制 ROI；不会改变现有 ch0/ch1/ch2/ch3。

## 当前人脸框边界

页面当前使用一个临时的中央引导框 `0.25,0.16,0.50,0.62`。这不是人脸检测结果，适合先联调抓拍、JPEG、HTTP、后端幂等记录。接入人脸检测模型后，应把 `AttendanceWindow::captureAndUpload` 的四个数替换为检测器输出的归一化人脸框，最小 ROI 为 112×112。

## 构建与运行

```sh
qmake rv1106_face_attendance.pro && make -j2
./rv1106_face_attendance --snapshot-socket /tmp/rv1106_face_snapshot.sock \
  --upload-url http://192.168.50.1:9012/v1/face/verify \
  --token-file /userdata/ai_camera/attendance.token
```

令牌只从本机文件读取，不写入 UI、日志或 HTTP URL。UI 固定为 720×720，使用 Qt 网络模块异步上传。
