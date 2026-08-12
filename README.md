# AI Camera Linux

RV1106 摄像头发送端与 ROCK 2A 接收、视觉识别端的单仓库工程。

```text
SC3336 -> RV1106 ISP/VI/VPSS/VENC -> H.264 RTSP -> ROCK 2A JPEG ring
                    |                                      |
                    `-> RKNN bbox + IoU track -> event engine -> Qwen Vision
                                                   |              |
                                  RV1106 Qt UI <- TCP JSON   SQLite + 最佳图
```

## 目录

```text
rv1106_sender/     Luckfox Pico RV1106：LCD 预览、H.264 双码流与 RTSP 服务
rock2a_receiver/   ROCK 2A：RTSP 解码、JPEG 抽帧、latest.jpg 与千问视觉识别
rv1106_ai_ui/      RV1106：720x720 Qt 中文界面与 AI JSON TCP 客户端
docs/               架构、事件模型/API、迁移和测试说明
```

## 当前网络

```text
RV1106 有线（静态）：192.168.50.2/24
ROCK 2A 有线（静态）：192.168.50.1/24
RV1106 RTSP：
rtsp://192.168.50.2:554/live/0   主码流 1280x720，25 FPS
rtsp://192.168.50.2:554/live/1   子码流 640x360，20 FPS
```

说明：RV1106 eth0 使用 ifupdown 静态配置（`/etc/network/interfaces`）并禁止 DHCP
（`/etc/dhcpcd.conf` 中 `denyinterfaces eth0`）；ROCK 2A end1 使用手动静态地址，
不提供 DHCP 服务。冷启动后 eth0 仅存在 192.168.50.2。

## 系统架构

```text
┌────────────────────────── RV1106（Luckfox · 采集/显示端）─────────────────────────┐
│ SC3336 摄像头                                                                     │
│   -> ISP/VI (2304x1296) -> VPSS                                                   │
│       ├─ ch0: 中心裁剪 1296x1296 -> 720x720 -> LCD（Qt 独占，no-VO）              │
│       ├─ ch1: 1280x720 H.264 -> RTSP /live/0（约 25 FPS）                         │
│       ├─ ch2: 640x360 H.264 -> RTSP /live/1（约 20 FPS）                          │
│       └─ ch3: 384x216 -> RGA -> DMA-BUF -> Qt 预览 + NPU 人形检测                 │
│                                                                                   │
│  supervisor（/etc/init.d/S95ai-camera 开机自启）                                  │
│    拉起 ai_cam + Qt + NPU -> 无人熄背光/有人唤醒 -> 看门狗与日志轮转              │
└───────────────────────────────┬───────────────────────────────────────────────────┘
                                │ RTSP H.264 双码流（192.168.50.2 ⇄ 192.168.50.1）
