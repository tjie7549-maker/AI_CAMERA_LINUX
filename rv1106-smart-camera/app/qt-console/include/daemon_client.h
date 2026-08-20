#ifndef DAEMON_CLIENT_H
#define DAEMON_CLIENT_H

#include <QObject>
#include <QString>

class QLocalSocket;

class DaemonClient : public QObject {
    Q_OBJECT
public:
    explicit DaemonClient(const QString &socketPath, QObject *parent = nullptr);
    void requestStatus();
    void setAutoAe(bool enabled);
    void setControl(const QString &id, int value);
    void restoreDefaults();
    void enterDebug();
    void exitDebug();
signals:
    void statusReceived(const QString &json);
    void requestFailed(const QString &message);
private:
    void send(const QByteArray &json);
    QString socketPath_;
    QLocalSocket *socket_;
};

#endif
