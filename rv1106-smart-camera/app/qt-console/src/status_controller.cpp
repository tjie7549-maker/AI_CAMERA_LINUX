#include "status_controller.h"

#include <QDebug>

StatusController::StatusController(QObject *parent)
    : QObject(parent)
{
    checkTimer_.setInterval(1000);
    connect(&checkTimer_, &QTimer::timeout,
            this, &StatusController::updateAiState);
    checkTimer_.start();
    publishState(QStringLiteral("AI OFFLINE"));
}

void StatusController::markAiResultReceived()
{
    lastResultTimer_.restart();
    publishState(QStringLiteral("AI ONLINE"));
}

void StatusController::resetAiState()
{
    lastResultTimer_.invalidate();
    publishState(QStringLiteral("AI OFFLINE"));
}

void StatusController::updateAiState()
{
    if (!lastResultTimer_.isValid()) {
        publishState(QStringLiteral("AI OFFLINE"));
        return;
    }

    const qint64 elapsedMs = lastResultTimer_.elapsed();
    if (elapsedMs <= 15000)
        publishState(QStringLiteral("AI ONLINE"));
    else if (elapsedMs <= 20000)
        publishState(QStringLiteral("AI STALE"));
    else
        publishState(QStringLiteral("AI OFFLINE"));
}

void StatusController::publishState(const QString &state)
{
    if (currentState_ == state)
        return;
    currentState_ = state;
    qInfo().noquote() << "#AI state:" << state;
    emit aiStateChanged(state);
}
