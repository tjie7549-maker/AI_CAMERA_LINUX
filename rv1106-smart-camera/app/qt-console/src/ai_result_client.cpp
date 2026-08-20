#include "ai_result_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QDebug>

namespace {
int jsonInt(const QJsonObject &object, const QString &key, int defaultValue)
{
    const QJsonValue value = object.value(key);
    return value.isDouble() ? value.toInt(defaultValue) : defaultValue;
}

QString jsonString(const QJsonObject &object, const QString &key,
                   const QString &defaultValue)
{
    const QJsonValue value = object.value(key);
    return value.isString() ? value.toString() : defaultValue;
}
} // namespace

AiResultClient::AiResultClient(const QString &serverIp, quint16 serverPort,
                               QObject *parent)
    : QObject(parent),
      serverIp_(serverIp),
      serverPort_(serverPort),
      socket_(new QTcpSocket(this)),
      reconnectTimer_(new QTimer(this)),
      connectTimeoutTimer_(new QTimer(this)),
      running_(false),
      invalidMessageCount_(0)
{
    reconnectTimer_->setSingleShot(true);
    connectTimeoutTimer_->setSingleShot(true);

    connect(reconnectTimer_, &QTimer::timeout,
            this, &AiResultClient::connectToServer);
    connect(connectTimeoutTimer_, &QTimer::timeout, this, [this]() {
        if (socket_->state() != QAbstractSocket::ConnectingState)
            return;
        emit errorOccurred(QStringLiteral("TCP connection timed out"));
        setConnectionState(QStringLiteral("Error"));
        socket_->abort();
        scheduleReconnect();
    });
    connect(socket_, &QTcpSocket::connected,
            this, &AiResultClient::onConnected);
    connect(socket_, &QTcpSocket::disconnected,
            this, &AiResultClient::onDisconnected);
    connect(socket_, &QTcpSocket::readyRead,
            this, &AiResultClient::onReadyRead);
    connect(socket_, QOverload<QAbstractSocket::SocketError>::of(
                         &QAbstractSocket::errorOccurred),
            this, &AiResultClient::onSocketError);
}

void AiResultClient::start()
{
    if (running_)
        return;
    running_ = true;
    connectToServer();
}

void AiResultClient::stop()
{
    running_ = false;
    reconnectTimer_->stop();
    connectTimeoutTimer_->stop();
    receiveBuffer_.clear();
    socket_->abort();
    setConnectionState(QStringLiteral("Disconnected"));
}

void AiResultClient::connectToServer()
{
    if (!running_ || socket_->state() != QAbstractSocket::UnconnectedState)
        return;

    reconnectTimer_->stop();
    receiveBuffer_.clear();
    setConnectionState(QStringLiteral("Connecting"));
    socket_->connectToHost(serverIp_, serverPort_);
    connectTimeoutTimer_->start(kReconnectDelayMs);
}

void AiResultClient::onConnected()
{
    connectTimeoutTimer_->stop();
    reconnectTimer_->stop();
    setConnectionState(QStringLiteral("Connected"));
}

void AiResultClient::onDisconnected()
{
    connectTimeoutTimer_->stop();
    receiveBuffer_.clear();
    setConnectionState(QStringLiteral("Disconnected"));
    scheduleReconnect();
}

void AiResultClient::onReadyRead()
{
    const QByteArray incoming = socket_->readAll();
    if (incoming.size() > kMaximumBufferSize - receiveBuffer_.size()) {
        receiveBuffer_.clear();
        ++invalidMessageCount_;
        emit errorOccurred(
            QStringLiteral("Receive buffer exceeded 64 KiB; discarded (%1 errors)")
                .arg(invalidMessageCount_));
        return;
    }

    receiveBuffer_.append(incoming);
    int newlineIndex = -1;
    while ((newlineIndex = receiveBuffer_.indexOf('\n')) >= 0) {
        const QByteArray message = receiveBuffer_.left(newlineIndex).trimmed();
        receiveBuffer_.remove(0, newlineIndex + 1);
        if (message.isEmpty())
            continue;

        AiResult result;
        QString error;
        if (parseMessage(message, result, error)) {
            qInfo().noquote()
                << QStringLiteral("#AI result: scene=%1, people=%2, warning=%3, objects=%4, latency=%5 ms")
                       .arg(result.scene)
                       .arg(result.peopleCount)
                       .arg(result.warning ? QStringLiteral("true")
                                           : QStringLiteral("false"))
                       .arg(result.objects.isEmpty()
                                ? QStringLiteral("none")
                                : result.objects.join(QStringLiteral(", ")))
                       .arg(result.latencyMs);
            emit resultReceived(result);
        } else {
            ++invalidMessageCount_;
            const QString message = QStringLiteral("Invalid JSON #%1: %2")
                                        .arg(invalidMessageCount_)
                                        .arg(error);
            qWarning().noquote() << "#AI error:" << message;
            emit errorOccurred(message);
        }
    }
}

void AiResultClient::onSocketError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error)
    connectTimeoutTimer_->stop();
    emit errorOccurred(QStringLiteral("TCP: %1").arg(socket_->errorString()));
    setConnectionState(QStringLiteral("Error"));
    if (socket_->state() != QAbstractSocket::UnconnectedState)
        socket_->abort();
    scheduleReconnect();
}

