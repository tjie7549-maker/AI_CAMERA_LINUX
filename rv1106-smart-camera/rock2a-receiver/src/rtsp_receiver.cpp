#include "rtsp_receiver.h"

// ROCK 2A 的 RTSP 接收实现：解码 RV1106 子码流，周期性写入最新 JPEG，
// 并维护有界帧环，供事件服务将检测结果匹配到可识别图像。

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr std::chrono::seconds kStatsInterval{5};

std::string timestampForFilename() {
    const std::time_t now = std::time(nullptr);
    std::tm local_time{};
    localtime_r(&now, &local_time);
    std::ostringstream stream;
    stream << std::put_time(&local_time, "%Y%m%d_%H%M%S");
    return stream.str();
}

std::int64_t epochMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

RtspReceiver::RtspReceiver(Config config) : config_(std::move(config)) {
}

RtspReceiver::~RtspReceiver() {
    close();
}

bool RtspReceiver::open() {
    std::error_code filesystem_error;
    std::filesystem::create_directories(config_.output_dir, filesystem_error);
    if (filesystem_error) {
        std::cerr << "create output directory failed: " << filesystem_error.message() << '\n';
        return false;
    }
    if (!openInput() || !openDecoder()) {
        close();
        return false;
    }

    packet_ = av_packet_alloc();
    decoded_frame_ = av_frame_alloc();
    rgb_frame_ = av_frame_alloc();
    if (!packet_ || !decoded_frame_ || !rgb_frame_) {
        std::cerr << "allocate packet or frame failed\n";
        close();
        return false;
    }
    printStreamInfo();
    return true;
}

bool RtspReceiver::openInput() {
    AVDictionary* options = nullptr;
    format_context_ = avformat_alloc_context();
    if (!format_context_) {
        std::cerr << "allocate format context failed\n";
        return false;
    }
    format_context_->interrupt_callback.callback = interruptCallback;
    format_context_->interrupt_callback.opaque = this;

    if (config_.use_tcp) {
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
    }
    av_dict_set_int(&options, "stimeout", static_cast<std::int64_t>(config_.open_timeout_ms) * 1000,
                    0);
    av_dict_set_int(&options, "rw_timeout",
                    static_cast<std::int64_t>(config_.read_timeout_ms) * 1000, 0);

    io_deadline_ =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.open_timeout_ms);
    int result = avformat_open_input(&format_context_, config_.url.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (result < 0) {
        if (shouldStop()) {
            std::cerr << "RTSP opening interrupted by stop request\n";
        } else if (result == AVERROR_EXIT) {
            std::cerr << "open RTSP timeout after " << config_.open_timeout_ms << " ms\n";
        } else {
            std::cerr << "open RTSP failed: " << errorString(result) << '\n';
        }
        return false;
    }

    io_deadline_ =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.open_timeout_ms);
    result = avformat_find_stream_info(format_context_, nullptr);
    if (result < 0) {
        if (shouldStop()) {
            std::cerr << "RTSP stream probing interrupted by stop request\n";
        } else if (result == AVERROR_EXIT) {
            std::cerr << "find stream info timeout after " << config_.open_timeout_ms << " ms\n";
        } else {
            std::cerr << "find stream info failed: " << errorString(result) << '\n';
        }
        return false;
    }

    result = av_find_best_stream(format_context_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (result < 0) {
        std::cerr << "find video stream failed: " << errorString(result) << '\n';
        return false;
    }
    video_stream_index_ = result;
    return true;
}

bool RtspReceiver::openDecoder() {
    const AVStream* stream = format_context_->streams[video_stream_index_];
    decoder_ = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder_) {
        std::cerr << "find H.264 decoder failed\n";
        return false;
    }
    decoder_context_ = avcodec_alloc_context3(decoder_);
    if (!decoder_context_) {
        std::cerr << "allocate decoder context failed\n";
        return false;
    }
    int result = avcodec_parameters_to_context(decoder_context_, stream->codecpar);
    if (result < 0) {
        std::cerr << "copy codec parameters failed: " << errorString(result) << '\n';
        return false;
    }
    result = avcodec_open2(decoder_context_, decoder_, nullptr);
    if (result < 0) {
        std::cerr << "open decoder failed: " << errorString(result) << '\n';
        return false;
    }
    return true;
}

