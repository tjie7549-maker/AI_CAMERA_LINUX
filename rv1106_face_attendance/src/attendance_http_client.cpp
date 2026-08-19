#include "attendance_http_client.h"

#include <QBuffer>
#include <QDateTime>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUuid>

AttendanceHttpClient::AttendanceHttpClient(const QUrl &url, const QByteArray &token, QObject *parent)
    : QObject(parent), url_(url), token_(token) {}

void AttendanceHttpClient::verify(const QImage &face, const QString &attendanceType) {
    QByteArray jpeg;
    QBuffer buffer(&jpeg); buffer.open(QIODevice::WriteOnly);
    if (!face.save(&buffer, "JPG", 90)) { emit finished(false, QStringLiteral("JPEG 编码失败")); return; }
    QNetworkRequest request(url_);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("image/jpeg"));
    request.setRawHeader("X-Attendance-Token", token_);
    request.setRawHeader("X-Attendance-Request-Id", QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1());
    request.setRawHeader("X-Attendance-Type", attendanceType.toLatin1());
    request.setRawHeader("X-Face-Width-Px", QByteArray::number(face.width()));
    request.setRawHeader("X-Face-Quality", "0.90");
    request.setRawHeader("X-Camera-Id", "rv1106-entrance-01");
    QNetworkReply *reply = manager_.post(request, jpeg);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QByteArray body = reply->readAll();
        const bool success = reply->error() == QNetworkReply::NoError && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
        emit finished(success, success ? QString::fromUtf8(body) : QStringLiteral("上传失败：%1 %2").arg(reply->errorString(), QString::fromUtf8(body)));
        reply->deleteLater();
    });
}
