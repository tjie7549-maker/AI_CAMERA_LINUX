# RV1106 SC3336 摄像头驱动增强与低照度智能视觉终端

本项目用于在 Luckfox Pico Ultra / Ultra W（RV1106、armhf、Buildroot Linux）上，围绕已经跑通的 SC3336 摄像头链路，逐步完成可回退的 V4L2 驱动增强、低照度策略、camera-daemon 与 Qt 调试控制台。

已完成阶段 0–5 的源码、补丁、交叉编译和主要板端联调。项目没有删除或覆盖原 AI 应用的板端目录；展示版本独立部署到 `/userdata/rv1106-smart-camera/`。

## 当前目录

```text
kernel-patches/          后续阶段保存可审查、可回退的内核与 DTS patch
app/camera-daemon/       后续阶段的守护进程
app/qt-console/          后续阶段从既有 Qt 工程最小化扩展
app/shared/              daemon/Qt 共用协议与数据结构
scripts/                 板端基线和 V4L2 冒烟测试
configs/                 后续阶段的示例配置
docs/                    审计、基线说明与待确认问题
tests/                   脚本静态检查及后续测试
```

## 构建、部署与使用

主机交叉构建（不下载依赖）：

```sh
./scripts/build_app.sh
./scripts/deploy_app.sh --ip 172.32.0.93 --dry-run
./scripts/deploy_app.sh --ip 172.32.0.93
```

部署只写 `/userdata/rv1106-smart-camera/`，不会删除远端文件或覆盖系统目录。板端推荐运行 `scripts/run_demo.sh`；压力测试用 `scripts/stress_test.sh 10`。运行期间项目原生 RKAIQ/RKMPI 管线独占相机，Qt 与 NPU 读取同一份 384×216 DMA-BUF 共享预览；退出后恢复系统 `rkipc`。

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
