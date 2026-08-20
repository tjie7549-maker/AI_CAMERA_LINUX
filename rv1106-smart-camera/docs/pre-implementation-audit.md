# 实施前审计报告（阶段 0）

审计日期：2026-08-13  
审计方式：只读检查 `/home/summary/linux/luckfox-pico` 与 `/home/summary/linux/ai_cam`。本阶段未修改二者。

## 1. SC3336 驱动定位

实际匹配 `compatible = "smartsens,sc3336"` 的驱动为：

```text
/home/summary/linux/luckfox-pico/sysdrv/source/kernel/drivers/media/i2c/sc3336.c
```

SDK 中同时存在：

```text
/home/summary/linux/luckfox-pico/sysdrv/source/kernel/drivers/media/i2c/sc3336p.c
```

但 Ultra DTS 使用 `smartsens,sc3336`，不是 `sc3336p`；因此后续补丁目标应是 `sc3336.c`，除非板端 `media-ctl -p`、`dmesg` 或实际 DTB 证明不同。

## 2. Ultra DTS 与 SC3336 链路

普通启动入口：

```text
/home/summary/linux/luckfox-pico/sysdrv/source/kernel/arch/arm/boot/dts/rv1106g-luckfox-pico-ultra.dts
```

该文件包含实际摄像头定义文件：

```text
/home/summary/linux/luckfox-pico/sysdrv/source/kernel/arch/arm/boot/dts/rv1106-luckfox-pico-ultra-ipc.dtsi
```

SC3336 节点位于上述 `.dtsi` 的 `&i2c4` 下（约 274 行）：

- 节点：`sc3336: sc3336@30`
- compatible：`smartsens,sc3336`
- I2C 地址：`0x30`
- 时钟：`MCLK_REF_MIPI0`，名称 `xvclk`
- reset：`gpio3 RK_PC5 GPIO_ACTIVE_HIGH`
- pinctrl：`mipi_refclk_out0`
- endpoint：`sc3336_out`，`data-lanes = <1 2>`

现有端点链路为：

```text
SC3336 sc3336_out
  -> csi2_dphy0/csi_dphy_input0
  -> csi_dphy_output
  -> mipi0_csi2/mipi_csi2_input
  -> mipi_csi2_output
  -> rkcif_mipi_lvds/cif_mipi_in
  -> rkcif_mipi_lvds_sditf/mipi_lvds_sditf
  -> rkisp_vir0/isp_in
```

注意：`rv1106g-luckfox-pico-ultra-fastboot.dts` 当前定义的是 SC3338（`smartsens,sc3338`），不能把该文件的 GPIO、节点或启动假设直接套到 SC3336 普通启动配置。实际产品使用哪个 DTB 仍须由板端启动日志/固件配置确认。

## 3. 驱动现有 V4L2 controls

`sc3336_initialize_controls()` 已注册：

| Control | 当前状态 | 备注 |
|---|---|---|
| `V4L2_CID_EXPOSURE` | 已实现，可写 | VBLANK 变化会同步调整曝光上限 |
| `V4L2_CID_ANALOGUE_GAIN` | 已实现，可写 | 使用驱动既有增益换算与寄存器 |
| `V4L2_CID_VBLANK` | 已实现，可写 | 更新 VTS 和驱动维护的帧间隔 |
| `V4L2_CID_HFLIP` | 已实现，可写 | 对镜像寄存器做 read-modify-write |
| `V4L2_CID_VFLIP` | 已实现，可写 | 对翻转寄存器做 read-modify-write |
| `V4L2_CID_TEST_PATTERN` | 已实现，菜单 | Disabled + 4 种 Vertical Color Bar |
| `V4L2_CID_LINK_FREQ` | 已实现，只读 | 两项既有频率菜单 |
| `V4L2_CID_PIXEL_RATE` | 已实现 | 由当前 mode/link frequency 派生 |
| `V4L2_CID_HBLANK` | 已实现，只读 | 固定为当前 mode 的水平 blanking |

这里只能证明源码已注册 control；control 是否经 ISP/media pipeline 暴露到预期节点、设置后是否生效，必须用板端 `v4l2-ctl --list-ctrls-menus` 和实际画面验证。

