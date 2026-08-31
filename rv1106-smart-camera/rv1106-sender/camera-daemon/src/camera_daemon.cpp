#include "camera_daemon.h"

// 相机守护进程实现。
// 负责独占相机所有权、启动/停止项目媒体子进程、恢复系统 rkipc，并向 Qt
// 暴露状态、AE 与 V4L2 参数控制。此文件中的 Impl 集中了运行期可变状态。

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

namespace {
static long long monotonic_ms() {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec * 1000LL + time.tv_nsec / 1000000LL;
}

static std::string json_escape(const std::string& value) {
    std::string escaped;
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '"' || value[index] == '\\')
            escaped += '\\';
        if (value[index] == '\n')
            escaped += "\\n";
        else
            escaped += value[index];
    }
    return escaped;
}

static bool field(const std::string& document, const char* name, std::string* value) {
    const std::string key = std::string("\"") + name + "\"";
    size_t position = document.find(key);
    if (position == std::string::npos)
        return false;

    position = document.find(':', position + key.size());
    if (position == std::string::npos)
        return false;
    position = document.find_first_not_of(" \t\r\n", position + 1);
    if (position == std::string::npos)
        return false;

    if (document[position] == '"') {
        const size_t end = document.find('"', position + 1);
        if (end == std::string::npos)
            return false;
        *value = document.substr(position + 1, end - position - 1);
        return true;
    }

    const size_t end = document.find_first_of(",}\r\n", position);
    *value = document.substr(position, end - position);
    return true;
}

static bool bool_field(const std::string& document, const char* name, bool* value) {
    std::string text;
    if (!field(document, name, &text))
        return false;
    *value = text == "true" || text == "1";
    return true;
}

static bool number_field(const std::string& document, const char* name, double* value) {
    std::string text;
    if (!field(document, name, &text))
        return false;
    char* end = 0;
    *value = strtod(text.c_str(), &end);
    return end && end != text.c_str();
}

static void mkdir_parent(const std::string& path) {
    size_t position = 0;
    while ((position = path.find('/', position + 1)) != std::string::npos) {
        const std::string directory = path.substr(0, position);
        if (!directory.empty())
            mkdir(directory.c_str(), 0755);
    }
}
}  // namespace

struct CameraDaemon::Impl {
    DaemonConfig c;
    int server_fd;
    pid_t sender_pid;
    pid_t npu_pid;
    pid_t rkipc_pid;
    bool running;
    bool npu_started_once;
    bool manual_restart_pending;
    /* V4L2 control 的回读值属于 sensor driver 缓存；RKAIQ 接管 AE 后，
     * 它不能可靠代表最近一次手动设置。因此在 daemon 内保存已成功下发的
     * 曝光/增益，后续修改其中一项时使用另一项的真实设定值。 */
    int manual_exposure, manual_gain;
    /* 在自动 AE/AGC 仍控制 sensor 时抓取的快照。这才是本次演示会话的
     * “恢复默认参数”，而不是写死的低亮度标定值。 */
    int baseline_exposure;
    int baseline_gain;
    int baseline_vblank;
    int baseline_hflip;
    int baseline_vflip;
    int baseline_test_pattern;
    int failures;
    int dark_frames;
    int bright_frames;
    int low_memory_count;
    long long restart_at;
    long long npu_start_at;
    long long last_memory_check_at;
    std::string state;
    std::string last_error;
    std::string mode;

