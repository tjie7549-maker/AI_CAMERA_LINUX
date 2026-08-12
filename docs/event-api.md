# 事件协议与 API

## NDJSON 消息

TCP 9000 和 RV1106 到 ROCK 2A 的检测链路使用每行一个 JSON。版本 1 envelope：

```json
{
  "schema_version": 1,
  "message_type": "event.update",
  "camera_id": "rv1106-01",
  "source": "local_npu",
  "frame_id": 12345,
  "captured_at_ms": 1720000000000,
  "produced_at_ms": 1720000000040,
  "request_id": "",
  "event_id": "evt_rv1106-01_1720000000040_a1b2c3",
  "tracks": [],
  "result": {
    "event_state": "active",
    "current_people": 1,
    "max_people": 2,
    "track_count": 1,
    "best_frame_id": 12345,
    "best_frame_offset_ms": 86,
    "frame_match": "approximate",
    "cloud_state": "complete"
  },
  "health": {}
}
```

支持 `detection`、`track.update`、`event.new`、`event.update`、`event.end`、
`recognition.result` 和 `health`。未知字段忽略；未知 schema、坏 JSON、超过 64 KiB
或不安全 ID 会被拒绝，且不会覆盖上一条有效状态。旧 `type=npu/peopleCount` 仍可
用于 Qt 值守展示，但没有 bbox 时不能创建对象事件。

## HTTP 9011

默认仅绑定 `192.168.50.1`：

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/health` | 事件服务和云端开关状态 |
| GET | `/events?limit=20&before=<ms>&state=active` | 倒序查询事件 |
| GET | `/events/<event_id>` | 事件、tracks 和 recognitions |
| GET | `/events/<event_id>/image` | 最佳 JPEG |
| POST | `/events/<event_id>/save` | 幂等保存事件图和 JSON |
| POST | `/ingest` | NPU 服务内部提交 track 消息 |
| GET | `/metrics` | Prometheus 文本指标 |

手动识别兼容接口继续由 HTTP 9001 提供：`POST /recognize`、
`POST /save-result`、`GET /health`。事件自动识别与手动冻结帧识别互不替代。

`/metrics` 包含事件数、活动 track、NPU 有效/无效消息、云端请求/失败/延迟、
检测到事件延迟、RTSP 帧、最佳帧更新、磁盘可用空间和服务运行时间。

## 端口

```text
RV1106 554   RTSP 主/子码流
ROCK 2A 9000 统一结果 NDJSON -> Qt
ROCK 2A 9001 手动识别和旧结果保存
ROCK 2A 9010 RV1106 NPU track 输入
ROCK 2A 9011 事件 API、内部 ingest 和 metrics
```
