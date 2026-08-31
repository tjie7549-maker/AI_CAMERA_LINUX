#include "daemon_client.h"

// Qt 到 camera-daemon 的 Unix Socket 客户端：将 UI 操作编码为 JSON 命令并异步返回状态。

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTimer>

DaemonClient::DaemonClient(const QString &socketPath, QObject *parent)
    : QObject(parent), socketPath_(socketPath), socket_(new QLocalSocket(this)) {
    connect(socket_, &QLocalSocket::readyRead, this, [this]() {
        const QString reply = QString::fromUtf8(socket_->readAll()).trimmed();
        socket_->disconnectFromServer();
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(reply.toUtf8(), &parseError);
        const QJsonObject object = document.object();
        if (parseError.error == QJsonParseError::NoError &&
            object.value(QStringLiteral("ok")).toBool() &&
            !object.contains(QStringLiteral("state"))) {
            QTimer::singleShot(0, this, &DaemonClient::requestStatus);
            return;
        }
        if (parseError.error == QJsonParseError::NoError &&
            object.contains(QStringLiteral("error"))) {
            emit requestFailed(object.value(QStringLiteral("error")).toString());
            return;
        }
        emit statusReceived(reply);
    });
    connect(socket_, QOverload<QLocalSocket::LocalSocketError>::of(&QLocalSocket::error), this,
            [this](QLocalSocket::LocalSocketError) {
                emit requestFailed(
                    QStringLiteral("camera-daemon 不可用：%1").arg(socket_->errorString()));
            });
}

void DaemonClient::send(const QByteArray &json) {
    if (socket_->state() != QLocalSocket::UnconnectedState)
        socket_->abort();
    socket_->connectToServer(socketPath_);
    if (!socket_->waitForConnected(250)) {
        emit requestFailed(
            QStringLiteral("camera-daemon 未启动或 Socket 不存在：%1").arg(socketPath_));
        return;
    }
    socket_->write(json + '\n');
    if (!socket_->waitForBytesWritten(250))
        emit requestFailed(
            QStringLiteral("camera-daemon 请求发送失败：%1").arg(socket_->errorString()));
}

void DaemonClient::requestStatus() {
    send("{\"cmd\":\"get_status\"}");
}
void DaemonClient::setAutoAe(bool enabled) {
    QJsonObject object;
    object.insert(QStringLiteral("cmd"), QStringLiteral("set_auto_ae"));
    object.insert(QStringLiteral("auto_ae"), enabled);
    send(QJsonDocument(object).toJson(QJsonDocument::Compact));
}
void DaemonClient::setControl(const QString &id, int value) {
    QJsonObject object;
    object.insert(QStringLiteral("cmd"), QStringLiteral("set_control"));
    object.insert(QStringLiteral("id"), id);
    object.insert(QStringLiteral("value"), value);
    send(QJsonDocument(object).toJson(QJsonDocument::Compact));
}
void DaemonClient::restoreDefaults() {
    send("{\"cmd\":\"restore_defaults\"}");
}
void DaemonClient::enterDebug() {
    send("{\"cmd\":\"enter_debug\"}");
}
void DaemonClient::exitDebug() {
    send("{\"cmd\":\"exit_debug\"}");
}
