# 千问视觉模块

本目录通过 OpenAI Python SDK 调用阿里云百炼的 OpenAI 兼容接口。运行前需在
`~/.config/ai_cam/qwen.env` 配置 `DASHSCOPE_API_KEY`、`DASHSCOPE_BASE_URL` 和
`QWEN_MODEL`；这些值不会写入源码、日志或 Git。

```sh
cd ~/rock2a_rtsp_receiver
python3 -m venv .venv-qwen
. .venv-qwen/bin/activate
python -m pip install --upgrade pip
pip install -r tools/qwen_vision/requirements.txt
```

固定图片验证：

```sh
. ~/.config/ai_cam/qwen.env
. .venv-qwen/bin/activate
python tools/qwen_vision/test_fixed_image.py --image /path/to/frame.jpg
```

实时监控只处理原子替换后的新图片，默认至少间隔五秒请求一次：

```sh
python tools/qwen_vision/watch_latest_image.py \
  --image /home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/ai_cam/latest.jpg \
  --result /home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime/ai_cam/latest_result.json \
  --interval-ms 5000
```

`latest_result.json` 也使用同目录临时文件写完、`fsync` 后再 `rename` 的方式更新。

生产智能终端不再使用固定间隔 watcher，而由以下轻量模块组成事件路径：

```text
npu_result_server.py -> event_protocol.py -> event_engine.py
  -> frame_cache.py + event_store.py -> event_service.py
```

`event_service.py` 按事件变化和冷却策略调用同一个 `QwenVisionClient`。watcher 仅保留
为无 Qt 兼容测试入口，不能与事件云端调度同时启用。接口和配置见仓库
`docs/event-api.md`、`docs/event-model.md`。
