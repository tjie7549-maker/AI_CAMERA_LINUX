#include "result_storage_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>

ResultStorageClient::ResultStorageClient(QObject *parent)
    : QObject(parent),
      manager_(new QNetworkAccessManager(this)),
      reply_(nullptr),
      recoveryReply_(nullptr),
      timer_(new QTimer(this))
{
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &ResultStorageClient::abort);
}

void ResultStorageClient::setUrl(const QUrl &url)
{
    url_ = url;
}

void ResultStorageClient::setEventApiBase(const QUrl &url)
{
    eventApiBase_ = url;
}

bool ResultStorageClient::isBusy() const
{
    return reply_ != nullptr;
}

bool ResultStorageClient::save(const QString &source, const QString &requestId)
{
    return post(url_, QJsonObject{{QStringLiteral("source"), source},
                                  {QStringLiteral("request_id"), requestId}},
                source, requestId);
}

bool ResultStorageClient::saveEvent(const QString &eventId)
{
    if (eventId.isEmpty() || !eventApiBase_.isValid())
        return false;
    QUrl url(eventApiBase_);
    url.setPath(QStringLiteral("/events/%1/save").arg(eventId));
    return post(url, QJsonObject(), QStringLiteral("event"), eventId);
}

bool ResultStorageClient::post(const QUrl &url, const QJsonObject &payload,
                               const QString &source, const QString &requestId)
{
    if (reply_ || !url.isValid() || source.isEmpty() || requestId.isEmpty())
        return false;
    source_ = source;
    requestId_ = requestId;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    reply_ = manager_->post(request,
                            QJsonDocument(payload).toJson(QJsonDocument::Compact));
    timer_->start(10000);
    connect(reply_, &QNetworkReply::finished, this, [this]() {
        QNetworkReply *finished = reply_;
        reply_ = nullptr;
        timer_->stop();
        const int status = finished->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonDocument document = QJsonDocument::fromJson(finished->readAll());
        if (finished->error() != QNetworkReply::NoError || !document.isObject() ||
            !document.object().value(QStringLiteral("success")).toBool()) {
            emit failed(source_, QStringLiteral("保存失败（HTTP %1）").arg(status));
        } else {
            const QJsonObject object = document.object();
            emit succeeded(source_, requestId_,
                           object.value(QStringLiteral("saved_relative_path")).toString(),
                           object.value(QStringLiteral("already_saved")).toBool());
        }
        finished->deleteLater();
    });
    return true;
}

void ResultStorageClient::recoverActiveEvent()
{
    if (recoveryReply_ || !eventApiBase_.isValid())
        return;
    QUrl url(eventApiBase_);
    url.setPath(QStringLiteral("/events"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("state"), QStringLiteral("active"));
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
    url.setQuery(query);
    recoveryReply_ = manager_->get(QNetworkRequest(url));
    QNetworkReply *request = recoveryReply_;
    QTimer::singleShot(3000, request, [request]() {
        if (request->isRunning())
            request->abort();
    });
    connect(request, &QNetworkReply::finished, this, [this, request]() {
        recoveryReply_ = nullptr;
        const QJsonDocument document = QJsonDocument::fromJson(request->readAll());
        if (request->error() != QNetworkReply::NoError || !document.isObject()) {
            emit activeEventRecoveryFailed(request->errorString());
        } else {
            const QJsonArray events = document.object()
                                          .value(QStringLiteral("events")).toArray();
            if (!events.isEmpty() && events.first().isObject())
                emit activeEventRecovered(events.first().toObject());
        }
        request->deleteLater();
    });
}

void ResultStorageClient::abort()
{
    if (recoveryReply_) {
        QNetworkReply *recovery = recoveryReply_;
        recoveryReply_ = nullptr;
        recovery->abort();
        recovery->deleteLater();
    }
    if (!reply_)
        return;
    QNetworkReply *request = reply_;
    reply_ = nullptr;
    timer_->stop();
    request->abort();
    request->deleteLater();
    emit failed(source_, QStringLiteral("保存已取消"));
}