┌───────────────────────────────▼───────────────────────────────────────────────────┐
│ ROCK 2A（接收/识别端）                                                             │
│ systemd ai-camera.service -> ROCK Supervisor（唯一重启决策层）                    │
│   ├─ RTSP 接收器（FFmpeg 解码） -> runtime/ai_cam/latest.jpg                     │
│   ├─ NPU track（TCP 9010）-> event service（HTTP 9011）-> SQLite                  │
│   ├─ 24 张 JPEG ring -> 最佳帧 -> 事件驱动云端识别                                │
│   ├─ HTTP 9001：暂停帧手动识别与旧结果保存                                         │
│   ├─ watcher：仅供无 Qt 的 headless 专项测试                                      │
│   ├─ TCP 9000：结果回传 -> RV1106 Qt 显示                                          │
│   └─ 保存：events/<event_id>；兼容 saved_results/{manual,auto}                    │
└────────────────────────────────────────────────────────────────────────────────────┘
```

## 新增功能（产品化部署，2026-08）

### 1. 静态 IP 统一

- 移除 `/oem/usr/bin/RkLunch.sh` 中 `udhcpc -i eth0`；
- `/etc/dhcpcd.conf` 增加 `denyinterfaces eth0`；
- `/etc/network/interfaces` 配置 eth0 静态 `192.168.50.2/24`；
- ROCK 2A end1 关闭 NetworkManager 共享 DHCP，仅保留手动静态地址；
- 冷启动验证：eth0 只存在 192.168.50.2，无 .166，双向 ping / SSH / RTSP 正常。

### 2. 双端监督与开机自启

- RV1106：`rv1106_sender/run_rv1106_supervisor.sh` + `/etc/init.d/S95ai-camera`
  （start/stop/restart/status，PID 文件防重复实例）；
- ROCK 2A：`rock2a_receiver/run_rock2a_supervisor.sh` + systemd 单元
  `rock2a_receiver/systemd/ai-camera.service`；
- 只有一层负责最终重启策略：systemd 只管 supervisor，组件恢复由 supervisor 负责。

### 3. 自动恢复

- RTSP 接收器、HTTP 9001、TCP 9000、自动 watcher 异常退出均自动重启；
- 退避策略：1/2/4/8/10 秒，5 次/60 秒上限，连续 5 次失败放弃并写错误日志；
- 断网特例：RV1106 不可达时不计失败、每 10 秒重试，网络恢复自动重连；
- 用户主动退出（Qt 退出码 42）识别为正常停止，systemd 不会自动拉起。

### 4. 自动保存策略

- `watch_latest_image.py` 支持 `--auto-save-policy none|warning|all`（默认 warning）；
- `--auto-save-dedup-seconds 60`：相同 scene/objects/warning 组合 60 秒内不重复保存，
  去重状态落盘（`runtime/auto_save_dedup.json`），重启后仍生效；
- 复用 `result_cache.save()` 同一套保存逻辑，不复制第二套代码。

### 5. 磁盘空间保护

- 保存前检查可用空间，`--min-free-mb`（默认 1024MB）；
- 低于阈值：自动保存跳过并记日志，手动保存返回 HTTP 507 明确错误，识别继续运行。

### 6. 日志管理

- ROCK 2A：`rock2a_receiver/runtime/logs/`（supervisor/rtsp_receiver/manual_server/
  tcp_sender/auto_watcher/save_stats）；
- RV1106：`/root/userdata/ai_camera/logs/`（supervisor/ai_cam/qt）；
- 单文件 5MiB、保留 3 份轮转；日志不含 API Key。

### 7. RV1106 内存看门狗

- 每 10 秒检查 MemAvailable，低于 40MB 且持续 30 秒自动重启整套链路，释放媒体缓冲；
- 实测：40.8MB -> 117MB 自动恢复，避免 OOM（厂商媒体栈存在 CMA 缓冲累积问题，
  该看门狗为产品化缓解）。

### 8. NPU 本地值守

- 摄像头、RTSP、Qt 和 NPU 始终运行，只关闭 LCD 背光，不反复初始化媒体管线；
- 连续检测到人员 3 次后立即点亮背光；连续无人 30 秒后关闭背光；
- NPU 只在 RV1106 本地处理预览帧，向 ROCK 2A 发送归一化人形框、临时 track_id
  和值守状态，不上传 NPU 输入图像；
- ROCK 2A 按对象事件选择 RTSP 最佳帧并低频调用千问，Qt 不再负责生产环境定时调度；
- 点击“暂停”后仍可由“识别”按钮提交精确冻结帧；
- NPU 退出时恢复背光，避免服务停止后 LCD 保持黑屏。

## 构建与运行

RV1106 发送端使用 Luckfox SDK 交叉编译：

```sh
cd rv1106_sender
make
```

ROCK 2A 接收与识别端的构建、配置和一键运行方式见 `rock2a_receiver/README.md`。
系统架构与两端职责见 [docs/architecture.md](docs/architecture.md)。

RV1106 Qt 界面使用 SDK 的交叉 qmake 编译：

```sh
cd rv1106_ai_ui
./scripts/build.sh
./scripts/deploy.sh
```

Qt 与 RKMPI VO 不能同时占用 LCD。智能视觉模式中使用 no-VO 发送端，由 Qt 独占 LCD；
发送端通过 CMA DMA-BUF 把实时预览帧传给 Qt。

## 部署与启动手册

### 部署 RV1106

板端部署目录：`/root/userdata/ai_camera/`

```sh
# 1) 部署发送端（交叉编译产物 + 启动脚本）
scp rv1106_sender/out/simple_vi_get_frame_send_vo_rv1106 \
    rv1106_sender/out/run_ai_headless_preview.sh \
    root@192.168.50.2:/root/userdata/ai_camera/rv1106_sender/

