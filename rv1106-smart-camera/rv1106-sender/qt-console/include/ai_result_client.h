#ifndef AI_RESULT_CLIENT_H
#define AI_RESULT_CLIENT_H

#include <QByteArray>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>

#include "ai_result.h"

class AiResultClient : public QObject {
    Q_OBJECT

   public:
    explicit AiResultClient(const QString &serverIp, quint16 serverPort, QObject *parent = nullptr);

    void start();
    void stop();

    static bool parseMessage(const QByteArray &message, AiResult &result, QString &error);

   signals:
    void resultReceived(const AiResult &result);
    void connectionStateChanged(const QString &state);
    void errorOccurred(const QString &message);

   private slots:
    void connectToServer();
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError error);

   private:
    void scheduleReconnect();
    void setConnectionState(const QString &state);

    static const int kMaximumBufferSize = 64 * 1024;
    static const int kReconnectDelayMs = 3000;

    QString serverIp_;
    quint16 serverPort_;
    QTcpSocket *socket_;
    QTimer *reconnectTimer_;
    QTimer *connectTimeoutTimer_;
    QByteArray receiveBuffer_;
    bool running_;
    quint64 invalidMessageCount_;
    QString connectionState_;
};

#endif