    Impl(const DaemonConfig& config)
        : c(config),
          server_fd(-1),
          sender_pid(-1),
          npu_pid(-1),
          rkipc_pid(-1),
          running(true),
          npu_started_once(false),
          manual_restart_pending(false),
          manual_exposure(config.default_exposure),
          manual_gain(config.default_analogue_gain),
          baseline_exposure(config.default_exposure),
          baseline_gain(config.default_analogue_gain),
          baseline_vblank(config.default_vblank),
          baseline_hflip(config.default_hflip),
          baseline_vflip(config.default_vflip),
          baseline_test_pattern(config.default_test_pattern),
          failures(0),
          dark_frames(0),
          bright_frames(0),
          low_memory_count(0),
          restart_at(0),
          npu_start_at(0),
          last_memory_check_at(0),
          state("NORMAL"),
          mode("DISPLAY") {
    }
    void event(const char* type, const std::string& detail) {
        mkdir_parent(c.log_path);
        const int fd = open(c.log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0)
            return;
        std::ostringstream output;
        output << "{\"monotonic_ms\":" << monotonic_ms() << ",\"type\":\"" << type
               << "\",\"state\":\"" << state << "\",\"detail\":\"" << json_escape(detail)
               << "\"}\n";
        const std::string line = output.str();
        const ssize_t ignored = write(fd, line.data(), line.size());
        (void)ignored;
        close(fd);
    }
    bool spawn(const std::string& path, bool sender) {
        if (access(path.c_str(), X_OK) != 0) {
            last_error = "executable unavailable: " + path;
            event("spawn_failed", last_error);
            return false;
        }

        const pid_t child = fork();
        if (child < 0) {
            last_error = "fork failed";
            return false;
        }
        if (child == 0) {
            setsid();
            setenv("LD_LIBRARY_PATH", "/oem/usr/lib", 1);
            if (sender && path == c.bridge_path) {
                execl(path.c_str(), path.c_str(), "--url", c.rtsp_url.c_str(), (char*)0);
            } else if (sender) {
                execl(path.c_str(), path.c_str(), "--no-vo", "--preview-shm", "/ai_cam_preview",
                      "--preview-width", "384", "--preview-height", "216", "--preview-fps", "15",
                      "--isp-control-socket", c.isp_control_socket.c_str(), "-a", c.iq_dir.c_str(),
                      "-o", "/dev/null", (char*)0);
            } else {
                const std::string idle = std::to_string(c.backlight_idle_seconds);
                const std::string wake = std::to_string(c.backlight_wake_hits);
                if (c.backlight_control) {
                    execl(path.c_str(), path.c_str(), "--idle-seconds", idle.c_str(), "--wake-hits",
                          wake.c_str(), "--backlight-path", c.backlight_path.c_str(), (char*)0);
                } else {
                    execl(path.c_str(), path.c_str(), "--no-backlight-control", (char*)0);
                }
            }
            _exit(127);
        }
        if (sender)
            sender_pid = child;
        else
            npu_pid = child;
        event(sender ? "pipeline_started" : "npu_started", path);
        return true;
    }
    const std::string& capture_path() const {
        return c.sender_path;
    }
    bool rkipc_ready() const {
        return access(c.rkipc_socket.c_str(), F_OK) == 0;
    }
    pid_t find_rkipc() const {
        DIR* directory = opendir("/proc");
        if (!directory)
            return -1;

        pid_t found = -1;
        struct dirent* entry;
        while ((entry = readdir(directory)) != 0) {
            char* end = 0;
            const long value = strtol(entry->d_name, &end, 10);
            if (!end || *end || value < 1)
                continue;

            const std::string base = std::string("/proc/") + entry->d_name;
            const std::string comm_path = base + "/comm";
            const int comm_fd = open(comm_path.c_str(), O_RDONLY);
            char comm[32] = {0};
            const ssize_t comm_size = comm_fd < 0 ? -1 : read(comm_fd, comm, sizeof(comm) - 1);
            if (comm_fd >= 0)
                close(comm_fd);

            const std::string cmd_path = base + "/cmdline";
            const int fd = open(cmd_path.c_str(), O_RDONLY);
            char command[256] = {0};
            const ssize_t command_size = fd < 0 ? -1 : read(fd, command, sizeof(command) - 1);
            if (fd >= 0)
                close(fd);

            const bool name_matches = comm_size > 0 && strncmp(comm, "rkipc", 5) == 0;
            const bool command_matches = command_size > 0 && strstr(command, c.rkipc_path.c_str());
            if (name_matches || command_matches) {
                found = static_cast<pid_t>(value);
                break;
            }
        }
        closedir(directory);
        return found;
    }
    bool start_rkipc() {
        if (rkipc_ready())
            return true;

        unlink(c.rkipc_socket.c_str());
        const pid_t child = fork();
        if (child < 0) {
            last_error = "fork rkipc failed";
            return false;
        }
        if (child == 0) {
            setsid();
            setenv("LD_LIBRARY_PATH", "/oem/usr/lib", 1);
            execl(c.rkipc_path.c_str(), c.rkipc_path.c_str(), (char*)0);
            _exit(127);
        }

        rkipc_pid = child;
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (rkipc_ready()) {
                event("rkipc_started", c.rkipc_path);
                return true;
            }
            usleep(100000);
        }
        last_error = "rkipc did not expose its control socket";
        return false;
    }
    bool stop_rkipc() {
        for (int attempt = 0; attempt < 40; ++attempt) {
            const pid_t child = find_rkipc();
            if (child < 0) {
                unlink(c.rkipc_socket.c_str());
                rkipc_pid = -1;
                event("rkipc_stopped", "");
                return true;
            }
            kill(child, SIGTERM);
            usleep(100000);
        }
        event("rkipc_killed", "SIGTERM timeout; sent SIGKILL to rkipc only");
        for (int attempt = 0; attempt < 30; ++attempt) {
            const pid_t child = find_rkipc();
            if (child < 0) {
                unlink(c.rkipc_socket.c_str());
                rkipc_pid = -1;
                event("rkipc_stopped", "");
                return true;
            }
            kill(child, SIGKILL);
            usleep(100000);
        }
        /* 部分固件在 SIGKILL 后会短暂保留 /proc 僵尸项；后续 RKAIQ 就绪检查
         * 才是判断相机所有权是否真正释放的依据。 */
        unlink(c.rkipc_socket.c_str());
        event("rkipc_force_kill_pending", "continuing to RKAIQ readiness check");
        return true;
    }
    void start_npu_later() {
        npu_started_once = false;
        npu_start_at = c.start_npu ? monotonic_ms() + 1500 : 0;
    }

    bool start_capture() {
        return spawn(capture_path(), true);
    }

    void stop_capture() {
        kill_child(&sender_pid);
        kill_child(&npu_pid);
        npu_started_once = false;
        npu_start_at = 0;
        restart_at = 0;
    }
    void snapshot_auto_baseline() {
        const int exposure = read_control(V4L2_CID_EXPOSURE);
        const int gain = read_control(V4L2_CID_ANALOGUE_GAIN);
        const int vblank = read_control(V4L2_CID_VBLANK);
        const int hflip = read_control(V4L2_CID_HFLIP);
        const int vflip = read_control(V4L2_CID_VFLIP);
        const int test_pattern = read_control(V4L2_CID_TEST_PATTERN);
        if (exposure >= 1)
            baseline_exposure = exposure;
        if (gain >= 128)
            baseline_gain = gain;
        if (vblank >= 0)
            baseline_vblank = vblank;
        if (hflip >= 0)
            baseline_hflip = hflip;
        if (vflip >= 0)
            baseline_vflip = vflip;
        if (test_pattern >= 0)
            baseline_test_pattern = test_pattern;
        manual_exposure = baseline_exposure;
        manual_gain = baseline_gain;
        event("auto_baseline_saved", "exposure=" + std::to_string(baseline_exposure) +
                                         ", gain=" + std::to_string(baseline_gain));
    }
    bool enter_debug() {
        if (c.auto_ae)
            snapshot_auto_baseline();
        mode = "DEBUG";
        event("debug_entered", "native RKAIQ/SC3336 diagnostic mode");
        return true;
    }
    bool exit_debug() {
        if (mode == "DISPLAY")
            return true;
        /* 返回预览页不能重启 media-sender。部分 RKAIQ 版本会拒绝从 MANUAL AE
         * 切回 AUTO；旧逻辑会因此重启管线、替换 DMA-BUF，而 Qt 仍持有旧 FD，
         * 最终造成黑屏。因此恢复自动 AE 必须在参数页由用户显式执行。 */
        mode = "DISPLAY";
        event("debug_exited", c.auto_ae ? "returned to preview with auto AE"
                                        : "returned to preview with retained manual AE");
        return true;
    }
    void kill_child(pid_t* p) {
        if (*p <= 0)
            return;

        const pid_t child = *p;
        kill(-child, SIGTERM);
        kill(child, SIGTERM);
        for (int attempt = 0; attempt < 30; ++attempt) {
            int status = 0;
            const pid_t result = waitpid(child, &status, WNOHANG);
            if (result == child || (result < 0 && errno == ECHILD)) {
                *p = -1;
                return;
            }
            usleep(100000);
        }

        event("child_killed", "SIGTERM timeout; sent SIGKILL");
        kill(-child, SIGKILL);
        kill(child, SIGKILL);
        while (waitpid(child, 0, 0) < 0 && errno == EINTR) {
        }
        *p = -1;
    }
    void check_children() {
        int status = 0;
        pid_t child;
        while ((child = waitpid(-1, &status, WNOHANG)) > 0) {
            const bool sender_exited = child == sender_pid;
            if (sender_exited)
                sender_pid = -1;
            if (child == npu_pid)
                npu_pid = -1;

            if (sender_exited && running) {
                last_error = "pipeline exited";
                event("pipeline_exit", last_error);
                if (manual_restart_pending) {
                    manual_restart_pending = false;
                    failures = 0;
                    state = "RECOVERING";
                    restart_at = monotonic_ms() + 500;
                    event("restart_scheduled", "manual request");
                } else {
                    ++failures;
                    if (failures >= c.restart_after_failures) {
                        state = "ERROR";
                        restart_at = monotonic_ms() + c.restart_backoff_ms;
                        event("restart_scheduled", last_error);
                    }
                }
            }
        }
        if (c.start_npu && !npu_started_once && npu_start_at && monotonic_ms() >= npu_start_at) {
            npu_started_once = true;
            (void)spawn(c.npu_path, false);
        }
        if (sender_pid < 0 && restart_at && monotonic_ms() >= restart_at) {
            restart_at = 0;
            failures = 0;
            state = "RECOVERING";
            if (start_capture())
                state = "NORMAL";
        }
    }
    long memory_available_kb() const {
        std::ifstream file("/proc/meminfo");
        std::string key;
        std::string unit;
        long value = 0;
        while (file >> key >> value >> unit) {
            if (key == "MemAvailable:")
                return value;
        }
        return -1;
    }
    void check_memory_watchdog() {
        if (!c.memory_watchdog_enabled)
            return;
        const long long now = monotonic_ms();
        if (last_memory_check_at && now - last_memory_check_at < c.memory_check_interval_ms)
            return;
        last_memory_check_at = now;
        const long available = memory_available_kb();
        if (available < 0)
            return;
        if (available >= c.memory_available_min_kb) {
            low_memory_count = 0;
            return;
        }
        ++low_memory_count;
        event("memory_pressure", "MemAvailable=" + std::to_string(available) +
                                     "kB, samples=" + std::to_string(low_memory_count));
        if (low_memory_count < c.memory_low_checks)
            return;
        low_memory_count = 0;
        failures = 0;
        state = "RECOVERING";
        event("memory_recovery", "stopping project media chain before restart");
        stop_capture();
        restart_at = monotonic_ms() + c.restart_backoff_ms;
    }
    bool rkipc_write_all(int fd, const void* data, size_t bytes) {
        const char* p = (const char*)data;
        while (bytes) {
            ssize_t n = write(fd, p, bytes);
            if (n <= 0)
                return false;
            p += n;
            bytes -= (size_t)n;
        }
        return true;
    }
    bool rkipc_read_all(int fd, void* data, size_t bytes) {
        char* p = (char*)data;
        while (bytes) {
            ssize_t n = read(fd, p, bytes);
            if (n <= 0)
                return false;
            p += n;
            bytes -= (size_t)n;
        }
        return true;
    }
    bool rkipc_open(int* fd) {
        *fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (*fd < 0)
            return false;
        struct timeval timeout;
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;
        (void)setsockopt(*fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(*fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, c.rkipc_socket.c_str(), sizeof(a.sun_path) - 1);
        int hello = 0;
        if (connect(*fd, (struct sockaddr*)&a, sizeof(a)) < 0 ||
            !rkipc_read_all(*fd, &hello, sizeof(hello))) {
            close(*fd);
            *fd = -1;
            last_error = "rkipc IPC unavailable";
            return false;
        }
        return true;
    }
    bool rkipc_set_string(const char* method, const std::string& value) {
        int fd = -1, id = 0, result = -1, name_len = (int)strlen(method) + 1,
            value_len = (int)value.size() + 1;
        if (!rkipc_open(&fd) || !rkipc_write_all(fd, &name_len, sizeof(name_len)) ||
            !rkipc_write_all(fd, method, (size_t)name_len) ||
            !rkipc_write_all(fd, &id, sizeof(id)) ||
            !rkipc_write_all(fd, &value_len, sizeof(value_len)) ||
            !rkipc_write_all(fd, value.c_str(), (size_t)value_len) ||
            !rkipc_read_all(fd, &result, sizeof(result))) {
            if (fd >= 0)
                close(fd);
            last_error = "rkipc IPC request failed";
            return false;
        }
        close(fd);
        if (result) {
            last_error = "rkipc ISP rejected request";
            return false;
        }
        return true;
    }
    bool rkipc_set_gain(int raw_gain) {
        const char* method = "rk_isp_set_exposure_gain";
        int fd = -1, id = 0, result = -1, name_len = (int)strlen(method) + 1, gain = raw_gain / 128;
        if (gain < 1)
            gain = 1;
        if (!rkipc_open(&fd) || !rkipc_write_all(fd, &name_len, sizeof(name_len)) ||
            !rkipc_write_all(fd, method, (size_t)name_len) ||
            !rkipc_write_all(fd, &id, sizeof(id)) || !rkipc_write_all(fd, &gain, sizeof(gain)) ||
            !rkipc_read_all(fd, &result, sizeof(result))) {
            if (fd >= 0)
                close(fd);
            last_error = "rkipc gain request failed";
            return false;
        }
        close(fd);
        if (result) {
            last_error = "rkipc ISP rejected gain";
            return false;
        }
        return true;
    }
    bool rkipc_set_manual(int exposure, int gain) {
        return rkipc_set_string("rk_isp_set_exposure_mode", "manual") &&
               rkipc_set_string("rk_isp_set_gain_mode", "manual") &&
               rkipc_set_string("rk_isp_set_exposure_time",
                                std::to_string((double)exposure / 40800.0)) &&
               rkipc_set_gain(gain);
    }
    bool control(const std::string& id, int value) {
        if (mode != "DEBUG") {
            last_error = "参数调节仅在驱动调试模式可用";
            return false;
        }
        if (c.auto_ae && (id == "exposure" || id == "analogue_gain")) {
            last_error = "manual exposure/gain rejected while auto_ae=true";
            return false;
        }
        if (id == "exposure") {
            const int gain = manual_gain >= 128 ? manual_gain : c.default_analogue_gain;
            if (!isp("manual " + std::to_string(value) + " " + std::to_string(gain) + "\n"))
                return false;
            manual_exposure = value;
            manual_gain = gain;
            event("control_set", "exposure");
            return true;
        }
        if (id == "analogue_gain") {
            const int exposure = manual_exposure >= 1 ? manual_exposure : c.default_exposure;
            if (!isp("manual " + std::to_string(exposure) + " " + std::to_string(value) + "\n"))
                return false;
            manual_exposure = exposure;
            manual_gain = value;
            event("control_set", "analogue_gain");
            return true;
        }
        __u32 cid = 0;
        if (id == "vblank")
            cid = V4L2_CID_VBLANK;
        else if (id == "hflip")
            cid = V4L2_CID_HFLIP;
        else if (id == "vflip")
            cid = V4L2_CID_VFLIP;
        else if (id == "test_pattern")
            cid = V4L2_CID_TEST_PATTERN;
        else {
            last_error = "unsupported control";
            return false;
        }
        int fd = open(c.sensor_subdev.c_str(), O_RDWR);
        if (fd < 0) {
            last_error = "open sensor failed: " + std::string(strerror(errno));
            return false;
        }
        struct v4l2_control ctl;
        memset(&ctl, 0, sizeof(ctl));
        ctl.id = cid;
        ctl.value = value;
        int r = ioctl(fd, VIDIOC_S_CTRL, &ctl);
        close(fd);
        if (r < 0) {
            last_error = "VIDIOC_S_CTRL failed: " + std::string(strerror(errno));
            return false;
        }
        event("control_set", id);
        return true;
    }
    bool isp(const std::string& s) {
        int f = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, c.isp_control_socket.c_str(), sizeof(a.sun_path) - 1);
        if (f < 0 || connect(f, (struct sockaddr*)&a, sizeof(a)) < 0) {
            last_error = "RKAIQ control unavailable";
            if (f >= 0)
                close(f);
            return false;
        }
        write(f, s.data(), s.size());
        char b[8] = {0};
        int n = read(f, b, 7);
        close(f);
        return n > 1 && b[0] == 'o' && b[1] == 'k';
    }
    bool restore_defaults() {
        if (mode != "DEBUG") {
            last_error = "恢复默认参数仅在驱动调试模式可用";
            return false;
        }
        const __u32 ids[] = {V4L2_CID_EXPOSURE, V4L2_CID_ANALOGUE_GAIN, V4L2_CID_VBLANK,
                             V4L2_CID_HFLIP,    V4L2_CID_VFLIP,         V4L2_CID_TEST_PATTERN};
        const int values[] = {baseline_exposure, baseline_gain,  baseline_vblank,
                              baseline_hflip,    baseline_vflip, baseline_test_pattern};
        if (!isp("manual " + std::to_string(baseline_exposure) + " " +
                 std::to_string(baseline_gain) + "\n"))
            return false;
        int fd = open(c.sensor_subdev.c_str(), O_RDWR);
        if (fd < 0) {
            last_error = "open sensor failed: " + std::string(strerror(errno));
            return false;
        }
        for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
            if (ids[i] == V4L2_CID_EXPOSURE || ids[i] == V4L2_CID_ANALOGUE_GAIN)
                continue;
            struct v4l2_control ctl;
            memset(&ctl, 0, sizeof(ctl));
            ctl.id = ids[i];
            ctl.value = values[i];
            if (ioctl(fd, VIDIOC_S_CTRL, &ctl) < 0) {
                close(fd);
                last_error = "restore default failed: " + std::string(strerror(errno));
                return false;
            }
        }
        close(fd);
        manual_exposure = baseline_exposure;
        manual_gain = baseline_gain;
        c.auto_ae = false;
        last_error.clear();
        event("controls_restored", "captured automatic baseline, auto_ae=false");
        return true;
    }
    int read_control(__u32 cid) const {
        int fd = open(c.sensor_subdev.c_str(), O_RDONLY);
        if (fd < 0)
            return -1;
        struct v4l2_control ctl;
        memset(&ctl, 0, sizeof(ctl));
        ctl.id = cid;
        int result = ioctl(fd, VIDIOC_G_CTRL, &ctl);
        close(fd);
        return result == 0 ? ctl.value : -1;
    }
    std::string status() const {
        std::ostringstream o;
        o << "{\"ok\":true,\"state\":\"" << state << "\",\"mode\":\"" << mode
          << "\",\"auto_ae\":" << (c.auto_ae ? "true" : "false")
          << ",\"pipeline_pid\":" << sender_pid << ",\"npu_pid\":" << npu_pid
          << ",\"failures\":" << failures << ",\"last_error\":\"" << json_escape(last_error)
          << "\",\"controls\":{\"exposure\":"
          << (c.auto_ae ? read_control(V4L2_CID_EXPOSURE) : manual_exposure)
          << ",\"analogue_gain\":"
          << (c.auto_ae ? read_control(V4L2_CID_ANALOGUE_GAIN) : manual_gain)
          << ",\"vblank\":" << read_control(V4L2_CID_VBLANK)
          << ",\"hflip\":" << read_control(V4L2_CID_HFLIP)
          << ",\"vflip\":" << read_control(V4L2_CID_VFLIP)
          << ",\"test_pattern\":" << read_control(V4L2_CID_TEST_PATTERN)
          << "},\"watchdog\":{\"backlight_control\":" << (c.backlight_control ? "true" : "false")
          << ",\"backlight_idle_seconds\":" << c.backlight_idle_seconds
          << ",\"backlight_wake_hits\":" << c.backlight_wake_hits
          << ",\"memory_watchdog_enabled\":" << (c.memory_watchdog_enabled ? "true" : "false")
          << ",\"memory_available_kb\":" << memory_available_kb() << "}}";
        return o.str();
    }
};

