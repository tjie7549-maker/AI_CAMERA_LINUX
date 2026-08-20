# 阶段 2：SC3336 驱动可靠性补丁

补丁文件：`kernel-patches/0001-media-sc3336-harden-stream-errors-and-add-stats.patch`

基线提交：`824b817f889c2cbff1d48fcdb18ab494a68f69d1`  
独立分支：`feature/sc3336-reliability-stats-v2`  
补丁提交：`24085062c`

## 改动范围

仅修改 `sysdrv/source/kernel/drivers/media/i2c/sc3336.c`。

1. 保留负的 I2C errno；短传输才归一为 `-EIO`。
2. 记录 I2C 读写数、错误数及最后一次 I2C 错误。
3. 多寄存器增益、曝光、VBLANK 和翻转操作遇到首个错误立即返回，不再用 `ret |=` 合并 errno 或在读失败后继续写。
4. `s_stream()` 只在成功启动/停止后改变 `streaming`，并记录最后启动耗时；stop 失败仍释放 PM 引用，但保留 streaming 状态供上层重试。
5. 检查 thunderboot power-on 错误；修正 `s_power()` 初始化失败时的 runtime-PM put 配对。
6. 新增只读 debugfs：`/sys/kernel/debug/sc3336-4-0030/stats`（目录名中的 `4-0030` 随实际 I2C 设备名变化）。

示例输出：

```text
i2c_reads: 0
i2c_writes: 0
i2c_errors: 0
last_i2c_error: 0
last_stream_start_us: 0
streaming: 0
```

此接口没有寄存器读写入口，不包含曝光策略、亮度策略或人脸识别逻辑。

## 校验

- `checkpatch.pl --strict`：0 errors、0 warnings、0 checks。
- `git apply --check`：通过，针对未修改的 SDK 基线提交。
- 尚未完整编译或烧录：当前 SDK 主工作树含用户修改，且本轮不在该工作树构建或安装内核。

## 应用、构建与回滚

必须在 SDK 的干净、完整独立 worktree 中操作，不能在当前含用户修改的 SDK 主目录直接应用：

```sh
git am /path/to/0001-media-sc3336-harden-stream-errors-and-add-stats.patch
cd /home/summary/linux/luckfox-pico
./build.sh kernel
```

SDK 的构建脚本会根据当前板级配置调用 `make kernel -C sysdrv`。构建完成后，先保留原始可启动镜像与 DTB，再按项目既有烧录流程仅烧录新内核镜像；本补丁不含 DTS 变更。

回滚方式：在该独立分支执行 `git revert 24085062c` 后重新构建镜像并按相同流程烧录原版本，或直接恢复事先备份的已验证镜像。不要使用 `git reset --hard` 清理用户工作树。

## 板端验证清单

1. 挂载 debugfs（如尚未挂载）：`mount -t debugfs none /sys/kernel/debug`。
2. 启动/停止原 sender 后读取 `stats`，确认计数和 `streaming` 合理变化。
3. 在停止 RKAIQ 自动 AE/AGC 或启用后续 daemon 的手动调试模式时，逐项验证 exposure、analogue gain、VBLANK、H/V flip、test pattern。
4. 恢复原 sender，重复 LCD、720P/360P RTSP、RKNN 识别与阶段 1 单帧冒烟测试。
5. I2C 失败和长稳结论均标记为待真机故障注入/长测验证；不能伪造数据。

