# Qt 调试控制台（阶段 4）

控制台由既有 `rv1106_ai_ui` 最小扩展而来，继续读取现有的
`/ai_cam_preview` DMA-BUF 预览，也继续保留原来的识别与结果保存页面。
主界面新增“设备调试”入口；对话框包含状态、相机控制、驱动状态、事件日志四页。

它只使用 `QLocalSocket` 向
`/userdata/rv1106-smart-camera/run/camera-daemon.sock` 请求 JSON，绝不打开
`/dev/v4l-subdev*`、不写寄存器。daemon 不存在、摄像头未启动或 control 不支持时，
错误会显示在对话框事件区，界面不会退出。

自动 AE/AGC 默认开启，曝光、模拟增益、VBLANK 帧率调试、水平/垂直翻转和测试图的
输入框均不可操作；仅在用户切换为手动调试后，输入框才可用，具体写控制仍由 daemon
仲裁。VBLANK 是本板已测传感器的可写时序控制，不把它伪装成直接的 FPS 控制。

当前 legacy preview 协议没有传递亮度和 NPU 时延，驱动 debugfs 统计也尚未接入 daemon
响应；页面会明确标为“未上报/待接入”，而不是展示虚构数据。板端 UI 交互、触摸、
Socket 和控制实际写入均留待阶段 5 真机测试。
