#include "video_widget.h"

#include <QDebug>
#include <QPainter>

#include "preview_shm_reader.h"

VideoWidget::VideoWidget(QWidget *parent)
    : QWidget(parent),
      latestLiveFrameId_(0),
      latestLiveTimestampNs_(0),
      displayedLiveFrameId_(0),
      displayedLiveTimestampNs_(0),
      frozenFrameId_(0),
      frozenTimestampNs_(0),
      frozen_(false),
      state_(PreviewShmReader::Offline) {
    setObjectName(QStringLiteral("previewFrame"));
    setFixedSize(380, 214);
}

void VideoWidget::setFrame(const QImage &image, qulonglong frameId, qulonglong sourceTimeNs) {
    latestLiveImage_ = image;
    latestLiveFrameId_ = frameId;
    latestLiveTimestampNs_ = sourceTimeNs;
    if (!frozen_)
        update();
}

void VideoWidget::setState(int state) {
    state_ = state;
    update();
}

bool VideoWidget::freezeCurrentFrame() {
    if (displayedLiveImage_.isNull())
        return false;

    frozenImage_ = displayedLiveImage_.copy();
    frozenFrameId_ = displayedLiveFrameId_;
    frozenTimestampNs_ = displayedLiveTimestampNs_;
    frozen_ = true;
    qInfo("[Preview] frozen frame_id=%llu timestamp_ns=%llu",
          static_cast<unsigned long long>(frozenFrameId_),
          static_cast<unsigned long long>(frozenTimestampNs_));
    update();
    return true;
}

void VideoWidget::resumeLivePreview() {
    const qulonglong frameDelta =
        latestLiveFrameId_ >= frozenFrameId_ ? latestLiveFrameId_ - frozenFrameId_ : 0;
    frozen_ = false;
    qInfo("[Preview] resumed latest_frame_id=%llu frame_delta=%llu",
          static_cast<unsigned long long>(latestLiveFrameId_),
          static_cast<unsigned long long>(frameDelta));
    update();
}

bool VideoWidget::isFrozen() const {
    return frozen_;
}

QImage VideoWidget::frozenImageCopy() const {
    return frozenImage_.copy();
}

qulonglong VideoWidget::frozenFrameId() const {
    return frozenFrameId_;
}

qulonglong VideoWidget::frozenTimestampNs() const {
    return frozenTimestampNs_;
}

QImage VideoWidget::displayedLiveImageCopy() const {
    return displayedLiveImage_.copy();
}

qulonglong VideoWidget::displayedLiveFrameId() const {
    return displayedLiveFrameId_;
}

qulonglong VideoWidget::displayedLiveTimestampNs() const {
    return displayedLiveTimestampNs_;
}

void VideoWidget::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QPainter painter(this);
    const QRect bounds = rect().adjusted(1, 1, -1, -1);
    const QImage *image = frozen_ ? &frozenImage_ : &latestLiveImage_;

    painter.fillRect(bounds, QColor(QStringLiteral("#0b0f14")));
    if (!image->isNull()) {
        if (!frozen_) {
            displayedLiveImage_ = latestLiveImage_;
            displayedLiveFrameId_ = latestLiveFrameId_;
            displayedLiveTimestampNs_ = latestLiveTimestampNs_;
        }
        const QSize rendered = image->size().scaled(bounds.size(), Qt::KeepAspectRatio);
        const QRect target(
            QPoint((width() - rendered.width()) / 2, (height() - rendered.height()) / 2), rendered);
        painter.drawImage(target, *image);
    } else {
        painter.setPen(QColor(QStringLiteral("#d7e0e7")));
        QFont titleFont = painter.font();
        titleFont.setPointSize(18);
        titleFont.setBold(true);
        painter.setFont(titleFont);
        painter.drawText(bounds.adjusted(0, -14, 0, -2), Qt::AlignCenter,
                         QStringLiteral("摄像头预览"));
        painter.setPen(QColor(QStringLiteral("#7f909d")));
        titleFont.setPointSize(13);
        titleFont.setBold(false);
        painter.setFont(titleFont);
        const QString message = state_ == PreviewShmReader::Stale ? QStringLiteral("视频超时")
                                                                  : QStringLiteral("等待视频输入");
        painter.drawText(bounds.adjusted(0, 18, 0, 0), Qt::AlignCenter, message);
    }

    if (state_ != PreviewShmReader::Online) {
        painter.setPen(state_ == PreviewShmReader::Stale ? QColor(QStringLiteral("#f3b455"))
                                                         : QColor(QStringLiteral("#9aa8b5")));
        painter.drawText(bounds.adjusted(8, 6, -8, -6), Qt::AlignTop | Qt::AlignRight,
                         state_ == PreviewShmReader::Stale ? QStringLiteral("视频超时")
                                                           : QStringLiteral("视频离线"));
    }
}
