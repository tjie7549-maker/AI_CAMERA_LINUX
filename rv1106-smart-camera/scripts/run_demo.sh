#!/bin/sh
# 顶部说明：此 demo 运行期间独占 SC3336 图像传感器；只有在 camera-daemon 停止后才会恢复系统原有的 rkipc 服务。
# Ctrl+C 信号永远不会直接发送给 camera-daemon（通过 setsid 会话隔离机制实现）。

set -u  # 开启严格模式：使用未定义的变量时立即报错退出

# 设置项目根目录，若未配置环境变量 RV1106_SMART_CAMERA_ROOT 则使用默认路径 /userdata/rv1106-smart-camera
root=${RV1106_SMART_CAMERA_ROOT:-/userdata/rv1106-smart-camera}
daemon_pid=  # 记录后台守护进程 camera-daemon 的 PID
ui_pid=      # 记录前端 UI 进程 rv1106_ai_ui 的 PID
cleaned=0    # 清理标志位，防止重复触发 cleanup 导致二次清理

# 函数：轮询等待指定 PID 的进程优雅退出（最多等待 50 秒）
wait_for_exit() {
  target=$1
  count=0
  # 使用 kill -0 检测进程是否存活，若存活且未超时（<50秒），则循环等待
  while kill -0 "$target" 2>/dev/null && [ "$count" -lt 50 ]; do
    sleep 1
    count=$((count + 1))
  done
}

# 函数：启动后台守护进程 camera-daemon 并校验其就绪状态
start_daemon() {
  # 使用 setsid 创建独立会话组启动后台，隔离 Ctrl+C 信号；标准输出和标准错误追加重定向写入日志文件
  setsid "$root/bin/camera-daemon" "$root/config.json" >>"$root/logs/camera-daemon.log" 2>&1 &
  daemon_pid=$!  # 保存刚刚后台运行的 daemon 进程 PID
  
  # 最多轮询 8 秒，通过 Unix Domain Socket 发送 get_status 指令检查 daemon 是否成功就绪
  for n in 1 2 3 4 5 6 7 8; do
    "$root/bin/camera-daemon" --request "$root/run/camera-daemon.sock" '{"cmd":"get_status"}' >/dev/null 2>&1 && return 0
    sleep 1
  done
  return 1  # 超过 8 秒未响应，返回启动失败状态码 1
}

# 函数：资源清理与优雅退出流程（信号处理函数）
cleanup() {
  [ "$cleaned" -eq 0 ] || return  # 保证清理逻辑只被执行一次
  cleaned=1
  trap - EXIT INT TERM HUP        # 解绑信号，防止在 cleanup 执行过程中重复触发信号重入

  # 1. 优先关闭前端 UI 进程
  if [ -n "$ui_pid" ] && kill -0 "$ui_pid" 2>/dev/null; then
    kill -TERM "$ui_pid" 2>/dev/null || true  # 发送 SIGTERM 请求优雅关闭 UI
    wait_for_exit "$ui_pid"                   # 等待 UI 进程彻底退出
  fi

  # 2. 通知后台退出调试模式（这会释放媒体子进程并准备交还 rkipc）
  "$root/bin/camera-daemon" --request "$root/run/camera-daemon.sock" \
    '{"cmd":"exit_debug"}' >/dev/null 2>&1 || true

  # 3. 最后关闭后台守护进程
  if [ -n "$daemon_pid" ] && kill -0 "$daemon_pid" 2>/dev/null; then
    kill -TERM "$daemon_pid" 2>/dev/null || true  # 发送 SIGTERM 请求关闭 daemon
    wait_for_exit "$daemon_pid"                    # 等待 daemon 彻底退出以恢复硬件驱动
  fi
}

# 信号回调：捕获到终止信号时以状态码 130 退出（130 代表被 SIGINT 中断）
on_signal() { exit 130; }

# 注册信号捕捉：
# - 无论正常退出还是 exit 退出，均触发 cleanup 函数
# - 收到 INT(Ctrl+C)、TERM(kill)、HUP(终端断开) 时，先调 on_signal 退出，进而间接触发 EXIT 处的 cleanup
trap cleanup EXIT
trap on_signal INT TERM HUP

# 单例防重校验：检查系统中是否已有 camera-daemon 在运行，若有则拒绝重复启动
if pidof camera-daemon >/dev/null 2>&1; then
  echo "camera-daemon is already running; refuse a second demo instance." >&2
  exit 2
fi

# 启动后台守护进程；如果 8 秒内未就绪，打印错误日志并退出脚本
start_daemon || {
  echo "camera-daemon did not become ready; see $root/logs/camera-daemon.log" >&2
  exit 4
}

# 设置 Qt 环境变量（指定运行平台为 linuxfb，开启 DRM，配置触摸屏设备文件路径）
export QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/qt/plugins/platforms
export QT_QPA_PLATFORM=linuxfb
export QT_QPA_FB_DRM=1
export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=/dev/input/event0

# 在后台启动 Qt AI 应用界面，并传入守护进程的 Socket 路径
"$root/bin/rv1106_ai_ui" --daemon-socket "$root/run/camera-daemon.sock" &
ui_pid=$!  # 记录 UI 进程的 PID

# 主轮询与保活循环：以 UI 进程的存活状态（kill -0）作为循环条件
while kill -0 "$ui_pid" 2>/dev/null; do
  # 每 2 秒向 daemon 发送一次心跳请求 get_status
  if ! "$root/bin/camera-daemon" --request "$root/run/camera-daemon.sock" '{"cmd":"get_status"}' >/dev/null 2>&1; then
    # 如果心跳失败且校验 PID 发现 daemon 进程已崩溃死亡
    if ! pidof camera-daemon >/dev/null 2>&1; then
      echo "camera-daemon exited; restarting preview backend" >&2
      # 尝试自动拉起保活重启 camera-daemon
      start_daemon || echo "camera-daemon restart failed; will retry" >&2
    fi
  fi
  sleep 2
done

# UI 退出后，等待 UI 进程资源完全回收并重置 ui_pid 变量
wait "$ui_pid" || true
ui_pid=
# 脚本运行到最后一行正常结束，会自动触发 trap cleanup EXIT，顺序回收后台 daemon 资源