#ifndef PREVIEW_SHM_READER_H
#define PREVIEW_SHM_READER_H

#include <QObject>
#include <QImage>

class QTimer;

class PreviewShmReader : public QObject {
    Q_OBJECT

public:
    enum State {
        Offline = 0,
        Stale = 1,
        Online = 2,
    };

    explicit PreviewShmReader(QString name, int timeoutMs, QObject *parent = nullptr);
    ~PreviewShmReader() override;

    void start();
    void stop();

signals:
    void frameReady(const QImage &image, qulonglong frameId, qulonglong sourceTimeNs);
    void stateChanged(int state);
    void statsChanged(const QString &line1, const QString &line2);

private slots:
    void poll();

private:
    bool openMapping();
    bool receiveBufferFds();
    void closeMapping();
    void closeBuffers();
    void setState(State state);

    QString name_;
    int timeoutMs_;
    int fd_;
    void *mapping_;
    size_t mappingBytes_;
    int bufferFds_[2];
    void *bufferMappings_[2];
    size_t bufferBytes_;
    QTimer *timer_;
    State state_;
    qulonglong lastFrameId_;
    qulonglong statsStartNs_;
    unsigned int statsFrames_;
};

#endif
