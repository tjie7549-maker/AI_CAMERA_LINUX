#include "result_storage_client.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
ResultStorageClient::ResultStorageClient(QObject *p):QObject(p),manager_(new QNetworkAccessManager(this)),reply_(nullptr),timer_(new QTimer(this)) { timer_->setSingleShot(true); connect(timer_,&QTimer::timeout,this,&ResultStorageClient::abort); }
void ResultStorageClient::setUrl(const QUrl &u){url_=u;} bool ResultStorageClient::isBusy()const{return reply_;}
bool ResultStorageClient::save(const QString&s,const QString&i){if(reply_||s.isEmpty()||i.isEmpty())return false; source_=s;requestId_=i; QJsonObject o{{"source",s},{"request_id",i}}; QNetworkRequest r(url_);r.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");reply_=manager_->post(r,QJsonDocument(o).toJson(QJsonDocument::Compact));timer_->start(10000);connect(reply_,&QNetworkReply::finished,this,[this](){QNetworkReply*q=reply_;reply_=nullptr;timer_->stop();QJsonDocument d=QJsonDocument::fromJson(q->readAll());int status=q->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();if(q->error()!=QNetworkReply::NoError||!d.isObject()||!d.object().value("success").toBool())emit failed(source_,QStringLiteral("保存失败（HTTP %1）").arg(status));else emit succeeded(source_,requestId_,d.object().value("saved_relative_path").toString(),d.object().value("already_saved").toBool());q->deleteLater();});return true;}
void ResultStorageClient::abort(){if(!reply_)return; QNetworkReply*q=reply_;reply_=nullptr;timer_->stop();q->abort();q->deleteLater();emit failed(source_,QStringLiteral("保存已取消"));}
