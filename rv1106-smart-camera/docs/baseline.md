# 阶段 1：板端基线与 V4L2 冒烟测试

## 1. 部署

仅把本项目的 `scripts/` 复制到板端可写目录：

```sh
mkdir -p /userdata/rv1106-smart-camera/scripts
# 使用 scp 或其他现有部署方式复制两个脚本
chmod +x /userdata/rv1106-smart-camera/scripts/*.sh
```

脚本不会修改系统目录，不会自动停止 `rkipc` 或现有 AI camera 进程，也不会写 sensor control。若摄像头正被占用，查询通常仍可执行，但抓帧可能返回 `Device or resource busy`；应按项目原有流程有序停止 owner，测试后再恢复。

## 2. 收集只读基线

默认结果保存到带 UTC 时间戳的目录：

```sh
/userdata/rv1106-smart-camera/scripts/camera_baseline.sh
```

也可指定输出根目录：

```sh
/userdata/rv1106-smart-camera/scripts/camera_baseline.sh \
  /userdata/rv1106-smart-camera/baseline
```

输出包括：内核版本、设备树 model/compatible、相关 dmesg、每个 media topology、V4L2 设备清单、每个 video/subdev 节点的驱动/格式/controls，以及 sysfs 节点名称和 driver 链接。命令失败会保留错误文本与退出码，不会把失败伪装成成功。

建议将整个时间戳目录拷回宿主机并保留为阶段 2 的审查证据。结果可能包含板卡标识、网络或产品信息，对外展示前应人工脱敏。

## 3. 冒烟测试

优先显式指定从 media topology 确认的 capture 节点：

```sh
/userdata/rv1106-smart-camera/scripts/v4l2_smoke_test.sh \
  -d /dev/videoN \
  -o /userdata/rv1106-smart-camera/tests/smoke-001
```

也可通过指定 media device 让脚本寻找常见 Rockchip 主采集实体：

```sh
/userdata/rv1106-smart-camera/scripts/v4l2_smoke_test.sh -m /dev/media0
```

不传 `-d/-m` 时，脚本遍历 `/dev/media*`，仅接受 topology 中名称匹配 `rkisp_mainpath`、`mainpath`、`stream_cif_mipi_id0` 或 `scale_ch0` 且 capability 为 capture 的节点。无法可靠判断时会停止并要求传 `-d`，不会写死或猜测 `/dev/videoX`。

测试内容：

1. `v4l2-ctl --all`
2. `v4l2-ctl --list-ctrls-menus`
3. 当前 video format
4. mmap 抓取一帧到 `frame.raw`；该命令完成一次 STREAMON、取帧和 STREAMOFF

`frame.raw` 只用于确认采集路径，不能直接宣称画质正确。须依据 `format.txt` 中的像素格式、宽高和 stride 用匹配工具检查，且不得提交抓拍数据。

## 4. 缺失依赖

若镜像没有 `v4l2-ctl` 或 `media-ctl`，脚本会明确退出并打印 Buildroot 建议。SDK 对应配置为：

```text
BR2_PACKAGE_LIBV4L=y
BR2_PACKAGE_LIBV4L_UTILS=y
```

启用工具需要 C++11 工具链；当前固定环境满足该前提。重新制作镜像属于后续板端操作，本阶段不自动修改 Buildroot 配置或烧录镜像。

## 5. 完成判据

- 基线目录完整生成，失败项有明确退出码。
- topology 中能确认 SC3336→DPHY→CSI/CIF→ISP 的实际链路。
- control 清单能确认六项目标 control 暴露在哪个节点。
- `frame.raw` 非空且日志显示 stream 命令退出码为 0。
- 如任一项未完成，在测试记录中标为“待验证/失败”，保留原始日志。

