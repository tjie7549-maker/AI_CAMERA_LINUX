# 测试与验收

## 统一检查

```sh
cd /home/summary/linux/ai_cam
./scripts/test.sh
```

脚本执行 Python compileall、标准库 unittest、Shell 语法、主机 IoU tracker、RV1106
NPU/Qt 交叉构建，并在本机安装 FFmpeg 开发包时构建 ROCK C++ 接收器。开发机缺少
FFmpeg headers 时会明确跳过，需在 ROCK 2A 执行：

```sh
cd /home/radxa/AI_CAMERA_LINUX/rock2a_receiver
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

单元测试覆盖协议兼容、bbox 校验、事件 A/B/C/D/F、grace、陈旧消息、云端冷却、
最佳帧评分/提升、SQLite 幂等和重启、HTTP 查询/图片/保存、坏 JSON、超大请求与
目录穿越。C 测试覆盖 IoU、创建、确认、短暂漏检、删除、单调 ID 和双人靠近。

## 板端冒烟

1. 启动两端 supervisor，检查 ROCK 的 9000/9001/9010/9011 和 RV 的 554。
2. 进入画面，确认 Qt 显示同一事件 ID、人数、track 数和最佳帧。
3. 短暂遮挡后恢复，事件 ID 不变；离开超过 grace 后出现 `event.end`。
4. 再次进入生成新 ID；点击暂停和识别仍使用冻结帧。
5. 点击保存，检查 `runtime/ai_cam/saved_results/events/<event_id>/`，重复保存不新增目录。
6. 断开网线超过检测超时，旧事件标记 `interrupted`；恢复后新检测不能并入旧事件。
7. 停止云端访问，确认本地事件、NPU 值守、RTSP 和 Qt 预览继续运行。

当前系统只做接收时间最近帧匹配，必须在结果中保持 `approximate`；板端性能、真实
RTSP 恢复和 LCD 排版不能由无硬件单元测试替代。
