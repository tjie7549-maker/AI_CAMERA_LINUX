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

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QImage image_;
    qulonglong frameId_;
    qulonglong sourceTimeNs_;
    int state_;
};

#endif