# 2) 部署 Qt（交叉编译产物 + 运行脚本 + 中文字体）
scp rv1106_ai_ui/build/rv1106_ai_ui rv1106_ai_ui/scripts/run.sh \
    root@192.168.50.2:/root/userdata/ai_camera/rv1106_ai_ui/
scp /usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf \
    root@192.168.50.2:/root/userdata/ai_camera/rv1106_ai_ui/fonts/

# 3) 部署 supervisor 与开机自启
scp rv1106_sender/run_rv1106_supervisor.sh \
    root@192.168.50.2:/root/userdata/ai_camera/
scp rv1106_sender/S95ai-camera root@192.168.50.2:/etc/init.d/
ssh root@192.168.50.2 \
    'chmod +x /root/userdata/ai_camera/run_rv1106_supervisor.sh /etc/init.d/S95ai-camera'

# 4) 系统网络配置（一次性）
#   /etc/network/interfaces: eth0 静态 192.168.50.2/24
#   /etc/dhcpcd.conf: denyinterfaces eth0
#   /oem/usr/bin/RkLunch.sh: 移除 udhcpc -i eth0
```

### 部署 ROCK 2A

```sh
# 1) 代码（仓库同步后）
cd /home/radxa/AI_CAMERA_LINUX/rock2a_receiver

# 2) 编译接收器
cmake --build build -j"$(nproc)"

# 3) 安装 systemd 服务与统一配置
sudo cp systemd/ai-camera.service /etc/systemd/system/
sudo install -m 0644 config/ai-camera.example.env /etc/ai-camera.env
sudo systemctl daemon-reload
sudo systemctl enable --now ai-camera.service

# 4) 模式文件（智能终端推荐 event；自动识别由事件服务调度）
echo event > /home/radxa/.config/ai_cam/mode

# 5) API Key（不进入仓库/systemd）
/home/radxa/.config/ai_cam/qwen.env
```

### 日常启动 / 停止 / 状态

```sh
# RV1106
/etc/init.d/S95ai-camera start     # 启动（开机自动执行）
/etc/init.d/S95ai-camera stop      # 停止（正常清理，不会自动拉起）
/etc/init.d/S95ai-camera status    # 查看状态
/etc/init.d/S95ai-camera restart   # 重启

