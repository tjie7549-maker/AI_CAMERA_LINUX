#include "preview_shm_reader.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <QDebug>
#include <QTimer>
#include <cstring>

#include "preview_shm_protocol.h"

namespace {
qulonglong monotonicNs() {
    timespec value = {};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return static_cast<qulonglong>(value.tv_sec) * 1000000000ULL +
           static_cast<qulonglong>(value.tv_nsec);
}
}  // namespace

PreviewShmReader::PreviewShmReader(QString name, int timeoutMs, QObject *parent)
    : QObject(parent),
      name_(std::move(name)),
      timeoutMs_(timeoutMs),
      fd_(-1),
      mapping_(nullptr),
      mappingBytes_(0),
      bufferFds_{-1, -1},
      bufferMappings_{nullptr, nullptr},
      bufferBytes_(0),
      timer_(new QTimer(this)),
      state_(Offline),
      lastFrameId_(0),
      statsStartNs_(0),
      statsFrames_(0) {
    // 50 ms 轮询元数据；超时阈值由调用方决定何时将预览标为离线。
    connect(timer_, &QTimer::timeout, this, &PreviewShmReader::poll);
    timer_->setInterval(50);
}

PreviewShmReader::~PreviewShmReader() {
    stop();
}

void PreviewShmReader::start() {
    timer_->start();
    poll();
}

void PreviewShmReader::stop() {
    timer_->stop();
    closeBuffers();
    closeMapping();
    setState(Offline);
}

bool PreviewShmReader::openMapping() {
    struct stat status = {};
    PreviewShmHeader *header;

    fd_ = shm_open(name_.toLocal8Bit().constData(), O_RDONLY, 0);
    if (fd_ < 0)
        return false;
    if (fstat(fd_, &status) != 0 || status.st_size < static_cast<off_t>(sizeof(PreviewShmHeader))) {
        closeMapping();
        return false;
    }
    mappingBytes_ = sizeof(PreviewShmHeader);
    mapping_ = mmap(nullptr, mappingBytes_, PROT_READ, MAP_SHARED, fd_, 0);
    if (mapping_ == MAP_FAILED) {
        mapping_ = nullptr;
        closeMapping();
        return false;
    }
    header = static_cast<PreviewShmHeader *>(mapping_);
    if (header->magic != PREVIEW_SHM_MAGIC || header->version != PREVIEW_SHM_VERSION ||
        header->header_size != sizeof(PreviewShmHeader) ||
        header->pixel_format != PREVIEW_SHM_PIXFMT_RGB888 ||
        header->buffer_count != PREVIEW_SHM_BUFFER_COUNT || header->width == 0 ||
        header->height == 0 || header->stride < header->width * 3U) {
        closeMapping();
        return false;
    }
    bufferBytes_ = static_cast<size_t>(header->height) * header->stride;
    return true;
}

bool PreviewShmReader::receiveBufferFds() {
    sockaddr_un address = {};
    PreviewFdMessage message = {};
    iovec iov = {};
    msghdr header = {};
    char control[CMSG_SPACE(sizeof(bufferFds_))] = {};
    int socketFd;
    ssize_t received;
    cmsghdr *cmsg;
    int receivedFds[2] = {-1, -1};

    socketFd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (socketFd < 0)
        return false;
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, PREVIEW_FD_SOCKET_PATH, sizeof(address.sun_path) - 1);
    if (::connect(socketFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(socketFd);
        return false;
    }
    iov.iov_base = &message;
    iov.iov_len = sizeof(message);
    header.msg_iov = &iov;
    header.msg_iovlen = 1;
    header.msg_control = control;
    header.msg_controllen = sizeof(control);
    received = recvmsg(socketFd, &header, 0);
    close(socketFd);
    if (received != static_cast<ssize_t>(sizeof(message)) ||
        message.magic != PREVIEW_FD_MESSAGE_MAGIC || message.version != PREVIEW_SHM_VERSION ||
        message.width == 0 || message.height == 0 || message.stride < message.width * 3U ||
        message.pixel_format != PREVIEW_SHM_PIXFMT_RGB888 ||
        message.buffer_count != PREVIEW_SHM_BUFFER_COUNT) {
        return false;
    }
    cmsg = CMSG_FIRSTHDR(&header);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len < CMSG_LEN(sizeof(receivedFds))) {
        return false;
    }
    std::memcpy(receivedFds, CMSG_DATA(cmsg), sizeof(receivedFds));
    for (int index = 0; index < 2; ++index) {
        bufferMappings_[index] =
            mmap(nullptr, bufferBytes_, PROT_READ, MAP_SHARED, receivedFds[index], 0);
        if (bufferMappings_[index] == MAP_FAILED) {
            bufferMappings_[index] = nullptr;
            close(receivedFds[index]);
            receivedFds[index] = -1;
            closeBuffers();
            return false;
        }
        bufferFds_[index] = receivedFds[index];
    }
    qInfo("#Preview: received %u DMA-BUF frames (%ux%u RGB888)", message.buffer_count,
          message.width, message.height);
    return true;
}

