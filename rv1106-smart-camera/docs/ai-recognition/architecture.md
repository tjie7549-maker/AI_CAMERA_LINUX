# 系统架构

## 数据流

```text
RV1106
SC3336 -> ISP/VI 2304x1296 -> VPSS
  |- ch0: 1296x1296 中心裁剪 -> 720x720 VO（兼容独立 LCD 模式）
  |- ch1: 1280x720 H.264 25 FPS -> RTSP /live/0
  |- ch2: 640x360 H.264 20 FPS -> RTSP /live/1
  `- ch3: 384x216 RGB888 DMA-BUF
       |- Qt 720x720 实时预览
       `- RKNN yolov5n -> bbox -> IoU tracker -> track.update TCP 9010
          `- confirmed track -> LCD 背光值守

ROCK 2A
RTSP /live/1 -> FFmpeg decode -> RGB24
  |- latest.jpg（旧功能兼容）
  `- 500ms/24张 JPEG ring -> 时间最近帧 -> 最佳帧

track.update -> npu_result_server -> event_service
  -> event.new/update/end -> SQLite events.db
  -> 事件策略 -> Qwen Vision -> recognition.result
  -> event_latest/event_recognition_latest -> TCP 9000 -> Qt
  -> HTTP 9011 查询、图片、保存、health、metrics
```

## 职责边界

RV1106 负责实时媒体管线、低延迟 LCD、端侧人形框、临时 track_id 和背光；不保存
长期事件，也不调用云端。ROCK 2A 负责事件状态、帧选择、云端调度、SQLite、文件
保存和 API。Qt 只展示预览/事件/语义结果并处理暂停、手动识别、保存和退出，关闭
Qt 不会停止事件生成。

## 对象事件

`detection` 是单帧框，`track` 是 IoU 跨帧关联，`event` 是一个或多个人从出现到
离开的连续过程。track_id 不是身份。状态为 `active -> ending -> ended`；grace 内
重新出现回到 active，检测流超时则 `interrupted`。详见
[event-model.md](event-model.md)。

## 云端与手动识别

生产默认由事件服务调度云端：首次短延迟、变化冷却 120 秒、长事件 300 秒刷新，
每个事件最多一个并发请求。Qt 的旧 30 秒周期默认关闭，只能通过兼容开关启用。
暂停后的冻结帧仍由 HTTP 9001 手动识别，不依赖事件自动调度。云端故障不影响事件、
RTSP、预览和背光。

## 帧同步边界

NPU 使用 RV1106 DMA-BUF 帧号；ROCK 使用独立 RTSP 解码帧号。两端没有共同帧同步，
只能按 epoch 接收时间选择最近 JPEG，记录偏差并标记 `frame_match=approximate`。

## 进程与恢复

RV1106 supervisor 管理媒体发送端、Qt 和 NPU；ROCK supervisor 管理 RTSP receiver、
event service、NPU receiver、HTTP 9001 和 TCP sender。组件异常采用有限退避重启。
事件服务启动会将数据库中的未结束事件标记为 interrupted；网络恢复后的新 track
生成新事件，不合并陈旧状态。

## 网络与端口

网线直连：RV1106 `192.168.50.2/24`，ROCK 2A `192.168.50.1/24`。ROCK 通过 Wi-Fi
访问千问 API。服务默认只绑定有线地址：

```text
554  RV1106 RTSP
9000 ROCK -> Qt NDJSON
9001 ROCK 手动识别/旧保存
9010 ROCK NPU track 接收
9011 ROCK 事件 API/metrics
```

消息和 HTTP 细节见 [event-api.md](event-api.md)，迁移/回退见
[migration.md](migration.md)，测试见 [testing.md](testing.md)。
