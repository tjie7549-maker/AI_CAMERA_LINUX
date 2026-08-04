#include "video_widget.h"

#include <QPainter>

#include "preview_shm_reader.h"

VideoWidget::VideoWidget(QWidget *parent)
    : QWidget(parent),
      frameId_(0),
      sourceTimeNs_(0),
      state_(PreviewShmReader::Offline)
{
    setObjectName(QStringLiteral("previewFrame"));
    setFixedSize(380, 214);
}

void VideoWidget::setFrame(const QImage &image, qulonglong frameId,
                           qulonglong sourceTimeNs)
{
    image_ = image;
    frameId_ = frameId;
    sourceTimeNs_ = sourceTimeNs;
    update();
}

void VideoWidget::setState(int state)
{
    state_ = state;
    update();
}

void VideoWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    const QRect bounds = rect().adjusted(1, 1, -1, -1);

    painter.fillRect(bounds, QColor(QStringLiteral("#0b0f14")));
    if (!image_.isNull()) {
        const QSize rendered = image_.size().scaled(bounds.size(), Qt::KeepAspectRatio);
        const QRect target(QPoint((width() - rendered.width()) / 2,
                                  (height() - rendered.height()) / 2), rendered);
        painter.drawImage(target, image_);
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
        const QString message = state_ == PreviewShmReader::Stale
                                    ? QStringLiteral("视频超时")
                                    : QStringLiteral("等待视频输入");
        painter.drawText(bounds.adjusted(0, 18, 0, 0), Qt::AlignCenter, message);
    }

    if (state_ != PreviewShmReader::Online) {
        painter.setPen(state_ == PreviewShmReader::Stale
                           ? QColor(QStringLiteral("#f3b455"))
                           : QColor(QStringLiteral("#9aa8b5")));
        painter.drawText(bounds.adjusted(8, 6, -8, -6), Qt::AlignTop | Qt::AlignRight,
                         state_ == PreviewShmReader::Stale
                             ? QStringLiteral("视频超时")
                             : QStringLiteral("视频离线"));
    }
}
