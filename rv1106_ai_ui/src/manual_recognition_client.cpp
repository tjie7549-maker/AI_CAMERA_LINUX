#include "manual_recognition_client.h"

#include <QBuffer>
#include <QDateTime>
#include <QImage>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace {
const int kTimeoutMs = 30000;
const int kMaximumJpegBytes = 1024 * 1024;
}

ManualRecognitionClient::ManualRecognitionClient(QObject *parent)
    : QObject(parent),
      manager_(new QNetworkAccessManager(this)),
      reply_(nullptr),
      timeoutTimer_(new QTimer(this)),
      frameId_(0),
      timestampNs_(0),
      startedMs_(0)
{
    timeoutTimer_->setSingleShot(true);
    connect(timeoutTimer_, &QTimer::timeout, this, [this]() {
        failCurrent(QStringLiteral("识别请求超时"), 0, true);
    });
}

void ManualRecognitionClient::setRecognizeUrl(const QUrl &url)
{
    url_ = url;
}

bool ManualRecognitionClient::isBusy() const
{
    return reply_ != nullptr;
}

bool ManualRecognitionClient::recognize(const QString &source, const QImage &image,
                                        const QString &requestId, quint64 frameId,
                                        quint64 timestampNs)
{
    if ((source != QStringLiteral("manual") && source != QStringLiteral("auto")) ||
        isBusy() || image.isNull() || requestId.isEmpty() || frameId == 0 ||
        !url_.isValid() || url_.scheme() != QStringLiteral("http")) {
        return false;
    }

    const qint64 encodeStartedMs = QDateTime::currentMSecsSinceEpoch();
    QByteArray jpegData;
    QBuffer buffer(&jpegData);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "JPG", 85) ||
        jpegData.size() <= 4 || jpegData.size() > kMaximumJpegBytes ||
        static_cast<unsigned char>(jpegData.at(0)) != 0xff ||
        static_cast<unsigned char>(jpegData.at(1)) != 0xd8 ||
        static_cast<unsigned char>(jpegData.at(jpegData.size() - 2)) != 0xff ||
        static_cast<unsigned char>(jpegData.at(jpegData.size() - 1)) != 0xd9) {
        return false;
    }

    QNetworkRequest request(url_);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("image/jpeg"));
    request.setHeader(QNetworkRequest::ContentLengthHeader, jpegData.size());
    request.setRawHeader("X-Recognition-Source", source.toUtf8());
    request.setRawHeader("X-Request-Id", requestId.toUtf8());
    request.setRawHeader("X-Frame-Id", QByteArray::number(frameId));
    request.setRawHeader("X-Frame-Timestamp-Ns", QByteArray::number(timestampNs));
    request.setRawHeader("X-Image-Width", QByteArray::number(image.width()));
    request.setRawHeader("X-Image-Height", QByteArray::number(image.height()));

    source_ = source;
    requestId_ = requestId;
    frameId_ = frameId;
    timestampNs_ = timestampNs;
    startedMs_ = QDateTime::currentMSecsSinceEpoch();
    reply_ = manager_->post(request, jpegData);
    timeoutTimer_->start(kTimeoutMs);
    qInfo("[Recognition] start source=%s request_id=%s frame_id=%llu jpeg_bytes=%d encode_ms=%lld",
          source_.toLocal8Bit().constData(), requestId_.toLocal8Bit().constData(),
          static_cast<unsigned long long>(frameId_),
          jpegData.size(), static_cast<long long>(startedMs_ - encodeStartedMs));
    emit requestStarted(source_, requestId_, frameId_, jpegData.size());
    connect(reply_, &QNetworkReply::finished, this, &ManualRecognitionClient::finishReply);
    return true;
}

void ManualRecognitionClient::abort()
{
    failCurrent(QStringLiteral("识别已取消"), 0, true);
}

void ManualRecognitionClient::failCurrent(const QString &message, int httpStatus,
                                          bool abortReply)
{
    if (!reply_)
        return;
    QNetworkReply *reply = reply_;
    reply_ = nullptr;
    timeoutTimer_->stop();
    const qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - startedMs_;
    if (abortReply) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
    }
    reply->deleteLater();
    qWarning("[Recognition] failed source=%s request_id=%s frame_id=%llu status=%d error=%s",
             source_.toLocal8Bit().constData(), requestId_.toLocal8Bit().constData(),
             static_cast<unsigned long long>(frameId_),
             httpStatus, message.toLocal8Bit().constData());
    emit requestFailed(source_, requestId_, frameId_, message, httpStatus, elapsedMs);
}

void ManualRecognitionClient::finishReply()
{
    if (!reply_)
        return;
    QNetworkReply *reply = reply_;
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        failCurrent(QStringLiteral("HTTP：%1").arg(reply->errorString()), httpStatus, false);
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (httpStatus < 200 || httpStatus >= 300 || parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        failCurrent(QStringLiteral("识别响应无效"), httpStatus, false);
        return;
    }
    const QJsonObject result = document.object();
    if (!result.value(QStringLiteral("success")).toBool(false)) {
        failCurrent(result.value(QStringLiteral("error_message")).toString(
                        result.value(QStringLiteral("error")).toString(QStringLiteral("识别失败"))),
                    httpStatus, false);
        return;
    }

    const QString source = source_;
    const QString requestId = requestId_;
    const quint64 frameId = frameId_;
    const qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - startedMs_;
    reply_ = nullptr;
    timeoutTimer_->stop();
    reply->deleteLater();
    qInfo("[Recognition] success source=%s request_id=%s frame_id=%llu elapsed_ms=%lld",
          source.toLocal8Bit().constData(), requestId.toLocal8Bit().constData(),
          static_cast<unsigned long long>(frameId),
          static_cast<long long>(elapsedMs));
    emit requestSucceeded(source, requestId, frameId, result, elapsedMs);
}
