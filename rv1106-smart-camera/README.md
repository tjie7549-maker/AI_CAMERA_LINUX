# RV1106 SC3336 摄像头驱动增强与低照度智能视觉终端

本项目用于在 Luckfox Pico Ultra / Ultra W（RV1106、armhf、Buildroot Linux）上，围绕已经跑通的 SC3336 摄像头链路，逐步完成可回退的 V4L2 驱动增强、低照度策略、camera-daemon 与 Qt 调试控制台。

已完成阶段 0–5 的源码、补丁、交叉编译和主要板端联调。项目没有删除或覆盖原 AI 应用的板端目录；展示版本独立部署到 `/userdata/rv1106-smart-camera/`。

## 目录说明

```text
rv1106-smart-camera/
├── rv1106-sender/               RV1106 发送端核心程序
│   ├── camera-daemon/            相机唯一控制仲裁者：管理 rkipc、媒体管线、NPU、
│   │                              自动/手动 AE 和 V4L2 参数；通过 Unix Socket 给 UI 提供接口
│   │   ├── include/              daemon 对外 C++ 头文件
│   │   └── src/                  daemon 入口、配置读取、控制与进程监管实现
│   ├── media-sender/             RV1106 原生 RKMPI/RKAIQ 采集管线：SC3336、VPSS、
│   │                              RTSP、共享预览和 RKNN 人形检测
│   │   ├── include/              媒体管线、IoU 跟踪和共享预览协议头文件
│   │   ├── src/                  ISP/VI/VPSS/VENC/RTSP/NPU 等实现
│   │   └── tests/                IoU 跟踪器的无需硬件单元测试
│   ├── qt-console/               RV1106 触摸屏 Qt 界面：实时预览、AI 结果、设备调试、
│   │                              手动曝光/增益和日志展示
│   │   ├── fonts/                板端中文字体
│   │   ├── include/              Qt 窗口、Socket 客户端、共享内存读取等头文件
│   │   ├── resources/            Qt 样式表和资源索引
│   │   ├── scripts/              单独编译或调试 Qt 界面的辅助脚本
│   │   └── src/                  Qt UI 与预览/识别/调参逻辑
│   ├── rtsp-preview-bridge/      兼容性 RTSP 到共享预览桥接；默认展示不依赖它
│   └── shared/                   预留给 daemon、媒体管线和 UI 的公共协议
├── rock2a-receiver/              可选的 ROCK 2A 识别端：接收 RTSP、事件选帧、SQLite、
│                                  千问视觉识别和 TCP 回传（不属于人脸考勤）
│   ├── config/                   ROCK 2A 服务环境变量模板
│   ├── include/、src/            C++ RTSP 接收器
│   ├── systemd/                  ROCK 2A systemd 服务单元
│   ├── tests/                    事件、HTTP、存储相关测试
│   └── tools/qwen_vision/        Python 事件引擎、千问调用、结果缓存与回传服务
├── configs/                      可移植配置
│   ├── config.json               默认板端运行配置（可直接部署）
│   └── *.example                 本地 SDK、板卡地址、传感器/NPU 差异的模板；复制后不入库
├── scripts/                      主流程脚本：构建、部署、启动、压力测试、调试和 release 打包
├── tools/                        主机侧工具，例如 doctor.sh 环境自检
├── tests/                        不需要硬件的脚本语法、配置契约等回归测试
├── docs/                         架构、基线、驱动、测试、复现和面试说明
│   └── ai-recognition/           ROCK 2A 事件识别链路的架构/API/迁移文档
└── kernel-patches/               可审查、可回退的 SC3336 V4L2 驱动增强补丁
```

`rv1106-sender/README.md` 提供板端进程、数据通道和源码入口的代码导览。构建产物位于 `build/`，release 包位于 `release/`；二者以及本机配置均被 Git 忽略。
板端部署目录固定为 `/userdata/rv1106-smart-camera/`，不在源码树中。

## 构建、部署与使用

主机交叉构建（不下载依赖）：

```sh
./tools/doctor.sh
./scripts/build_app.sh
./scripts/deploy_app.sh --ip 172.32.0.93 --dry-run
./scripts/deploy_app.sh --ip 172.32.0.93
```

