#include "camera_daemon.h"
#include <signal.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static CameraDaemon* g_daemon = 0;
static void on_signal(int) { if (g_daemon) g_daemon->stop(); }

static int request(const char* socketPath, const char* json) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return 4;
  struct sockaddr_un address; memset(&address, 0, sizeof(address)); address.sun_family = AF_UNIX;
  if (strlen(socketPath) >= sizeof(address.sun_path)) { close(fd); return 4; }
  strcpy(address.sun_path, socketPath);
  if (connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) { perror("camera-daemon request connect"); close(fd); return 4; }
  std::string line(json);
  line += '\n';
  if (write(fd, line.data(), line.size()) < 0) { close(fd); return 4; }
  char buffer[1024]; ssize_t count = read(fd, buffer, sizeof(buffer) - 1); close(fd);
  if (count <= 0)
    return 4;
  buffer[count] = 0;
  fputs(buffer, stdout);
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 4 && strcmp(argv[1], "--request") == 0)
    return request(argv[2], argv[3]);
  if (argc == 3 && strcmp(argv[1], "--validate-config") == 0) {
    DaemonConfig config;
    std::string error;
    if (!CameraDaemon::load_config(argv[2], &config, &error)) {
      fprintf(stderr, "camera-daemon: config %s: %s\n", argv[2], error.c_str());
      return 2;
    }
    printf("config valid: %s\n", argv[2]);
    return 0;
  }
  const char* config_path = argc > 1 ? argv[1] : "/userdata/rv1106-smart-camera/config.json";
  DaemonConfig config;
  std::string error;
  if (!CameraDaemon::load_config(config_path, &config, &error)) {
    fprintf(stderr, "camera-daemon: config %s: %s\n", config_path, error.c_str());
    return 2;
  }
  CameraDaemon daemon(config);
  g_daemon = &daemon;
  signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
  if (!daemon.start()) return 3;
  daemon.run();
  g_daemon = 0;
  return 0;
}
