#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QWidget>
#include <QImage>

#include "ai_result.h"

class QLabel;
class ManualRecognitionClient;
class ResultStorageClient;
class QPushButton;
class QTimer;
class VideoWidget;
class QJsonObject;

class MainWindow : public QWidget {
    Q_OBJECT

public:
    explicit MainWindow(ManualRecognitionClient *manualClient, ResultStorageClient *storageClient,
                        int autoRecognitionIntervalMs,
                        bool legacyAutoRecognitionEnabled,
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
    void onRecognitionRequestStarted(const QString &source, const QString &requestId,
                                     quint64 frameId, int jpegBytes);
    void onRecognitionRequestSucceeded(const QString &source, const QString &requestId,
                                       quint64 frameId,
                                  const QJsonObject &result, qint64 elapsedMs);
    void onRecognitionRequestFailed(const QString &source, const QString &requestId,
                                    quint64 frameId, const QString &message,
                                    int httpStatus, qint64 elapsedMs);
    void onSaveSucceeded(const QString&,const QString&,const QString&,bool);
    void onSaveFailed(const QString&,const QString&);
    void onActiveEventRecovered(const QJsonObject &event);
signals:
    void userExitRequested();

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
    void onAutoRecognitionTimeout();
    void updateAutoRecognitionTimer();
    void setPreviewUiState(PreviewUiState state);
    void updateVideoStatus();
    void applyManualResult(const AiResult &result, qint64 totalElapsedMs);
    void applyEventResult(const AiResult &result);
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
    ResultStorageClient *storageClient_;
    QPushButton *pauseButton_;
    QPushButton *recognizeButton_;
    QPushButton *saveResultButton_; QPushButton *exitButton_;

    QLabel *sceneValue_;
    QLabel *peopleValue_;
    QLabel *objectsValue_;
    QLabel *warningValue_;
    QLabel *summaryValue_;
    QLabel *eventValue_;

    QLabel *latencyValue_;
    QLabel *totalTokensValue_;
    QLabel *inputTokensValue_;
    QLabel *outputTokensValue_;
    QLabel *updatedValue_;
    QLabel *tcpValue_;
    QLabel *errorValue_;

    QTimer *clockTimer_;
    QTimer *autoRecognitionTimer_;
    int autoRecognitionIntervalMs_;
    bool legacyAutoRecognitionEnabled_;
    PreviewUiState previewUiState_;
    int previewStreamState_;
    bool sentinelActive_;
    qulonglong sourceFrameId_;
    qulonglong sourceTimestampNs_;
    FrozenFrameSnapshot requestSnapshot_;
    QString activeRequestSource_;
    QString activeRequestId_;
    QString lastAppliedRequestId_;
    quint64 activeRequestFrameId_;
    quint64 activeRequestTimestampNs_;
    quint64 manualRequestSequence_;
    quint64 autoRequestSequence_;
    QString saveSource_, saveRequestId_; bool exitConfirm_;
    QString currentEventId_;
    QString currentEventState_;
};

#endif
