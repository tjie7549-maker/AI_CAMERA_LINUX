# Camera daemon (stage 3)

`camera-daemon` is the sole owner of sensor-control requests.  It exposes a
newline-delimited JSON protocol on `/userdata/rv1106-smart-camera/run/camera-daemon.sock`
and writes append-only JSONL events beneath `/userdata`.

It deliberately supervises, rather than replaces, the validated existing
sender and NPU processes.  The sender keeps its RKMPI VI/VPSS/VENC/RTSP/RGA
implementation and its `/ai_cam_preview` contract; the existing detector keeps
using that preview.  This protects the AI path while the app is being migrated.

Internally the implementation separates the following responsibilities:
`CameraPipeline` is the sender process supervisor and restart owner;
`StreamManager` is the stream-health/status model; `NpuDetector` supervises the
unchanged detector process; `IlluminationAnalyzer` accepts frame metrics;
`LowLightPolicy` performs the threshold/hysteresis transitions; and
`CameraControl` is the only path that can issue `VIDIOC_S_CTRL`.  They are kept
behind the daemon's single event loop so the future Qt console remains a client,
never a second hardware owner.

Supported requests are `get_status`, `set_auto_ae`, `set_control`,
`report_metrics`, and `restart_pipeline`.  `set_control` accepts `exposure`,
`analogue_gain`, or `vblank`. Exposure and gain are rejected while `auto_ae` is
true, so RKAIQ remains the automatic exposure/gain owner.

The `sensor_subdev` default is `/dev/v4l-subdev2`, measured in the stage-1
baseline. It is configuration, not a topology assumption; validate it again
after changing kernel/DTS. Low-light transitions are metric-driven: an external
frame-metric producer should send `report_metrics` with luma, stream health and
NPU latency. The legacy preview header does not carry luminance, so stage 3 does
not fabricate a luma source; stage 4 will add the in-process metrics producer.

The daemon uses `poll(2)` with a one-second maintenance timeout, process groups
and bounded restart backoff. It has no high-frequency busy loop. It is not
deployed or auto-started in this stage.
