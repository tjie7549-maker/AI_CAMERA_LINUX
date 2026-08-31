#ifndef STATUS_CONTROLLER_H
#define STATUS_CONTROLLER_H

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

class StatusController : public QObject {
    Q_OBJECT

   public:
    explicit StatusController(QObject *parent = nullptr);

   public slots:
    void markAiResultReceived();
    void resetAiState();

   signals:
    void aiStateChanged(const QString &state);

   private slots:
    void updateAiState();

   private:
    void publishState(const QString &state);

    QElapsedTimer lastResultTimer_;
    QTimer checkTimer_;
    QString currentState_;
};

#endif
