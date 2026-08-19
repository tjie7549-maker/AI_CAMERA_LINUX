#ifndef ATTENDANCE_HTTP_CLIENT_H
#define ATTENDANCE_HTTP_CLIENT_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QUrl>
#include <QImage>

class AttendanceHttpClient : public QObject {
    Q_OBJECT
public:
    AttendanceHttpClient(const QUrl &url, const QByteArray &token, QObject *parent = nullptr);
    void verify(const QImage &face, const QString &attendanceType);
signals:
    void finished(bool success, const QString &message);
private:
    QNetworkAccessManager manager_;
    QUrl url_;
    QByteArray token_;
};

#endif
