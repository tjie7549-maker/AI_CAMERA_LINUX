#ifndef RV1106_CAMERA_DAEMON_H
#define RV1106_CAMERA_DAEMON_H

#include <map>
#include <string>

struct DaemonConfig {
  std::string socket_path, log_path, sensor_subdev, sender_path, bridge_path, npu_path, iq_dir, isp_control_socket, rkipc_path, rkipc_socket, rtsp_url, backlight_path;
  int restart_after_failures, restart_backoff_ms, low_light_frames, recover_frames;
  int backlight_idle_seconds, backlight_wake_hits, memory_available_min_kb, memory_low_checks, memory_check_interval_ms;
  int default_exposure, default_analogue_gain, default_vblank, default_hflip, default_vflip, default_test_pattern;
  double low_light_luma, npu_latency_max_ms;
  bool start_pipeline, start_npu, auto_ae, backlight_control, memory_watchdog_enabled;
};

class CameraDaemon {
 public:
  explicit CameraDaemon(const DaemonConfig& config);
  ~CameraDaemon();
  bool start();
  void run();
  void stop();
  std::string handle(const std::string& request);
  static bool load_config(const std::string& path, DaemonConfig* config, std::string* error);
 private:
  struct Impl;
  Impl* impl_;
};

#endif
