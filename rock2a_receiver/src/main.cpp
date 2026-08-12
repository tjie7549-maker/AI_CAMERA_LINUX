#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
}

#include "rtsp_receiver.h"

namespace {

std::atomic_bool g_stop{false};

void signalHandler(int) {
    g_stop.store(true);
}

void printUsage(const char* program) {
    std::cout << "Usage: " << program << " [options]\n"
              << "  --url URL              RTSP URL\n"
              << "  --output DIRECTORY     JPEG output directory\n"
              << "  --interval-ms NUMBER   Snapshot interval in milliseconds\n"
              << "  --latest-image PATH    Atomically updated latest JPEG (disabled by default)\n"
              << "  --latest-interval-ms NUMBER  Latest JPEG update interval (default: 3000)\n"
              << "  --frame-cache-dir PATH Bounded recent JPEG ring for event matching\n"
              << "  --frame-cache-interval-ms NUMBER  Ring interval (default: 500)\n"
              << "  --frame-cache-max NUMBER  Ring capacity (default: 24)\n"
              << "  --duration SECONDS     Stop after this many seconds (0 means Ctrl+C)\n"
              << "  --help                 Show this help\n";
}

int parsePositive(const std::string& value, const char* option, bool allow_zero) {
    try {
        const int parsed = std::stoi(value);
        if (parsed < 0 || (!allow_zero && parsed == 0)) {
            throw std::out_of_range("range");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("invalid value for ") + option + ": " + value);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    RtspReceiver::Config config;

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--help") {
                printUsage(argv[0]);
                return EXIT_SUCCESS;
            }
            if (index + 1 >= argc) {
                throw std::runtime_error("missing value for " + option);
            }
            const std::string value = argv[++index];
            if (option == "--url") {
                config.url = value;
            } else if (option == "--output") {
                config.output_dir = value;
            } else if (option == "--interval-ms") {
                config.snapshot_interval_ms = parsePositive(value, "--interval-ms", false);
            } else if (option == "--latest-image") {
                config.latest_image_path = value;
            } else if (option == "--latest-interval-ms") {
                config.latest_interval_ms = parsePositive(value, "--latest-interval-ms", false);
            } else if (option == "--frame-cache-dir") {
                config.frame_cache_dir = value;
            } else if (option == "--frame-cache-interval-ms") {
                config.frame_cache_interval_ms = parsePositive(value, "--frame-cache-interval-ms", false);
            } else if (option == "--frame-cache-max") {
                config.frame_cache_max = parsePositive(value, "--frame-cache-max", false);
            } else if (option == "--duration") {
                config.duration_seconds = parsePositive(value, "--duration", true);
            } else {
                throw std::runtime_error("unknown option: " + option);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "argument error: " << error.what() << '\n';
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    config.external_stop = &g_stop;
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    avformat_network_init();
    RtspReceiver receiver(config);
    const bool opened = receiver.open();
    const int result = opened ? receiver.run() : (g_stop.load() ? EXIT_SUCCESS : EXIT_FAILURE);
    receiver.close();
    avformat_network_deinit();
    return result;
}
