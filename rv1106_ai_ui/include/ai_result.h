#ifndef AI_RESULT_H
#define AI_RESULT_H

#include <QMetaType>
#include <QString>
#include <QStringList>

struct AiResult {
    AiResult()
        : scene(QStringLiteral("等待识别结果")),
          peopleCount(0),
          warning(false),
          warningReason(QStringLiteral("-")),
          summary(QStringLiteral("暂无识别结果")),
          model(QStringLiteral("qwen3-vl-flash")),
          latencyMs(0),
          inputTokens(0),
          outputTokens(0),
          totalTokens(0),
          timestamp(QStringLiteral("-")),
          success(false),
          hasSentinelState(false),
          sentinelActive(false),
          displayAwake(false),
          frameId(0),
          frameTimestampNs(0),
          serverLatencyMs(0)
    {
    }

    QString scene;
    int peopleCount;
    QStringList objects;
    bool warning;
    QString warningReason;
    QString summary;

    QString model;
    qint64 latencyMs;

    int inputTokens;
    int outputTokens;
    int totalTokens;

    QString timestamp;
    bool success;
    bool hasSentinelState;
    bool sentinelActive;
    bool displayAwake;

    QString type;
    QString source;
    QString requestId;
    quint64 frameId;
    quint64 frameTimestampNs;
    qint64 serverLatencyMs;

    QString errorType;
    QString errorMessage;
};

Q_DECLARE_METATYPE(AiResult)

#endif
