#ifndef RESULT_STORAGE_CLIENT_H
#define RESULT_STORAGE_CLIENT_H

#include <QJsonObject>
#include <QObject>
#include <QUrl>
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class ResultStorageClient : public QObject {
    Q_OBJECT
   public:
    explicit ResultStorageClient(QObject *parent = nullptr);
    void setUrl(const QUrl &url);
    void setEventApiBase(const QUrl &url);
    bool isBusy() const;
    bool save(const QString &source, const QString &requestId);
    bool saveEvent(const QString &eventId);
    void recoverActiveEvent();
    void abort();
   signals:
    void succeeded(const QString &, const QString &, const QString &, bool);
    void failed(const QString &, const QString &);
    void activeEventRecovered(const QJsonObject &);
    void activeEventRecoveryFailed(const QString &);

   private:
    bool post(const QUrl &url, const QJsonObject &payload, const QString &source,
              const QString &requestId);
    QNetworkAccessManager *manager_;
    QNetworkReply *reply_;
    QNetworkReply *recoveryReply_;
    QTimer *timer_;
    QUrl url_;
    QUrl eventApiBase_;
    QString source_, requestId_;
};
#endif