void PreviewShmReader::closeMapping() {
    if (mapping_) {
        munmap(mapping_, mappingBytes_);
        mapping_ = nullptr;
        mappingBytes_ = 0;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void PreviewShmReader::closeBuffers() {
    for (int index = 0; index < 2; ++index) {
        if (bufferMappings_[index]) {
            munmap(bufferMappings_[index], bufferBytes_);
            bufferMappings_[index] = nullptr;
        }
        if (bufferFds_[index] >= 0) {
            close(bufferFds_[index]);
            bufferFds_[index] = -1;
        }
    }
    bufferBytes_ = 0;
}

void PreviewShmReader::setState(State state) {
    if (state_ == state)
        return;
    state_ = state;
    const char *name = state_ == Online ? "online" : (state_ == Stale ? "stale" : "offline");
    qInfo("#Preview: state=%s", name);
    emit stateChanged(static_cast<int>(state_));
}

void PreviewShmReader::poll() {
    PreviewShmHeader *header;
    qulonglong nowNs;
    uint32_t before;
    uint32_t after;
    uint32_t activeIndex;
    qulonglong frameId;
    qulonglong sourceTimeNs;

    if (!mapping_ && !openMapping()) {
        setState(Offline);
        return;
    }
    if (!bufferMappings_[0] && !receiveBufferFds()) {
        setState(Offline);
        return;
    }
    header = static_cast<PreviewShmHeader *>(mapping_);
    nowNs = monotonicNs();
    if (__atomic_load_n(&header->producer_online, __ATOMIC_ACQUIRE) == 0U) {
        setState(Offline);
        return;
    }
    sourceTimeNs = __atomic_load_n(&header->last_frame_monotonic_ns, __ATOMIC_ACQUIRE);
    if (sourceTimeNs == 0 || nowNs < sourceTimeNs ||
        nowNs - sourceTimeNs > static_cast<qulonglong>(timeoutMs_) * 1000000ULL) {
        setState(Stale);
        return;
    }
    setState(Online);
    frameId = __atomic_load_n(&header->last_frame_id, __ATOMIC_ACQUIRE);
    if (frameId == lastFrameId_)
        return;

    before = __atomic_load_n(&header->sequence, __ATOMIC_ACQUIRE);
    if (before & 1U)
        return;
    activeIndex = __atomic_load_n(&header->active_index, __ATOMIC_ACQUIRE);
    if (activeIndex >= PREVIEW_SHM_BUFFER_COUNT)
        return;
    QImage image(static_cast<int>(header->width), static_cast<int>(header->height),
                 QImage::Format_RGB888);
    const unsigned char *source = static_cast<const unsigned char *>(bufferMappings_[activeIndex]);
    for (uint32_t row = 0; row < header->height; ++row) {
        std::memcpy(image.scanLine(static_cast<int>(row)),
                    source + static_cast<size_t>(row) * header->stride,
                    static_cast<size_t>(header->width) * 3U);
    }
    after = __atomic_load_n(&header->sequence, __ATOMIC_ACQUIRE);
    if (before != after || (after & 1U))
        return;

    lastFrameId_ = frameId;
    if (statsStartNs_ == 0)
        statsStartNs_ = nowNs;
    statsFrames_++;
    if (nowNs - statsStartNs_ >= 1000000000ULL) {
        const double seconds = static_cast<double>(nowNs - statsStartNs_) / 1000000000.0;
        const double fps = static_cast<double>(statsFrames_) / seconds;

        qInfo("#Preview: Qt display FPS=%.2f, frame=%llu", fps,
              static_cast<unsigned long long>(frameId));
        emit statsChanged(QStringLiteral("%1 x %2   预览 %3 FPS")
                              .arg(header->width)
                              .arg(header->height)
                              .arg(fps, 0, 'f', 1),
                          QStringLiteral("帧 %1   源时间 %2 s")
                              .arg(frameId)
                              .arg(static_cast<double>(sourceTimeNs) / 1000000000.0, 0, 'f', 2));
        statsStartNs_ = nowNs;
        statsFrames_ = 0;
    }
    emit frameReady(image, frameId, sourceTimeNs);
}
