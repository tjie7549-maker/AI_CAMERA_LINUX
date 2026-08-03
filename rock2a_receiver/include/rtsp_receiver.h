#ifndef RTSP_RECEIVER_H
#define RTSP_RECEIVER_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

extern "C" {
struct AVCodec;
struct AVCodecContext;
struct AVDictionary;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
}

class RtspReceiver {
public:
    struct Config {
        std::string url = "rtsp://192.168.63.2:554/live/1";
        std::string output_dir = "./frames";
        int snapshot_interval_ms = 1000;
        std::string latest_image_path;
        int latest_interval_ms = 3000;
        int open_timeout_ms = 5000;
        int read_timeout_ms = 5000;
        int duration_seconds = 0;
        bool use_tcp = true;
        std::atomic_bool* external_stop = nullptr;
    };

    explicit RtspReceiver(Config config);
    ~RtspReceiver();

    bool open();
    int run();
    void requestStop();
    void close();

private:
    bool openInput();
    bool openDecoder();
    bool prepareConverter(const AVFrame* frame);
    bool processPacket(const AVPacket* packet);
    bool receiveDecodedFrames();
    bool processDecodedFrame(AVFrame* frame);
    bool saveSnapshot(const AVFrame* rgb_frame);
    bool updateLatestImage(const AVFrame* rgb_frame);
    bool encodeAndWriteJpeg(const AVFrame* rgb_frame,
                            const std::string& image_path,
                            bool atomic_replace);
    bool prepareJpegEncoder(int width, int height);
    void closeJpegEncoder();
    void printStreamInfo() const;
    void printStats(bool force = false);
    bool shouldStop() const;
    static int interruptCallback(void* opaque);
    static std::string errorString(int error_code);

    Config config_;
    std::atomic_bool stop_requested_{false};
    AVFormatContext* format_context_ = nullptr;
    AVCodecContext* decoder_context_ = nullptr;
    const AVCodec* decoder_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* decoded_frame_ = nullptr;
    AVFrame* rgb_frame_ = nullptr;
    SwsContext* rgb_sws_context_ = nullptr;
    std::vector<std::uint8_t> rgb_buffer_;
    int video_stream_index_ = -1;
    int source_width_ = 0;
    int source_height_ = 0;
    int source_format_ = -1;

    const AVCodec* jpeg_codec_ = nullptr;
    AVCodecContext* jpeg_context_ = nullptr;
    AVFrame* jpeg_frame_ = nullptr;
    AVPacket* jpeg_packet_ = nullptr;
    SwsContext* jpeg_sws_context_ = nullptr;
    int jpeg_width_ = 0;
    int jpeg_height_ = 0;
    std::int64_t jpeg_sequence_ = 0;

    std::chrono::steady_clock::time_point io_deadline_{};
    std::chrono::steady_clock::time_point run_started_{};
    std::chrono::steady_clock::time_point stats_started_{};
    std::chrono::steady_clock::time_point last_snapshot_{};
    bool has_snapshot_time_ = false;
    std::chrono::steady_clock::time_point last_latest_image_{};
    bool has_latest_image_time_ = false;
    std::uint64_t compressed_packets_ = 0;
    std::uint64_t decoded_frames_ = 0;
    std::uint64_t decoded_frames_at_last_stats_ = 0;
    std::uint64_t snapshots_ = 0;
    std::uint64_t read_errors_ = 0;
    std::uint64_t decode_errors_ = 0;
    std::int64_t last_pts_ = 0;
    std::int64_t last_best_effort_pts_ = 0;
    double last_pts_seconds_ = 0.0;
};

#endif
