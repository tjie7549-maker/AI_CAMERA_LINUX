# 待确认问题

以下信息无法从宿主机源码静态审计中可靠得出，不阻塞阶段 1，但进入阶段 2 前必须确认。

1. 板端实际加载的 DTB 是普通 `rv1106g-luckfox-pico-ultra.dts`、fastboot 版本，还是定制 DTB？
   - 确认方法：保存 boot log、`cat /proc/device-tree/model`、compatible，以及固件/BoardConfig 的实际 DTB 选择。
2. 实际摄像头实体是否确认为 `sc3336`，而不是 fastboot DTS 中的 SC3338？
   - 确认方法：`dmesg`、`media-ctl -p`、sensor subdev 名称。
3. 六项标准 V4L2 controls 暴露在哪个 subdev，RKAIQ 运行时是否允许外部设置？
   - 确认方法：对 topology 中 sensor subdev 执行 `v4l2-ctl --list-ctrls-menus`，分别在 RKAIQ 自动模式和停止状态测试。
4. Ultra 与 Ultra W 是否使用完全相同的 camera GPIO/MCLK/供电连接？
   - 确认方法：实际原理图、板级 DTS 选择和板卡版本；不能从名称推断。
5. 当前生产启动链路由 `rkipc`、`rv1106_sender` 还是其他 supervisor 独占媒体节点？
   - 确认方法：`ps`、打开文件描述符和现有启动脚本；冒烟抓帧前需有序释放。
6. 驱动统计选择 debugfs 还是 sysfs？
   - 建议先确认生产镜像是否挂载 debugfs。统计只读且偏调试，优先 debugfs；若秋招演示镜像不启用 debugfs，再评估只读 sysfs device attributes。
7. 现有 `ai_cam` 没有顶层 `luckfox/` 目录。是否另有未放入当前工作区的原始应用？
   - 若有，应在阶段 3 前提供路径，以免重复实现或遗漏已验证模块。