int RtspReceiver::run() {
    run_started_ = std::chrono::steady_clock::now();
    stats_started_ = run_started_;
    while (!shouldStop()) {
        const auto now = std::chrono::steady_clock::now();
        if (config_.duration_seconds > 0 &&
            now - run_started_ >= std::chrono::seconds(config_.duration_seconds)) {
            break;
        }

        io_deadline_ = now + std::chrono::milliseconds(config_.read_timeout_ms);
        const int result = av_read_frame(format_context_, packet_);
        if (result < 0) {
            if (shouldStop()) {
                break;
            }
            ++read_errors_;
            if (result == AVERROR_EXIT) {
                std::cerr << "read timeout after " << config_.read_timeout_ms << " ms\n";
            } else if (result == AVERROR_EOF) {
                std::cerr << "RTSP server disconnected\n";
            } else {
                std::cerr << "read frame failed: " << errorString(result) << '\n';
            }
            return 1;
        }

        if (packet_->stream_index == video_stream_index_) {
            ++compressed_packets_;
            processPacket(packet_);
        }
        av_packet_unref(packet_);
        printStats();
    }

    avcodec_send_packet(decoder_context_, nullptr);
    receiveDecodedFrames();
    printStats(true);
    return 0;
}

void RtspReceiver::requestStop() {
    stop_requested_.store(true);
}

bool RtspReceiver::processPacket(const AVPacket* packet) {
    int result = avcodec_send_packet(decoder_context_, packet);
    if (result == AVERROR(EAGAIN)) {
        receiveDecodedFrames();
        result = avcodec_send_packet(decoder_context_, packet);
    }
    if (result < 0) {
        ++decode_errors_;
        std::cerr << "send packet to decoder failed: " << errorString(result) << '\n';
        return false;
    }
    return receiveDecodedFrames();
}

bool RtspReceiver::receiveDecodedFrames() {
    while (true) {
        const int result = avcodec_receive_frame(decoder_context_, decoded_frame_);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return true;
        }
        if (result < 0) {
            ++decode_errors_;
            std::cerr << "receive decoded frame failed: " << errorString(result) << '\n';
            return false;
        }
        processDecodedFrame(decoded_frame_);
        av_frame_unref(decoded_frame_);
    }
}

bool RtspReceiver::processDecodedFrame(AVFrame* frame) {
    ++decoded_frames_;
    last_pts_ = frame->pts;
    last_best_effort_pts_ = frame->best_effort_timestamp;
    if (last_best_effort_pts_ != AV_NOPTS_VALUE) {
        const AVRational time_base = format_context_->streams[video_stream_index_]->time_base;
        last_pts_seconds_ = last_best_effort_pts_ * av_q2d(time_base);
    }
    if (!prepareConverter(frame)) {
        ++decode_errors_;
        return false;
    }

    sws_scale(rgb_sws_context_, frame->data, frame->linesize, 0, frame->height, rgb_frame_->data,
              rgb_frame_->linesize);

    const auto now = std::chrono::steady_clock::now();
    if (!has_snapshot_time_ ||
        now - last_snapshot_ >= std::chrono::milliseconds(config_.snapshot_interval_ms)) {
        if (saveSnapshot(rgb_frame_)) {
            ++snapshots_;
        }
        last_snapshot_ = now;
        has_snapshot_time_ = true;
    }

    if (!config_.latest_image_path.empty() &&
        (!has_latest_image_time_ ||
         now - last_latest_image_ >= std::chrono::milliseconds(config_.latest_interval_ms))) {
        updateLatestImage(rgb_frame_);
        last_latest_image_ = now;
        has_latest_image_time_ = true;
    }
    if (!config_.frame_cache_dir.empty() &&
        (!has_frame_cache_time_ ||
         now - last_frame_cache_ >= std::chrono::milliseconds(config_.frame_cache_interval_ms))) {
        saveFrameCache(rgb_frame_);
        last_frame_cache_ = now;
        has_frame_cache_time_ = true;
    }
    return true;
}