CameraDaemon::CameraDaemon(const DaemonConfig& c) : impl_(new Impl(c)) {
}
CameraDaemon::~CameraDaemon() {
    stop();
    delete impl_;
}
bool CameraDaemon::start() {
    mkdir_parent(impl_->c.socket_path);
    unlink(impl_->c.socket_path.c_str());
    impl_->server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (impl_->server_fd < 0)
        return false;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, impl_->c.socket_path.c_str(), sizeof(a.sun_path) - 1);
    if (bind(impl_->server_fd, (struct sockaddr*)&a, sizeof(a)) < 0 ||
        listen(impl_->server_fd, 4) < 0) {
        close(impl_->server_fd);
        impl_->server_fd = -1;
        return false;
    }
    chmod(impl_->c.socket_path.c_str(), 0660);
    impl_->event("daemon_started", "socket ready");
    if (!impl_->stop_rkipc())
        return false;
    if (!impl_->start_capture())
        return false;
    impl_->start_npu_later();
    return true;
}
void CameraDaemon::stop() {
    if (!impl_ || !impl_->running)
        return;
    impl_->running = false;
    impl_->stop_capture();
    impl_->mode = "DISPLAY";
    (void)impl_->start_rkipc();
    if (impl_->server_fd >= 0) {
        close(impl_->server_fd);
        impl_->server_fd = -1;
    }
    unlink(impl_->c.socket_path.c_str());
    impl_->event("daemon_stopped", "");
}
void CameraDaemon::run() {
    while (impl_->running) {
        struct pollfd p = {impl_->server_fd, POLLIN, 0};
        int r = poll(&p, 1, 1000);
        impl_->check_children();
        impl_->check_memory_watchdog();
        if (r > 0 && (p.revents & POLLIN)) {
            int fd = accept(impl_->server_fd, 0, 0);
            if (fd >= 0) {
                char b[1025];
                ssize_t n = read(fd, b, 1024);
                if (n > 0) {
                    b[n] = 0;
                    std::string x = handle(b);
                    x += '\n';
                    ssize_t ignored = write(fd, x.data(), x.size());
                    (void)ignored;
                }
                close(fd);
            }
        }
    }
}
std::string CameraDaemon::handle(const std::string& r) {
    std::string cmd;
    if (!field(r, "cmd", &cmd))
        return "{\"ok\":false,\"error\":\"missing cmd\"}";
    if (cmd == "get_status")
        return impl_->status();
    if (cmd == "enter_debug")
        return impl_->enter_debug()
                   ? impl_->status()
                   : "{\"ok\":false,\"error\":\"" + json_escape(impl_->last_error) + "\"}";
    if (cmd == "exit_debug")
        return impl_->exit_debug()
                   ? impl_->status()
                   : "{\"ok\":false,\"error\":\"" + json_escape(impl_->last_error) + "\"}";
    if (cmd == "restart_pipeline") {
        if (impl_->sender_pid > 0) {
            kill(-impl_->sender_pid, SIGTERM);
            kill(impl_->sender_pid, SIGTERM);
            impl_->manual_restart_pending = true;
        } else {
            impl_->failures = 0;
            impl_->restart_at = monotonic_ms();
        }
        impl_->event("restart_requested", "");
        return "{\"ok\":true}";
    }
    if (cmd == "set_auto_ae") {
        if (impl_->mode != "DEBUG")
            return "{\"ok\":false,\"error\":\"请先进入驱动调试模式\"}";
        bool x;
        if (!bool_field(r, "auto_ae", &x))
            return "{\"ok\":false,\"error\":\"missing auto_ae\"}";
        if (!x && impl_->c.auto_ae)
            impl_->snapshot_auto_baseline();
        int exposure =
            impl_->manual_exposure >= 1 ? impl_->manual_exposure : impl_->baseline_exposure;
        int gain = impl_->manual_gain >= 128 ? impl_->manual_gain : impl_->baseline_gain;
        bool ok = x ? impl_->isp("auto\n")
                    : impl_->isp("manual " + std::to_string(exposure) + " " + std::to_string(gain) +
                                 "\n");
        if (!ok)
            return "{\"ok\":false,\"error\":\"" + json_escape(impl_->last_error) + "\"}";
        impl_->c.auto_ae = x;
        impl_->event("ae_mode", x ? "auto" : "manual");
        return impl_->status();
    }
    if (cmd == "set_control") {
        std::string id;
        double v;
        if (!field(r, "id", &id) || !number_field(r, "value", &v))
            return "{\"ok\":false,\"error\":\"id/value required\"}";
        return impl_->control(id, (int)v)
                   ? "{\"ok\":true}"
                   : "{\"ok\":false,\"error\":\"" + json_escape(impl_->last_error) + "\"}";
    }
    if (cmd == "restore_defaults")
        return impl_->restore_defaults()
                   ? impl_->status()
                   : "{\"ok\":false,\"error\":\"" + json_escape(impl_->last_error) + "\"}";
    if (cmd == "report_metrics") {
        double luma = 0, lat = 0;
        bool stream = true;
        number_field(r, "luma", &luma);
        number_field(r, "npu_latency_ms", &lat);
        bool_field(r, "stream_ok", &stream);
        if (!stream) {
            ++impl_->failures;
        }
        if (luma < impl_->c.low_light_luma) {
            ++impl_->dark_frames;
            impl_->bright_frames = 0;
            if (impl_->dark_frames >= impl_->c.low_light_frames && impl_->state == "NORMAL") {
                impl_->state = "LOW_LIGHT";
                impl_->event("low_light", "threshold reached");
            }
        } else {
            ++impl_->bright_frames;
            impl_->dark_frames = 0;
            if (impl_->state == "LOW_LIGHT" && impl_->bright_frames >= impl_->c.recover_frames) {
                impl_->state = "NORMAL";
                impl_->event("light_recovered", "");
            }
        }
        if (lat > impl_->c.npu_latency_max_ms)
            impl_->event("npu_latency_high", "threshold exceeded");
        return impl_->status();
    }
    return "{\"ok\":false,\"error\":\"unknown cmd\"}";
}
bool CameraDaemon::load_config(const std::string& path, DaemonConfig* c, std::string* e) {
    std::ifstream f(path.c_str());
    if (!f) {
        *e = "cannot open";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str(), v;
    c->socket_path = "/userdata/rv1106-smart-camera/run/camera-daemon.sock";
    c->log_path = "/userdata/rv1106-smart-camera/logs/events.jsonl";
    c->sensor_subdev = "/dev/v4l-subdev2";
    c->sender_path = "/userdata/rv1106-smart-camera/bin/media-sender";
    c->bridge_path = "/userdata/rv1106-smart-camera/bin/rtsp-preview-bridge";
    c->npu_path = "/userdata/npu_detect/npu_detect";
    c->iq_dir = "/oem/usr/share/iqfiles";
    c->isp_control_socket = "/tmp/rv1106_isp_control.sock";
    c->rkipc_path = "/oem/usr/bin/rkipc";
    c->rkipc_socket = "/var/tmp/rkipc";
    c->rtsp_url = "rtsp://127.0.0.1/live/0";
    c->backlight_path = "/sys/class/backlight/backlight/bl_power";
    c->restart_after_failures = 3;
    c->restart_backoff_ms = 3000;
    c->low_light_frames = 15;
    c->recover_frames = 30;
    c->backlight_idle_seconds = 30;
    c->backlight_wake_hits = 3;
    c->memory_available_min_kb = 40960;
    c->memory_low_checks = 3;
    c->memory_check_interval_ms = 10000;
    c->low_light_luma = 45;
    c->npu_latency_max_ms = 150;
    c->default_exposure = 128;
    c->default_analogue_gain = 128;
    c->default_vblank = 64;
    c->default_hflip = 0;
    c->default_vflip = 0;
    c->default_test_pattern = 0;
    c->start_pipeline = true;
    c->start_npu = true;
    c->auto_ae = true;
    c->backlight_control = true;
    c->memory_watchdog_enabled = true;
#define STR(k, m)        \
    if (field(s, k, &v)) \
    c->m = v
#define NUM(k, m)                   \
    {                               \
        double x;                   \
        if (number_field(s, k, &x)) \
            c->m = (int)x;          \
    }
#define DBL(k, m)                   \
    {                               \
        double x;                   \
        if (number_field(s, k, &x)) \
            c->m = x;               \
    }
#define BOL(k, m)                 \
    {                             \
        bool x;                   \
        if (bool_field(s, k, &x)) \
            c->m = x;             \
    }
    STR("socket_path", socket_path);
    STR("log_path", log_path);
    STR("sensor_subdev", sensor_subdev);
    STR("sender_path", sender_path);
    STR("bridge_path", bridge_path);
    STR("npu_path", npu_path);
    STR("iq_dir", iq_dir);
    STR("isp_control_socket", isp_control_socket);
    STR("rkipc_path", rkipc_path);
    STR("rkipc_socket", rkipc_socket);
    STR("rtsp_url", rtsp_url);
    STR("backlight_path", backlight_path);
    NUM("restart_after_failures", restart_after_failures);
    NUM("restart_backoff_ms", restart_backoff_ms);
    NUM("low_light_frames", low_light_frames);
    NUM("recover_frames", recover_frames);
    NUM("backlight_idle_seconds", backlight_idle_seconds);
    NUM("backlight_wake_hits", backlight_wake_hits);
    NUM("memory_available_min_kb", memory_available_min_kb);
    NUM("memory_low_checks", memory_low_checks);
    NUM("memory_check_interval_ms", memory_check_interval_ms);
    NUM("default_exposure", default_exposure);
    NUM("default_analogue_gain", default_analogue_gain);
    NUM("default_vblank", default_vblank);
    NUM("default_hflip", default_hflip);
    NUM("default_vflip", default_vflip);
    NUM("default_test_pattern", default_test_pattern);
    DBL("low_light_luma", low_light_luma);
    DBL("npu_latency_max_ms", npu_latency_max_ms);
    BOL("start_pipeline", start_pipeline);
    BOL("start_npu", start_npu);
    BOL("auto_ae", auto_ae);
    BOL("backlight_control", backlight_control);
    BOL("memory_watchdog_enabled", memory_watchdog_enabled);
#undef STR
#undef NUM
#undef DBL
#undef BOL
    if (c->restart_after_failures < 1 || c->low_light_frames < 1 || c->recover_frames < 1 ||
        c->backlight_idle_seconds < 1 || c->backlight_wake_hits < 1 ||
        c->memory_available_min_kb < 1 || c->memory_low_checks < 1 ||
        c->memory_check_interval_ms < 1) {
        *e = "thresholds must be positive";
        return false;
    }
    return true;
}