bool AiResultClient::parseMessage(const QByteArray &message, AiResult &result,
                                  QString &error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(message, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        error = parseError.errorString();
        return false;
    }
    if (!document.isObject()) {
        error = QStringLiteral("root value is not an object");
        return false;
    }

    const QJsonObject root = document.object();
    result.schemaVersion = jsonInt(root, QStringLiteral("schema_version"), 0);
    if (result.schemaVersion != 0 && result.schemaVersion != 1) {
        error = QStringLiteral("unsupported schema_version");
        return false;
    }
    result.messageType = jsonString(root, QStringLiteral("message_type"), QString());
    result.cameraId = jsonString(root, QStringLiteral("camera_id"), QString());
    if (result.schemaVersion == 1 &&
        (result.messageType.isEmpty() || result.cameraId.isEmpty())) {
        error = QStringLiteral("versioned message lacks type or camera_id");
        return false;
    }
    result.timestamp = jsonString(root, QStringLiteral("timestamp"), result.timestamp);
    result.model = jsonString(root, QStringLiteral("model"), result.model);
    result.success = root.value(QStringLiteral("success")).toBool(false);
    result.hasSentinelState = root.contains(QStringLiteral("sentinel_active")) ||
                              root.contains(QStringLiteral("display_awake"));
    if (result.hasSentinelState) {
        result.sentinelActive = root.value(QStringLiteral("sentinel_active")).toBool(false);
        result.displayAwake = root.value(QStringLiteral("display_awake")).toBool(false);
    }
    result.type = jsonString(root, QStringLiteral("type"), QString());
    result.source = jsonString(root, QStringLiteral("source"), QString());
    result.requestId = jsonString(root, QStringLiteral("request_id"), QString());
    result.eventId = jsonString(root, QStringLiteral("event_id"), QString());
    if (root.value(QStringLiteral("frame_id")).isDouble())
        result.frameId = static_cast<quint64>(root.value(QStringLiteral("frame_id")).toDouble());
    if (root.value(QStringLiteral("frame_timestamp_ns")).isDouble())
        result.frameTimestampNs = static_cast<quint64>(
            root.value(QStringLiteral("frame_timestamp_ns")).toDouble());
    if (root.value(QStringLiteral("server_latency_ms")).isDouble())
        result.serverLatencyMs = static_cast<qint64>(
            root.value(QStringLiteral("server_latency_ms")).toDouble());
    if (root.value(QStringLiteral("latency_ms")).isDouble())
        result.latencyMs = static_cast<qint64>(
            root.value(QStringLiteral("latency_ms")).toDouble());
    if (root.value(QStringLiteral("captured_at_ms")).isDouble())
        result.capturedAtMs = static_cast<qint64>(
            root.value(QStringLiteral("captured_at_ms")).toDouble());

    const QJsonObject usage = root.value(QStringLiteral("usage")).toObject();
    result.inputTokens = jsonInt(usage, QStringLiteral("input_tokens"), 0);
    result.outputTokens = jsonInt(usage, QStringLiteral("output_tokens"), 0);
    result.totalTokens = jsonInt(usage, QStringLiteral("total_tokens"),
                                 result.inputTokens + result.outputTokens);

    const QJsonObject resultObject = root.value(QStringLiteral("result")).toObject();
    result.scene = jsonString(resultObject, QStringLiteral("scene"), result.scene);
    result.currentPeople = jsonInt(resultObject, QStringLiteral("current_people"), 0);
    result.maxPeople = jsonInt(resultObject, QStringLiteral("max_people"),
                               result.currentPeople);
    result.trackCount = jsonInt(resultObject, QStringLiteral("track_count"), 0);
    result.durationMs = static_cast<qint64>(
        resultObject.value(QStringLiteral("duration_ms")).toDouble(0));
    result.bestFrameId = static_cast<quint64>(
        resultObject.value(QStringLiteral("best_frame_id")).toDouble(0));
    result.eventState = jsonString(resultObject, QStringLiteral("event_state"),
                                   QString());
    result.cloudState = jsonString(resultObject, QStringLiteral("cloud_state"),
                                   QString());
    result.frameMatch = jsonString(resultObject, QStringLiteral("frame_match"),
                                   jsonString(root, QStringLiteral("frame_match"),
                                              QString()));
    result.peopleCount = jsonInt(resultObject, QStringLiteral("people_count"),
                                 result.currentPeople);
    result.warning = resultObject.value(QStringLiteral("warning")).toBool(false);
    result.warningReason = jsonString(resultObject,
                                      QStringLiteral("warning_reason"),
                                      result.warningReason);
    result.summary = jsonString(resultObject, QStringLiteral("summary"),
                                result.summary);

    const QJsonArray objects = resultObject.value(QStringLiteral("objects")).toArray();
    for (const QJsonValue &value : objects) {
        if (value.isString()) {
            result.objects.append(value.toString());
            continue;
        }
        if (!value.isObject())
            continue;
        const QJsonObject object = value.toObject();
        const QString name = jsonString(object, QStringLiteral("name"),
                                        QStringLiteral("Object"));
        const int count = jsonInt(object, QStringLiteral("count"), 1);
        result.objects.append(count > 1
                                  ? QStringLiteral("%1 x%2").arg(name).arg(count)
                                  : name);
    }

    const QJsonObject errorObject = root.value(QStringLiteral("error")).toObject();
    result.errorType = jsonString(root, QStringLiteral("error_type"),
                                  jsonString(errorObject, QStringLiteral("type"),
                                             QString()));
    result.errorMessage = jsonString(root, QStringLiteral("error_message"),
                                     jsonString(errorObject, QStringLiteral("message"),
                                                QString()));
    return true;
}

void AiResultClient::scheduleReconnect()
{
    if (!running_ || reconnectTimer_->isActive())
        return;
    reconnectTimer_->start(kReconnectDelayMs);
}

void AiResultClient::setConnectionState(const QString &state)
{
    if (connectionState_ == state)
        return;
    connectionState_ = state;
    qInfo().noquote() << "#TCP state:" << state;
    emit connectionStateChanged(state);
}
