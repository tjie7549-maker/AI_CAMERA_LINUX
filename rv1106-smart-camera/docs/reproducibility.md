# 复现与可移植性手册

## 1. 固定输入

本项目不要求把个人目录、板子密码或 API Key 写入仓库。复制模板并仅在本机填写：

```sh
cd rv1106-smart-camera
cp configs/build.env.example configs/build.env
cp configs/board.env.example configs/board.env
cp configs/config.local.json.example configs/config.local.json
```

- `build.env`：Luckfox SDK、qmake 和构建输出位置；
- `board.env`：板子地址和用户；
- `config.local.json`：传感器节点、IQ 文件、NPU 路径等板端差异。

后三者已被 `.gitignore` 忽略。密码始终通过 SSH 交互输入或 SSH key 提供。

## 2. 宿主机构建

先验证环境，不下载或安装任何依赖：

```sh
make doctor ENV=configs/build.env
make build ENV=configs/build.env
make test
```

`doctor` 会检查 SDK 的 `media/Makefile.param`、交叉 GCC、Buildroot qmake 和所需的宿主命令。
`test` 覆盖脚本语法、配置契约和 IoU tracker；这些测试不需要相机或开发板。

## 3. 版本化交付

构建完成后打包：

```sh
make package VERSION=rv1106-sc3336-v0.1.0
```

压缩包包含四个可执行程序、字体、运行配置、启动脚本（含开机自启动服务与开关脚本）和 `manifest.txt`。manifest 记录 Git commit、UTC 时间和每个交付文件的 SHA256，用于同学或面试官复核“代码版本—二进制—板端行为”的对应关系。

常规部署仍保持原有的非破坏性方式：

```sh
./scripts/deploy_app.sh --env configs/board.env --config configs/config.local.json
```

需要开机启动时，在部署命令末尾增加 `--install-autostart`。这会安装本项目专用的
`/oem/usr/etc/init.d/S99rv1106-smart-camera`；服务延迟 25 秒执行 `run_demo.sh`。
在板端执行 `scripts/autostart_ctl.sh disable` 会创建
`/userdata/rv1106-smart-camera/.autostart-disabled`，立即停止当前项目并禁止后续开机启动；
执行 `enable` 删除该文件并重新允许启动。它不会启用或修改旧 `/userdata/ai_camera` 项目。

脚本只写 `/userdata/rv1106-smart-camera/`，不写系统目录，也不会删除远端文件。

## 4. 运行模式

| 模式 | 入口 | 相机所有权 | 用途 |
| --- | --- | --- | --- |
| 展示 | `run_demo.sh` | 项目原生 RKAIQ/RKMPI 管线 | 默认演示，Qt 与 NPU 读同一共享预览 |
| 驱动调试 | `mode_ctl.sh debug` 或 UI“设备调试” | 仍由项目管线持有 | 手动 AE/AGC、曝光、增益、VBLANK、翻转和恢复快照 |
| 系统兼容 | 不启动本项目 | `rkipc` | 仅用于厂商系统排障；本固件不保证 Qt 经 rkipc RTSP 的预览稳定性 |

退出项目时 daemon 会停止项目子进程并恢复 `rkipc`。`display` 只退出参数页，不会为了切回自动 AE 重建媒体管线，因此不会替换 Qt 正在使用的 DMA-BUF。

## 4.1 背光与内存看门狗

这两项由当前 `camera-daemon` 启动和监管，不依赖旧的 `/userdata/ai_camera` supervisor：

- `backlight_control=true` 时，daemon 启动 NPU 时传入背光策略：连续确认人形
  `backlight_wake_hits=3` 次后点亮，连续无人 `backlight_idle_seconds=30` 秒后熄灭；
- `memory_watchdog_enabled=true` 时，daemon 每 `memory_check_interval_ms=10000` 毫秒读取
  `/proc/meminfo` 的 `MemAvailable`；低于 `memory_available_min_kb=40960` 连续
  `memory_low_checks=3` 次后，有序停止并重启本项目的 media-sender 与 NPU；
- 看门狗只重启项目子进程，不重启系统、不删除数据，也不会杀死 `rkipc` 以外的系统服务。

若板卡背光节点不同，必须在 `config.local.json` 调整 `backlight_path`。不希望演示中自动
熄屏时，将 `backlight_control` 设为 `false`；不希望自动恢复时，将
`memory_watchdog_enabled` 设为 `false`。

## 5. 迁移到另一块板子

1. 从 `config.local.json.example` 复制本机配置；
2. 通过 `v4l2-ctl --list-devices` 和媒体拓扑确认 `sensor_subdev`；
3. 设置正确的 `iq_dir`、NPU 程序与模型路径；
4. 执行 `camera_baseline.sh`、`v4l2_smoke_test.sh`；
5. 再运行展示模式和调试模式验收。

若换用非 SC3336 传感器，应新增 sensor backend，而不是把新控制 ID、曝光换算或节点号直接散落在 Qt 代码中。Qt 只发送通用控制请求，`camera-daemon` 是唯一的硬件控制仲裁点。
