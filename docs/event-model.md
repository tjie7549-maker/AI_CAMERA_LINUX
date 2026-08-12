# 对象事件模型

## 三层含义

- `detection`：模型对单帧给出的 person 框，不跨帧保持身份。
- `track`：RV1106 用 IoU 将连续帧检测框关联为临时 `track_id`。它不是人脸身份，
  也不能表示现实中的具体人员。
- `event`：ROCK 2A 将一个或多个 confirmed track 从出现到离开的连续过程归为
  一个 `event_id`，作为云端调用、持久化和界面展示的基本单位。

所有 bbox 均为预览图上的归一化 `x/y/w/h`，范围 `0..1`。YOLO 320x320
letterbox 输出先去除 padding，再反算到 384x216 预览坐标。

## 状态机

```text
无事件 --confirmed track--> active --全部消失--> ending
                              ^                    |
                              | grace 内重新出现   | grace 到期
                              +--------------------+--> ended

active/ending --检测流超时--> interrupted
```

首次出现生成 `event.new`；track 集合、人数、最佳帧、云端状态变化及每 5 秒状态刷新
生成 `event.update`；正常结束或检测流中断生成 `event.end`。grace 默认 10 秒，期间
重新出现对象沿用原事件。服务启动时数据库中未结束的旧事件标记为 `interrupted`，
陈旧检测消息不能创建事件。

## 最佳帧和帧匹配

ROCK 2A 的 RTSP 接收器每 500 ms 保存一张候选图，最多 24 张。事件服务按 NPU
消息接收时间选择最近候选，并综合检测置信度、框面积、亮度、灰度方差和画面差异
评分。胜出图提升到 `runtime/ai_cam/event_best/<event_id>.jpg`，不会随候选环淘汰。

RV1106 NPU 帧与 ROCK 2A 解码帧没有共同硬件时钟或帧序号，因此该关联始终标记
`frame_match=approximate`，同时保留 `best_frame_offset_ms`。这不是零误差同帧同步。

## 云端策略

事件创建后默认等待 3 秒，以取得更好画面；首次调用后，人数或显著画面变化至少
冷却 120 秒，长事件每 300 秒最多刷新一次。同一事件同时只有一个请求，失败最多
重试 2 次并退避。事件结束默认不补调云端。默认策略下，持续 10 分钟的单一事件
约调用 3 次，显著少于旧的 30 秒周期 20 次。

云端不可用不会阻止事件生成、结束、保存或本地背光值守。手动冻结帧识别继续走
HTTP 9001，独立于事件自动调度。

## 存储

`runtime/ai_cam/events.db` 使用 SQLite WAL、foreign keys、事务和 busy timeout：

- `events`：事件状态、时长、告警、摘要和最佳图路径。
- `tracks`：事件内历史 track、首次/最后出现和最高置信度。
- `recognitions`：以 `request_id` 幂等保存云端/手动识别元数据。
- `event_updates`：事件消息审计记录。

JPEG 只存文件系统路径，不写入 SQLite BLOB。
