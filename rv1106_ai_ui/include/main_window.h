#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QWidget>
#include <QImage>

#include "ai_result.h"

class QLabel;
class ManualRecognitionClient;
class QPushButton;
class QTimer;
class VideoWidget;
class QJsonObject;

class MainWindow : public QWidget {
    Q_OBJECT

public:
    explicit MainWindow(ManualRecognitionClient *manualClient,
                        QWidget *parent = nullptr);

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
    void onManualRequestStarted(const QString &requestId, quint64 frameId,
                                int jpegBytes);
    void onManualRequestSucceeded(const QString &requestId, quint64 frameId,
                                  const QJsonObject &result, qint64 elapsedMs);
    void onManualRequestFailed(const QString &requestId, quint64 frameId,
                               const QString &message, int httpStatus,
                               qint64 elapsedMs);

private:
    enum class PreviewUiState {
        Live,
        Frozen,
        Recognizing,
        ResultReady,
        Error,
    };

    struct FrozenFrameSnapshot {
        QImage image;
        quint64 frameId = 0;
        quint64 timestampNs = 0;

        bool isValid() const { return !image.isNull() && frameId > 0; }
    };

    void buildUi();
    void applyStyle();
    void showDefaultState();
    void onPauseToggled(bool checked);
    void onRecognizeClicked();
    void setPreviewUiState(PreviewUiState state);
    void updateVideoStatus();
    void applyManualResult(const AiResult &result, qint64 totalElapsedMs);
    void setStateLabel(QLabel *label, const QString &text,
                       const QString &state);

    QLabel *clockLabel_;
    QLabel *videoStatusLabel_;
    QLabel *aiStatusLabel_;
    QLabel *modelLabel_;
    VideoWidget *videoWidget_;
    QLabel *videoInfo_;
    QLabel *videoWaiting_;
    ManualRecognitionClient *manualClient_;
    QPushButton *pauseButton_;
    QPushButton *recognizeButton_;

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
    PreviewUiState previewUiState_;
    int previewStreamState_;
    qulonglong sourceFrameId_;
    qulonglong sourceTimestampNs_;
    FrozenFrameSnapshot requestSnapshot_;
    QString activeRequestId_;
    QString lastAppliedRequestId_;
    quint64 activeRequestFrameId_;
    quint64 activeRequestTimestampNs_;
    quint64 manualRequestSequence_;
};

#endif