bool RtspReceiver::prepareConverter(const AVFrame* frame) {
    const int source_format = frame->format;
    if (source_width_ == frame->width && source_height_ == frame->height &&
        source_format_ == source_format && rgb_sws_context_) {
        return true;
    }

    rgb_sws_context_ = sws_getCachedContext(
        rgb_sws_context_, frame->width, frame->height, static_cast<AVPixelFormat>(source_format),
        frame->width, frame->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!rgb_sws_context_) {
        std::cerr << "create RGB converter failed\n";
        return false;
    }

    const int buffer_size =
        av_image_get_buffer_size(AV_PIX_FMT_RGB24, frame->width, frame->height, 1);
    if (buffer_size < 0) {
        std::cerr << "calculate RGB buffer size failed: " << errorString(buffer_size) << '\n';
        return false;
    }
    rgb_buffer_.assign(static_cast<std::size_t>(buffer_size), 0);
    av_frame_unref(rgb_frame_);
    rgb_frame_->format = AV_PIX_FMT_RGB24;
    rgb_frame_->width = frame->width;
    rgb_frame_->height = frame->height;
    const int result =
        av_image_fill_arrays(rgb_frame_->data, rgb_frame_->linesize, rgb_buffer_.data(),
                             AV_PIX_FMT_RGB24, frame->width, frame->height, 1);
    if (result < 0) {
        std::cerr << "prepare RGB frame failed: " << errorString(result) << '\n';
        return false;
    }
    source_width_ = frame->width;
    source_height_ = frame->height;
    source_format_ = source_format;
    const char* source_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(source_format));
    std::cout << "[decode] input_format=" << (source_name ? source_name : "unknown")
              << " output_format=rgb24 size=" << source_width_ << 'x' << source_height_ << '\n'
              << std::flush;
    return prepareJpegEncoder(frame->width, frame->height);
}

bool RtspReceiver::prepareJpegEncoder(int width, int height) {
    if (jpeg_context_ && jpeg_width_ == width && jpeg_height_ == height) {
        return true;
    }
    closeJpegEncoder();
    jpeg_codec_ = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!jpeg_codec_) {
        std::cerr << "find MJPEG encoder failed\n";
        return false;
    }
    jpeg_context_ = avcodec_alloc_context3(jpeg_codec_);
    jpeg_frame_ = av_frame_alloc();
    jpeg_packet_ = av_packet_alloc();
    if (!jpeg_context_ || !jpeg_frame_ || !jpeg_packet_) {
        std::cerr << "allocate JPEG resources failed\n";
        closeJpegEncoder();
        return false;
    }
    jpeg_context_->pix_fmt = AV_PIX_FMT_YUVJ420P;
    jpeg_context_->width = width;
    jpeg_context_->height = height;
    jpeg_context_->time_base = AVRational{1, 1};
    int result = avcodec_open2(jpeg_context_, jpeg_codec_, nullptr);
    if (result < 0) {
        std::cerr << "open MJPEG encoder failed: " << errorString(result) << '\n';
        closeJpegEncoder();
        return false;
    }
    jpeg_frame_->format = jpeg_context_->pix_fmt;
    jpeg_frame_->width = width;
    jpeg_frame_->height = height;
    result = av_frame_get_buffer(jpeg_frame_, 1);
    if (result < 0) {
        std::cerr << "allocate JPEG frame failed: " << errorString(result) << '\n';
        closeJpegEncoder();
        return false;
    }
    jpeg_sws_context_ =
        sws_getCachedContext(nullptr, width, height, AV_PIX_FMT_RGB24, width, height,
                             AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!jpeg_sws_context_) {
        std::cerr << "create JPEG converter failed\n";
        closeJpegEncoder();
        return false;
    }
    jpeg_width_ = width;
    jpeg_height_ = height;
    return true;
}

bool RtspReceiver::saveSnapshot(const AVFrame* rgb_frame) {
    std::ostringstream name;
    name << "frame_" << std::setw(6) << std::setfill('0') << (snapshots_ + 1) << '_'
         << timestampForFilename() << ".jpg";
    const std::filesystem::path output_path =
        std::filesystem::path(config_.output_dir) / name.str();
    return encodeAndWriteJpeg(rgb_frame, output_path.string(), false);
}

bool RtspReceiver::updateLatestImage(const AVFrame* rgb_frame) {
    return encodeAndWriteJpeg(rgb_frame, config_.latest_image_path, true);
}

