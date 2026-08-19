# 人脸考勤任务交接

仓库：`AI_CAMERA_LINUX`；分支：`feature/npu-on-device`。

## 约束

- 不得修改、替换或同时启动现有 `rv1106_ai_ui`。
- 新考勤 UI 必须是独立 720×720 Qt 程序。
- RV1106 使用 `/userdata/ai_camera` 写入数据；根分区只读。
- 当前只有 person 检测，没有人脸检测/识别模型。绝不能把 person 框当 face 框。
- 固定直连网络：RV1106 `192.168.50.2`，ROCK 2A `192.168.50.1`。
- 当前不要部署到板子；先做代码、交叉编译与测试。

## 已完成的代码

### ROCK 2A 后端

目录：`rock2a_receiver/tools/face_attendance/`。

- `service.py`：`GET /health` 与 `POST /v1/face/verify`，默认 `192.168.50.1:9012`。
- `protocol.py`：校验 JPEG、令牌、请求 ID、签到类型和人脸宽度。
- `store.py`：SQLite 人员/考勤表，请求幂等、同人同日同类型去重。
- `verifier.py`：OpenCV LBPH 验证器接口；不可用时 fail-closed。
- `person_admin.py`：本地人员库 CLI。

### RV1106 高分辨率 ROI 抓拍

涉及文件：

- `rv1106_sender/include/ai_cam.h`
- `rv1106_sender/src/ai_cam_vpss.c`
- `rv1106_sender/src/ai_cam_face_snapshot.c`
- `rv1106_sender/src/ai_cam_app.c`
- `rv1106_sender/src/main.c`

新增参数：

```sh
--face-snapshot-socket /tmp/rv1106_face_snapshot.sock --face-width 720 --face-height 720
```

启用后会新增 VPSS ch4：由 2304×1296 中心裁成正方形，缩放为 720×720 NV12，5 FPS。现有 ch0/ch1/ch2/ch3 不改变。抓拍服务使用本地 Unix socket：

```text
请求：capture <x> <y> <w> <h>\n
成功：OK <width> <height> <byte_count>\n + 连续 NV12 数据
失败：ERR <reason>\n
```

坐标是 0..1 的归一化 face bbox；ROI 至少 112×112，且偶数对齐。服务只裁剪，不做脸检。

### 独立 Qt 考勤页面

目录：`rv1106_face_attendance/`。

- 固定 720×720，独立 `.pro` 工程。
- `face_snapshot_client.cpp` 从 Unix socket 取 NV12 ROI 并转 QImage。
- `attendance_http_client.cpp` 将 ROI 编码 JPEG，异步上传至 `http://192.168.50.1:9012/v1/face/verify`。
- `attendance_window.cpp` 支持签到、签退、抓拍预览、结果状态。

当前 `AttendanceWindow::captureAndUpload()` 使用临时中央框 `0.25, 0.16, 0.50, 0.62`，仅用于打通链路，必须替换为真实人脸检测结果。

## 推荐后续工作

1. 在 RV1106 的现有 RKNN 框架接入轻量人脸检测模型（例如可运行的 SCRFD RKNN）。
2. 在有人时按约 5 FPS 检测，通过 IoU 和质量阈值稳定人脸 bbox。
3. 将 bbox 加约 15% padding 后送入抓拍服务。
4. ROCK 2A 改为特征提取 + 余弦相似度识别；单人场景可用 MobileFaceNet/ArcFace 特征库，LBPH 仅作临时兜底。
5. 编写注册工具：多张采样、质量筛选、平均特征、SQLite 存储。
6. 实板压测：RTSP 双码流、Qt 预览、person NPU、ch4 抓拍并发。
7. 最后增加 RFID 兜底、离线补传和管理页。

## 已验证与未验证

已通过：

```sh
python3 -m py_compile rock2a_receiver/tools/face_attendance/*.py
python3 -m unittest discover -s rock2a_receiver/tools/face_attendance/tests -v
git diff --check
```

未执行：RV1106 交叉编译、Qt 构建（本机没有 qmake）、实板媒体/socket/HTTP/LCD 验收。

## 工作区保护

以下文件原本已有用户的未提交 ISP AE 控制 socket 改动，后续必须保留：

- `rv1106_sender/include/ai_cam.h`
- `rv1106_sender/src/ai_cam_app.c`
- `rv1106_sender/src/ai_cam_isp.c`
- `rv1106_sender/src/main.c`

本次 face 改动还未提交。禁止使用 `git checkout` 或 `git reset --hard` 清理工作区。