## 4. 驱动错误路径审计

值得在阶段 2 做最小修复的事实：

1. `sc3336_write_reg()` 将所有短写和负 errno 统一返回 `-EIO`；`sc3336_read_reg()` 也把负的 `i2c_transfer()` errno折叠成 `-EIO`。这会丢失底层错误语义，且当前没有读写/失败计数。
2. 曝光、增益、VBLANK、翻转和测试图的多寄存器操作多处使用 `ret |= ...`。失败后仍继续 I2C 操作，并可能把 errno 位或后变成不清晰的返回值；read-modify-write 在 read 失败后仍可能写入默认值。
3. `sc3336_s_stream()` 的 stop 分支忽略 `__sc3336_stop_stream()` 返回值，随后无条件 `pm_runtime_put()` 并把 `streaming` 更新为 false。软件状态可能与传感器实际状态不一致。
4. thunderboot 失败恢复路径调用 `__sc3336_power_on()` 时未检查返回值。
5. `sc3336_s_power()` 在 runtime PM get 成功、全局寄存器写失败后调用 `pm_runtime_put_noidle()`；阶段 2 需结合内核 5.10 PM 语义检查并成对回退，避免持有 usage count 或设备状态不一致。
6. 目前没有题目要求的只读统计接口：I2C 读写次数、失败次数、最后错误码、最后启动耗时和流状态均未导出。

不得在没有板级证据时改寄存器表、增加 mode、猜测电源 rail 或 GPIO。`probe/remove` 的 regulator、clock、GPIO 与 thunderboot 路径也应在补丁前逐条复核，但不能仅凭静态阅读宣称存在真机故障。

## 5. 原应用结构与可复用模块

原工程未发现题目所称的顶层 `luckfox/` 目录；当前可复用实现位于 `rv1106_sender/` 与 `rv1106_ai_ui/`。

### RV1106 媒体发送端

```text
rv1106_sender/src/main.c              参数解析、信号处理、应用入口
rv1106_sender/src/ai_cam_app.c        pipeline 总体启动、失败回退、停止、统计
rv1106_sender/src/ai_cam_isp.c        RKAIQ 初始化/prepare/start/stop/deinit
rv1106_sender/src/ai_cam_vi.c         RKMPI VI 初始化与销毁
rv1106_sender/src/ai_cam_vpss.c       VPSS 通道、裁剪与缩放
rv1106_sender/src/ai_cam_vo.c         720x720 LCD/VO 与旋转
rv1106_sender/src/ai_cam_venc.c       720P/360P H.264、绑定、取流线程
rv1106_sender/src/ai_cam_rtsp.c       /live/0、/live/1 RTSP 会话生命周期
rv1106_sender/src/ai_cam_preview.c    VPSS ch3 + RGA + DMA-BUF/Unix socket 预览
rv1106_sender/src/npu_detect.c        RKNN 本地检测（独立进程，读取预览共享内存）
rv1106_sender/src/iou_tracker.c       检测框 IoU 跟踪
rv1106_sender/include/ai_cam.h        配置、状态、模块接口
rv1106_sender/Makefile                SDK media 参数与 armhf/uClibc 工具链入口
```

现有已实现链路：2304x1296 sensor/VI → VPSS；ch0 为 720x720 LCD，ch1 为 1280x720 H.264/RTSP `/live/0`，ch2 为 640x360 H.264/RTSP `/live/1`，ch3 为 384x216 RGA RGB 预览。`ai_cam_start()` 已按阶段初始化，失败跳转回收；`ai_cam_stop()` 负责线程和媒体资源退出。这些是 camera-daemon 应抽取/包装的基线，不应重写。

本地 NPU 程序使用 RKNN runtime 和预览共享内存；Qt 还通过 TCP/HTTP 对接既有识别与事件服务。后续 daemon 化必须保持这些接口兼容或提供清晰迁移层，不能因新增低照度状态机而移除识别链路。

### Qt5 Widgets