bool RtspReceiver::saveFrameCache(const AVFrame* rgb_frame) {
    const std::int64_t captured_at_ms = epochMilliseconds();
    const std::filesystem::path directory(config_.frame_cache_dir);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;
    const std::string stem =
        "frame_" + std::to_string(captured_at_ms) + "_" + std::to_string(decoded_frames_);
    const std::filesystem::path image_path = directory / (stem + ".jpg");
    if (!encodeAndWriteJpeg(rgb_frame, image_path.string(), false))
        return false;

    double mean = 0.0, square = 0.0, difference = 1.0;
    std::uint64_t samples = 0;
    std::vector<std::uint8_t> signature;
    signature.reserve(static_cast<std::size_t>((rgb_frame->width + 7) / 8) *
                      static_cast<std::size_t>((rgb_frame->height + 7) / 8));
    for (int y = 0; y < rgb_frame->height; y += 8) {
        const std::uint8_t* row = rgb_frame->data[0] + y * rgb_frame->linesize[0];
        for (int x = 0; x < rgb_frame->width; x += 8) {
            const std::uint8_t* pixel = row + x * 3;
            const double gray = 0.299 * pixel[0] + 0.587 * pixel[1] + 0.114 * pixel[2];
            signature.push_back(static_cast<std::uint8_t>(std::lround(gray)));
            mean += gray;
            square += gray * gray;
            ++samples;
        }
    }
    if (samples) {
        mean /= samples;
        square = std::max(0.0, square / samples - mean * mean);
    }
    if (last_frame_signature_.size() == signature.size() && !signature.empty()) {
        double absolute_difference = 0.0;
        for (std::size_t index = 0; index < signature.size(); ++index)
            absolute_difference += std::abs(static_cast<int>(signature[index]) -
                                            static_cast<int>(last_frame_signature_[index]));
        difference = absolute_difference / (255.0 * static_cast<double>(signature.size()));
    }
    last_frame_signature_ = std::move(signature);
    std::ofstream metadata(directory / (stem + ".json"), std::ios::trunc);
    metadata << "{\"captured_at_ms\":" << captured_at_ms
             << ",\"receiver_frame_id\":" << decoded_frames_ << ",\"brightness\":" << mean
             << ",\"variance\":" << square << ",\"difference\":" << difference << "}\n";
    metadata.close();

    std::vector<std::filesystem::path> cached;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
        if (!error && entry.path().extension() == ".jpg")
            cached.push_back(entry.path());
    std::sort(cached.begin(), cached.end());
    while (cached.size() > static_cast<std::size_t>(config_.frame_cache_max)) {
        const std::filesystem::path old = cached.front();
        cached.erase(cached.begin());
        std::filesystem::remove(old, error);
        std::filesystem::remove(old.parent_path() / (old.stem().string() + ".json"), error);
    }
    return true;
}

bool RtspReceiver::encodeAndWriteJpeg(const AVFrame* rgb_frame, const std::string& image_path,
                                      bool atomic_replace) {
    if (!jpeg_context_ || !jpeg_frame_ || !jpeg_sws_context_) {
        std::cerr << "save JPEG failed: encoder is not ready\n";
        return false;
    }
    int result = av_frame_make_writable(jpeg_frame_);
    if (result < 0) {
        std::cerr << "make JPEG frame writable failed: " << errorString(result) << '\n';
        return false;
    }
    sws_scale(jpeg_sws_context_, rgb_frame->data, rgb_frame->linesize, 0, rgb_frame->height,
              jpeg_frame_->data, jpeg_frame_->linesize);
    jpeg_frame_->pts = jpeg_sequence_++;
    result = avcodec_send_frame(jpeg_context_, jpeg_frame_);
    if (result < 0) {
        std::cerr << "send JPEG frame failed: " << errorString(result) << '\n';
        return false;
    }
    result = avcodec_receive_packet(jpeg_context_, jpeg_packet_);
    if (result < 0) {
        std::cerr << "receive JPEG packet failed: " << errorString(result) << '\n';
        return false;
    }

    const std::filesystem::path output_path(image_path);
    const std::filesystem::path output_parent = output_path.parent_path();
    std::error_code filesystem_error;
    if (!output_parent.empty()) {
        std::filesystem::create_directories(output_parent, filesystem_error);
        if (filesystem_error) {
            std::cerr << "create JPEG directory failed: " << output_parent << '\n';
            av_packet_unref(jpeg_packet_);
            return false;
        }
    }
    const std::filesystem::path write_path =
        atomic_replace ? std::filesystem::path(image_path + ".tmp") : output_path;
    std::ofstream output(write_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "open JPEG output failed: " << write_path << '\n';
        av_packet_unref(jpeg_packet_);
        return false;
    }
    output.write(reinterpret_cast<const char*>(jpeg_packet_->data), jpeg_packet_->size);
    output.flush();
    const bool success = output.good();
    output.close();
    av_packet_unref(jpeg_packet_);
    if (!success) {
        std::cerr << "write JPEG failed: " << output_path << '\n';
        return false;
    }
    if (atomic_replace) {
        std::filesystem::rename(write_path, output_path, filesystem_error);
        if (filesystem_error) {
            std::cerr << "replace latest JPEG failed: " << output_path << '\n';
            return false;
        }
    }
    return true;
}