# ROCK 2A
sudo systemctl start ai-camera     # 启动
sudo systemctl stop ai-camera      # 停止
sudo systemctl restart ai-camera   # 重启
systemctl status ai-camera         # 状态
```

端口：RV1106 `554`（RTSP）；ROCK 2A `9000`（TCP 回传）、`9001`（手动识别/旧保存）、
`9010`（NPU track 元数据）、`9011`（事件 API/metrics）。服务默认绑定有线地址。

### 故障恢复手册

| 故障 | 行为 | 需要人工操作？ |
| --- | --- | --- |
| 网线断开 / RV1106 重启 | ROCK 每 10 秒重试，链路恢复自动重连；HTTP/TCP 保持在线 | 否 |
| RTSP 接收器 / HTTP / TCP / watcher 异常退出 | supervisor 退避自动重启 | 否 |
| RV1106 可用内存 < 40MB 持续 30 秒 | 内存看门狗自动重启链路 | 否 |
| LCD 用户主动退出（Qt 42） | 两端正常停止，systemd/init 不自动拉起 | 重启：`S95 start` + `systemctl start ai-camera` |
| 连续 5 次启动失败 | 停止重启并写错误日志 | 排查后手动启动 |

日志：

```text
RV1106：  /root/userdata/ai_camera/logs/{supervisor,ai_cam,qt}.log
ROCK 2A： rock2a_receiver/runtime/logs/{supervisor,rtsp_receiver,manual_server,tcp_sender,auto_watcher,save_stats}.log
```

## 验收结果（2026-08-06/07）

### 2 小时稳定性（手动模式，110 个每分钟采样）

| 指标 | 结果 |
| --- | --- |
| Qt 预览 FPS | 平均 14.98（目标约 15） |
| 主 RTSP FPS | 平均 25.07（目标约 25） |
| 子 RTSP FPS | 平均 19.95（目标约 20） |
| RGA 错误 | 0 |
| 内存增长 | 四个进程 2h 增量 0.2%~2.7%，进入平台期，无持续增长 |
| 识别成功率 | 接口 15/15 + LCD 38 次，100% |
| request_id/frame_id 匹配率 | 100% |
| manual/auto 保存隔离 | 100% 正确 |
| 重复保存幂等 | 100% 返回 already_saved |
| 退出清理 | 两端进程/端口零残留 |

### 19 小时连续运行（1015 个每分钟采样）

| 指标 | 结果 |
| --- | --- |
| 帧率 | 预览 14.53 / 主 25.16 / 子 20.32，恒定 |
| RGA / 解码错误 | 0 / 0 |
| 服务在线 | RV supervisor + ROCK systemd 全程在线 |
| 内存看门狗 | 40.8MB -> 117MB 自动恢复一次，无 OOM |

### 异常恢复测试

| 场景 | 次数 | 结果 |
| --- | --- | --- |
| 断网恢复（eth0 down/up） | 5+1 | 全部自动恢复 |
| HTTP 9001 击杀 | 4 | 全部自动拉起 |
| TCP 9000 击杀 | 4 | 全部自动拉起 |
| RTSP 接收器击杀 | 4 | 全部自动拉起 |
| watcher 击杀 | 1 | 自动拉起且唯一 |
| 用户主动退出 | 3 | systemd 不拉起，手动恢复 |
| 启动顺序 A/B/C | 各验证 | 全部通过 |
| ROCK / RV 冷启动 | 多次 | 自启成功 |

验收目录与原始数据：`rock2a_receiver/runtime/acceptance/`（运行数据，不入库）。

## LCD 操作

Qt 界面以 16:9 显示 DMA-BUF 实时预览，提供以下触摸操作：

1. NPU confirmed track 唤醒 LCD；ROCK 2A 以事件策略低频识别最佳帧。
2. 点击“暂停”冻结当前显示帧；按钮变为“继续”，后台事件仍独立运行。
3. 点击“识别”，将冻结帧 JPEG 发送至 ROCK 2A；完成后显示中文结果与延迟。
4. 点击“保存结果”优先保存当前 `event_id`；没有事件时回退保存手动 `request_id`，重复保存不产生副本。
5. 点击“退出”后，3 秒内再次点击“确认退出”，由监督脚本依次正常停止 Qt、摄像头和联动 AI 管线。

保存文件仅存储在 ROCK 2A：

```text
/home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/saved_results/manual/
/home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/saved_results/auto/
/home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/ai_cam/saved_results/events/
```

对象事件设计见 [docs/event-model.md](docs/event-model.md)，接口见
[docs/event-api.md](docs/event-api.md)。硬件无关回归统一执行 `./scripts/test.sh`。

## 仓库规则

- 两端各自独立构建，不共享二进制产物。
- Qt 构建产物和中文字体文件不提交到 Git；部署脚本从 Ubuntu 系统字体目录复制字体。
- API Key 仅存放在 ROCK 2A 的 `~/.config/ai_cam/qwen.env`，不提交到 Git。
- 测试图片、识别 JSON、日志、虚拟环境和编译产物均被忽略。
- 板端运行数据（runtime/、artifacts/）不入库；验收报告如需归档请放入 docs/。
