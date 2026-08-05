#ifndef MANUAL_RECOGNITION_CLIENT_H
#define MANUAL_RECOGNITION_CLIENT_H

#include <QObject>
#include <QJsonObject>
#include <QUrl>

class QImage;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

class ManualRecognitionClient : public QObject {
    Q_OBJECT

public:
    explicit ManualRecognitionClient(QObject *parent = nullptr);

    void setRecognizeUrl(const QUrl &url);
    bool isBusy() const;
    bool recognize(const QImage &image, const QString &requestId,
                   quint64 frameId, quint64 timestampNs);
    void abort();

signals:
    void requestStarted(const QString &requestId, quint64 frameId, int jpegBytes);
    void requestSucceeded(const QString &requestId, quint64 frameId,
                          const QJsonObject &result, qint64 totalElapsedMs);
    void requestFailed(const QString &requestId, quint64 frameId,
                       const QString &errorMessage, int httpStatus,
                       qint64 totalElapsedMs);

private:
    void failCurrent(const QString &message, int httpStatus, bool abortReply);
    void finishReply();

    QNetworkAccessManager *manager_;
    QNetworkReply *reply_;
    QTimer *timeoutTimer_;
    QUrl url_;
    QString requestId_;
    quint64 frameId_;
    quint64 timestampNs_;
    qint64 startedMs_;
};

#endif
