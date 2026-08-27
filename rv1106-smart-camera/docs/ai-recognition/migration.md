# 迁移与回退

## 渐进启用

1. ROCK 2A 编译新版 RTSP 接收器，确认支持 `--frame-cache-dir`。
2. 将 `config/ai-camera.example.env` 复制到 `/etc/ai-camera.env` 并按需覆盖；
   systemd 和手动启动的 supervisor 都会读取。
3. 模式文件写入 `event`，由 supervisor 启动 receiver、event service、NPU server、
   手动服务和 TCP sender。
4. 更新 RV1106 `npu_detect`、Qt 和 supervisor。NPU 新消息保留旧 peopleCount 字段，
   因此两端可分步升级。
5. 观察 `/health`、`/metrics`、Qt 当前事件区域和 `events.db`。

## 旧索引导入

旧 `saved_results/index.jsonl` 不会自动修改。需要时显式执行：

```sh
python3 tools/qwen_vision/migrate_index_jsonl.py \
  --index runtime/ai_cam/saved_results/index.jsonl \
  --saved-root runtime/ai_cam/saved_results \
  --database runtime/ai_cam/events.db
```

导入只向 `recognitions` 增加 `backend=legacy` 记录，原图片和 index 保持不变；重复
`request_id` 由 SQLite 主键忽略。

## 回退

- Qt：设置 `ENABLE_LEGACY_AUTO_RECOGNITION=1` 恢复固定间隔自动识别；默认关闭。
- ROCK：模式改为 `manual` 可保留 HTTP 9001 和事件服务但不依赖旧 watcher；改为
  `auto` 仅用于无 Qt 的 headless watcher 测试，事件云端调用会关闭以防重复计费。
- NPU：ROCK 仍解析旧 peopleCount 消息；旧 NPU 不会产生 track/event。
- 数据：删除或移动新版程序不影响旧 `result_cache`、`saved_results` 和 `index.jsonl`。

不要同时启用事件自动云端、Qt 旧 30 秒周期和 headless watcher，否则会产生重复调用。
