#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QWidget>

#include "ai_result.h"

class QLabel;
class QTimer;
class VideoWidget;
class QImage;

class MainWindow : public QWidget {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

public slots:
    void updateAiResult(const AiResult &result);
    void updateConnectionState(const QString &state);
    void updateCurrentTime();
    void updateAiState(const QString &state);
    void updateError(const QString &message);
    void updatePreviewFrame(const QImage &image, qulonglong frameId,
                            qulonglong sourceTimeNs);
    void updatePreviewState(int state);
    void updatePreviewStats(const QString &line1, const QString &line2);

private:
    void buildUi();
    void applyStyle();
    void showDefaultState();
    void setStateLabel(QLabel *label, const QString &text,
                       const QString &state);

    QLabel *clockLabel_;
    QLabel *videoStatusLabel_;
    QLabel *aiStatusLabel_;
    QLabel *modelLabel_;
    VideoWidget *videoWidget_;
    QLabel *videoInfo_;
    QLabel *videoWaiting_;

    QLabel *sceneValue_;
    QLabel *peopleValue_;
    QLabel *objectsValue_;
    QLabel *warningValue_;
    QLabel *warningReasonValue_;
    QLabel *summaryValue_;

    QLabel *latencyValue_;
    QLabel *totalTokensValue_;
    QLabel *inputTokensValue_;
    QLabel *outputTokensValue_;
    QLabel *updatedValue_;
    QLabel *tcpValue_;
    QLabel *errorValue_;

    QTimer *clockTimer_;
};

#endif