void RtspReceiver::printStreamInfo() const {
    const AVStream* stream = format_context_->streams[video_stream_index_];
    const AVCodecParameters* parameters = stream->codecpar;
    std::cout << "[stream] url=" << config_.url << '\n'
              << "[stream] codec=" << avcodec_get_name(parameters->codec_id)
              << " decoder=" << decoder_->name << '\n'
              << "[stream] size=" << parameters->width << 'x' << parameters->height << '\n'
              << "[stream] time_base=" << stream->time_base.num << '/' << stream->time_base.den
              << '\n'
              << "[stream] declared_fps=" << stream->r_frame_rate.num << '/'
              << stream->r_frame_rate.den << " average_fps=" << stream->avg_frame_rate.num << '/'
              << stream->avg_frame_rate.den << '\n'
              << std::flush;
}

void RtspReceiver::printStats(bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - stats_started_ < kStatsInterval) {
        return;
    }
    const double elapsed = std::chrono::duration<double>(now - stats_started_).count();
    const double fps =
        elapsed > 0.0
            ? static_cast<double>(decoded_frames_ - decoded_frames_at_last_stats_) / elapsed
            : 0.0;
    const double runtime = std::chrono::duration<double>(now - run_started_).count();
    std::cout << "[stats] packets=" << compressed_packets_ << " decoded=" << decoded_frames_
              << " fps=" << std::fixed << std::setprecision(2) << fps << " snapshots=" << snapshots_
              << " read_errors=" << read_errors_ << " decode_errors=" << decode_errors_
              << " runtime_s=" << runtime << " pts=" << last_pts_
              << " best_effort_pts=" << last_best_effort_pts_ << " pts_s=" << last_pts_seconds_
              << '\n'
              << std::flush;
    stats_started_ = now;
    decoded_frames_at_last_stats_ = decoded_frames_;
}

bool RtspReceiver::shouldStop() const {
    return stop_requested_.load() || (config_.external_stop && config_.external_stop->load());
}

int RtspReceiver::interruptCallback(void* opaque) {
    const auto* receiver = static_cast<RtspReceiver*>(opaque);
    if (receiver->shouldStop()) {
        return 1;
    }
    return std::chrono::steady_clock::now() > receiver->io_deadline_ ? 1 : 0;
}

std::string RtspReceiver::errorString(int error_code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error_code, buffer, sizeof(buffer));
    return buffer;
}

void RtspReceiver::closeJpegEncoder() {
    sws_freeContext(jpeg_sws_context_);
    jpeg_sws_context_ = nullptr;
    av_packet_free(&jpeg_packet_);
    av_frame_free(&jpeg_frame_);
    avcodec_free_context(&jpeg_context_);
    jpeg_codec_ = nullptr;
    jpeg_width_ = 0;
    jpeg_height_ = 0;
    jpeg_sequence_ = 0;
}

void RtspReceiver::close() {
    closeJpegEncoder();
    sws_freeContext(rgb_sws_context_);
    rgb_sws_context_ = nullptr;
    rgb_buffer_.clear();
    av_frame_free(&rgb_frame_);
    av_frame_free(&decoded_frame_);
    av_packet_free(&packet_);
    avcodec_free_context(&decoder_context_);
    if (format_context_) {
        avformat_close_input(&format_context_);
    }
    decoder_ = nullptr;
    video_stream_index_ = -1;
}
