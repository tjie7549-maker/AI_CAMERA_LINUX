# RV1106 SC3336 智能相机项目交接

## 当前结论（务必先读）

不要把本板固件的 `rkipc` RTSP 当作 Qt 预览的视频源。已经实测：`rtsp://127.0.0.1/live/0` 会短暂出帧后卡死，或向 ffmpeg 返回无效数据；Qt 由此提示“视频超时”。`/live/1` 在该固件未产生帧。

当前已采用并验证的方案是：**项目原生 RKAIQ/VI 管线独占相机，Qt 与 NPU 从同一共享预览读取帧**。

## 路径与连接

- 宿主项目：`/home/summary/linux/rv1106-smart-camera`
- 原有采集器源码：`/home/summary/linux/ai_cam/rv1106_sender`
- 开发板项目目录：`/userdata/rv1106-smart-camera`
- 板端账户：`root@172.32.0.93`（密码由用户单独提供，勿写入仓库）
- 板端启动：`sh /userdata/rv1106-smart-camera/scripts/run_demo.sh`

## 当前板端已验证状态

最近一次启动后的实测结果：

- `camera-daemon`：正常，`mode=DISPLAY`、`auto_ae=true`
- `media-sender`：正常运行（项目原生采集）
- `npu_detect`：正常运行
- Qt：成功接收 2 个 DMA-BUF；预览约 15 FPS
- 共享内存：`/dev/shm/ai_cam_preview`，384x216 RGB888，帧号持续增长
- 手动验证：进入调试 → 关闭自动 AE → 曝光设为 320 → 3 秒后回读仍为 320，参数确实生效且保持

## 架构与关键改动

### 相机与 AI

`camera-daemon` 在启动项目采集前会停止 `rkipc`，然后启动：

```text
media-sender (RKAIQ + VI + VPSS + preview shared memory)
  ├─ Qt UI: rv1106_ai_ui
  └─ AI: /userdata/npu_detect/npu_detect
```

退出演示时，守护会先停止项目采集，再重启 `rkipc`，以恢复系统默认服务。

### 554 端口冲突修复

`media-sender` 原本无条件启动自己的 RTSP 服务并占用 554；固件残留/系统服务可能也占用该端口，导致采集启动失败。

已修改 `rv1106_sender/src/ai_cam_app.c`：仅当未使用 `--preview-shm` 时才启动其 RTSP。项目当前传入 `--preview-shm`，因此不抢 554，仍保留完整的共享预览和 AI。

### 手动 AE / 自动 AE

调试控制通过 `media-sender` 的 Unix Socket `/tmp/rv1106_isp_control.sock`：

- `manual <曝光行数> <增益>`：原生 RKAIQ 手动 AE
- `auto`：恢复自动 AE

该固件有时会拒绝“运行中手动 → 自动”的热切换。`camera-daemon::exit_debug()` 已做兼容：热切换失败时，仅重启项目采集管线以恢复自动 AE，不重启开发板、不触碰网络。

## 主要文件

- `rv1106-smart-camera/rv1106-sender/camera-daemon/src/camera_daemon.cpp`
  - 生命周期、rkipc 接管/恢复、UI 请求、手动控制。
- `rv1106-smart-camera/scripts/run_demo.sh`
  - 一键启动、Ctrl+C 清理、守护进程退出后的自动拉起。
- `rv1106-smart-camera/rv1106-sender/qt-console/src/camera_debug_dialog.cpp`
  - 调试页布局、数值输入、恢复安全基线按钮。
- `ai_cam/rv1106_sender/src/ai_cam_app.c`
  - `--preview-shm` 模式下禁用本地 RTSP 端口绑定。
- `rv1106-smart-camera/kernel-patches/0001-media-sc3336-harden-stream-errors-and-add-stats.patch`
  - SC3336 驱动增强补丁；目前只在项目中保存，尚未编译/刷入内核。

## 已知限制与注意事项

1. `rkipc` RTSP 方案已废弃为展示视频源；不要再把 bridge 设为默认采集路径。
2. 演示运行时相机由项目独占，其他需要 rkipc 摄像头的应用应在退出项目后再启动。
3. 不要直接用 Ctrl+C 杀 `camera-daemon` 或 `rkipc`；使用 `run_demo.sh` 的 Ctrl+C，让脚本完成顺序清理。
4. 若替换正在运行的板端二进制，先退出 UI/项目进程，否则会遇到 `Text file busy`。
5. 交叉编译：`sh /home/summary/linux/rv1106-smart-camera/scripts/build_app.sh`；采集器单独编译：`make -C /home/summary/linux/ai_cam/rv1106_sender SDK_DIR=/home/summary/linux/luckfox-pico`。

## 建议下一步

先不要更改相机所有权架构。优先做 UI 细节、日志展示、驱动补丁的编译刷入验证；每次改动后按“启动 → 等待 20 秒 → 检查共享帧帧号与 Qt FPS → 调试页手动 AE 回读”验证。
