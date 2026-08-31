#ifndef RV1106_CAMERA_DAEMON_H
#define RV1106_CAMERA_DAEMON_H

// 相机守护进程的公共接口。
// 它是 RV1106 上唯一的相机控制仲裁者：切换 rkipc 与项目媒体管线、监管子进程，
// 并通过 Unix Socket 向 Qt 提供状态查询和调参接口。

#include <map>
#include <string>

struct DaemonConfig {
    // 文件、可执行文件和设备路径；可由 config.json 覆盖以适配不同板端环境。
    std::string socket_path, log_path, sensor_subdev, sender_path, bridge_path, npu_path, iq_dir,
        isp_control_socket, rkipc_path, rkipc_socket, rtsp_url, backlight_path;
    // 进程重启、低照度判定和内存看门狗阈值；时间字段以毫秒计。
    int restart_after_failures, restart_backoff_ms, low_light_frames, recover_frames;
    int backlight_idle_seconds, backlight_wake_hits, memory_available_min_kb, memory_low_checks,
        memory_check_interval_ms;
    int default_exposure, default_analogue_gain, default_vblank, default_hflip, default_vflip,
        default_test_pattern;
    // 传感器控制默认值，以及低照度/NPU 延迟阈值。
    double low_light_luma, npu_latency_max_ms;
    // 启动媒体/NPU、自动曝光、背光控制和内存监控的功能开关。
    bool start_pipeline, start_npu, auto_ae, backlight_control, memory_watchdog_enabled;
};

class CameraDaemon {
   public:
    explicit CameraDaemon(const DaemonConfig& config);
    ~CameraDaemon();
    bool start();
    void run();
    void stop();
    // 处理一条 JSON Socket 请求，并返回 JSON 响应。
    std::string handle(const std::string& request);
    // 加载配置并做基础范围校验；失败原因写入 error。
    static bool load_config(const std::string& path, DaemonConfig* config, std::string* error);

   private:
    struct Impl;
    Impl* impl_;
};

#endif