```text
rv1106_ai_ui/rv1106_ai_ui.pro         Qt5/C++11 构建入口
rv1106_ai_ui/scripts/build.sh         指定 Luckfox Buildroot qmake
rv1106_ai_ui/src/main_window.cpp      当前单窗口预览、识别结果、状态与事件交互
rv1106_ai_ui/src/preview_shm_reader.cpp DMA-BUF/共享预览读取
rv1106_ai_ui/src/video_widget.cpp     实时画面绘制
rv1106_ai_ui/src/ai_result_client.cpp TCP AI 结果客户端
rv1106_ai_ui/src/manual_recognition_client.cpp HTTP 手动/自动识别
rv1106_ai_ui/src/result_storage_client.cpp 事件保存/恢复
rv1106_ai_ui/src/status_controller.cpp 连接与识别状态
```

现有 `.pro` 使用 `QT += core gui widgets network`、`CONFIG += c++11 release`。后续只增加必要调试页与 Unix domain socket 客户端，不让 UI 直接打开 V4L2 节点或写 sensor 寄存器。

### 当前构建与部署

- sender：`rv1106_sender/Makefile`，包含 SDK `media/Makefile.param`，交叉前缀为 SDK 内 `arm-rockchip830-linux-uclibcgnueabihf`。
- NPU：同一 Makefile 的 `make npu`，依赖既有 `rknpu2` armhf-uClibc include/lib。
- Qt：`rv1106_ai_ui/scripts/build.sh`，qmake 为题目指定的 Buildroot host qmake。
- 当前部署脚本会写 `/userdata/ai_camera`，并且还会写 `/oem/usr/etc/init.d`；新项目后续部署必须改为只写 `/userdata/rv1106-smart-camera/`。

## 6. 修改边界

后续需要修改/新增：

- 在独立 SDK 分支/工作树中最小修改 `sc3336.c`，输出 patch 到本项目。
- 只有板端确认普通 Ultra DTS 与硬件实际不一致时才修改 Ultra `.dtsi`，并生成独立 DTS patch。
- 从 sender 复用媒体模块，新增 daemon 生命周期、策略、日志与 Unix socket 控制层。
- 在现有 Qt 工程基础上移植到本项目并最小扩展调试页面。
- 新增只写 `/userdata/rv1106-smart-camera/` 的构建、部署、启动和测试脚本。

绝对不应修改：

- SC3336 既有 mode/寄存器表（除非出现 SDK 内可靠表或数据手册证据）。
- ISP/CSI/RKMPI/RGA/RKNN 的既有工作原理和已跑通的双 RTSP/AI 识别链路。
- 自动曝光/增益策略不进入内核；生产接口不允许任意寄存器读写。
- 原 `ai_cam`、SDK 当前工作树和系统只读根文件系统。

## 7. 主要风险

| 风险 | 影响 | 控制措施 |
|---|---|---|
| 实际启动的是 fastboot/其他 DTB | 对错误设备树打补丁，摄像头失效 | 先保存启动日志、`/proc/device-tree/model` 与媒体拓扑 |
| control 位于 sensor subdev 而非 capture node | 脚本/UI 误报不支持 | 基线同时记录 media topology，并允许明确传入 subdev/capture 节点 |
| RKAIQ AE/AGC 与手动 V4L2 写入冲突 | 曝光抖动或设置立即被覆盖 | daemon 统一仲裁；自动模式拒绝曝光/增益写请求 |
| stream stop I2C 失败与 PM 状态不一致 | 后续重启失败、资源引用失衡 | 补丁保留首个 errno、只在成功时更新状态，设计失败回滚测试 |
| 原媒体资源只有单 owner | baseline/smoke 与现有 rkipc/sender 冲突 | 测试前明确停止占用者，测试后按原方式恢复；脚本不主动杀进程 |
| SDK 与 ai_cam 工作树已有用户修改 | 覆盖现有成果 | 使用新 worktree/branch；不 reset/clean，不覆盖复制 |
| uClibc/BusyBox 工具有限 | 主机可用脚本板端失败 | 脚本保持 POSIX sh，并明确报告缺失依赖 |
| 低照策略增加 NPU/CPU 负载 | 降低预览、编码或检测帧率 | 后续从已有帧/统计抽样，事件驱动定时，不高频忙等 |

