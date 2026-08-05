#ifndef RESULT_STORAGE_CLIENT_H
#define RESULT_STORAGE_CLIENT_H

#include <QObject>
#include <QUrl>
class QNetworkAccessManager; class QNetworkReply; class QTimer;
class ResultStorageClient : public QObject {
    Q_OBJECT
public:
    explicit ResultStorageClient(QObject *parent=nullptr);
    void setUrl(const QUrl &url); bool isBusy() const;
    bool save(const QString &source, const QString &requestId);
    void abort();
signals:
    void succeeded(const QString&, const QString&, const QString&, bool);
    void failed(const QString&, const QString&);
private:
    QNetworkAccessManager *manager_; QNetworkReply *reply_; QTimer *timer_; QUrl url_;
    QString source_, requestId_;
};
#endif