在另一台主机或另一块板子上，先复制 `configs/*.example` 为本地配置，再通过
`--env configs/build.env`、`--env configs/board.env` 和 `--config configs/config.local.json`
覆盖默认值；详见 `docs/reproducibility.md`。

部署默认只写 `/userdata/rv1106-smart-camera/`，不会删除远端文件。板端推荐运行 `scripts/run_demo.sh`；压力测试用 `scripts/stress_test.sh 10`。运行期间项目原生 RKAIQ/RKMPI 管线独占相机，Qt 与 NPU 读取同一份 384×216 DMA-BUF 共享预览；退出后恢复系统 `rkipc`。

### 当前项目开机自启动

首次部署时使用 `./scripts/deploy_app.sh --ip 172.32.0.93 --install-autostart`，仅额外安装当前项目的启动入口到 `/oem/usr/etc/init.d/S99rv1106-smart-camera`。系统启动后会等待 25 秒，再运行 `/userdata/rv1106-smart-camera/scripts/run_demo.sh`，避免与系统媒体服务的启动阶段争用相机。

板端开关不改系统文件，只使用项目目录里的标记文件：

```sh
# 禁用后立即停止本项目；下次开机也不会启动
sh /userdata/rv1106-smart-camera/scripts/autostart_ctl.sh disable

# 删除禁用标记并安排一次启动；下次开机同样生效
sh /userdata/rv1106-smart-camera/scripts/autostart_ctl.sh enable

# 查看开关、启动包装器及 camera-daemon 状态
sh /userdata/rv1106-smart-camera/scripts/autostart_ctl.sh status
```

## 架构与增强点

SC3336 驱动补丁增强 I2C 错误传播、`s_stream()` 状态回滚、runtime PM 平衡，并在 debugfs 提供只读统计。camera-daemon 监督既有 RKMPI/RGA/RTSP/RKNN 进程，使用 JSONL 记录状态切换；Qt 仅通过 Unix Socket 请求 daemon，绝不直接控制传感器。系统 `rkipc` 的 RTSP 在本固件上存在卡流问题，因此展示模式使用项目原生采集；主/子 H.264 流改用不冲突的 8554 端口（`/live/0`、`/live/1`）。

本项目是对 BSP 中既有 SC3336 V4L2 驱动的增强，不是从零重写 ISP 或 MIPI CSI 驱动。

## 测试、限制与面试表述

真实结果见 `docs/test-report.md`；未真机验证项均保留为“待验证”。Qt 预览约 15 FPS、NPU 推理、手动曝光保持和 8554 双码流均已板端验证；真实低照度指标、内核补丁刷入和长稳测试仍明确标记为待验证，不把模拟数据写成实测结论。

简历描述：**在 RV1106/Luckfox Pico Ultra 上增强 SC3336 V4L2 驱动的错误恢复与 debugfs 可观测性，设计以 Unix Socket 控制仲裁的低照智能视觉终端，复用 RKMPI/RGA/RKNN 双码流链路并提供 Qt 调试控制台。**

面试可重点讲：标准 V4L2 control 与 RKAIQ AE/AGC 的所有权冲突如何避免；sensor I2C 短传输为何必须视作失败；应用 pipeline 重建如何避免无限忙等；以及为何用媒体拓扑基线而不是写死 `/dev/videoX`。

## 阶段约束

- 本项目是对 BSP 中既有 SC3336 V4L2 sensor driver 的增强，不是从零重写 ISP、CSI 或传感器驱动。
- 仅把已有板端日志或拉流结果的项目标为“已验证”，其余项目保持“待验证”。
- 下一阶段开始前需先确认实际启动 DTB、板端媒体拓扑和 SC3336 control 行为。
- SDK 当前工作树已有与本任务无关的修改；后续内核工作必须在单独分支/工作树中进行，并保留原工作树。

## 文档索引

- `docs/pre-implementation-audit.md`：阶段 0 实施前审计报告
- `docs/baseline.md`：阶段 1 板端执行与结果保存方法
- `docs/open-questions.md`：必须由板端或硬件资料确认的问题
- `docs/reproducibility.md`：环境自检、配置外置、版本化交付与迁移步骤
