#ifndef VIDEO_WIDGET_H
#define VIDEO_WIDGET_H

#include <QImage>
#include <QWidget>

class VideoWidget : public QWidget {
    Q_OBJECT

   public:
    explicit VideoWidget(QWidget *parent = nullptr);

   public slots:
    void setFrame(const QImage &image, qulonglong frameId, qulonglong sourceTimeNs);
    void setState(int state);

   public:
    bool freezeCurrentFrame();
    void resumeLivePreview();
    bool isFrozen() const;
    QImage frozenImageCopy() const;
    qulonglong frozenFrameId() const;
    qulonglong frozenTimestampNs() const;
    QImage displayedLiveImageCopy() const;
    qulonglong displayedLiveFrameId() const;
    qulonglong displayedLiveTimestampNs() const;

   protected:
    void paintEvent(QPaintEvent *event) override;

   private:
    QImage latestLiveImage_;
    QImage displayedLiveImage_;
    QImage frozenImage_;
    qulonglong latestLiveFrameId_;
    qulonglong latestLiveTimestampNs_;
    qulonglong displayedLiveFrameId_;
    qulonglong displayedLiveTimestampNs_;
    qulonglong frozenFrameId_;
    qulonglong frozenTimestampNs_;
    bool frozen_;
    int state_;
};

#endif
