#include "main_window.h"

// Qt 主窗口：组合实时预览、识别结果、手动识别、保存和相机诊断交互。

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include "ai_result_client.h"
#include "camera_debug_dialog.h"
#include "daemon_client.h"
#include "manual_recognition_client.h"
#include "result_storage_client.h"
#include "video_widget.h"

namespace {
QLabel *makeLabel(const QString &text, const QString &objectName, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setObjectName(objectName);
    return label;
}

void addResultField(QVBoxLayout *layout, const QString &caption, QLabel *value,
                    int maximumHeight = 48) {
    auto *captionLabel = makeLabel(caption, QStringLiteral("fieldCaption"), value->parentWidget());
    layout->addWidget(captionLabel);
    value->setWordWrap(true);
    value->setMaximumHeight(maximumHeight);
    layout->addWidget(value);
}

QString eventStateText(const QString &state) {
    if (state == QStringLiteral("active"))
        return QStringLiteral("进行中");
    if (state == QStringLiteral("ending"))
        return QStringLiteral("等待结束");
    if (state == QStringLiteral("ended"))
        return QStringLiteral("已结束");
    if (state == QStringLiteral("interrupted"))
        return QStringLiteral("已中断");
    return state.isEmpty() ? QStringLiteral("无事件") : state;
}

QString cloudStateText(const QString &state) {
    if (state == QStringLiteral("pending"))
        return QStringLiteral("等待");
    if (state == QStringLiteral("running"))
        return QStringLiteral("识别中");
    if (state == QStringLiteral("complete"))
        return QStringLiteral("已完成");
    if (state == QStringLiteral("failed"))
        return QStringLiteral("失败");
    return state.isEmpty() ? QStringLiteral("未请求") : state;
}

}  // namespace

MainWindow::MainWindow(ManualRecognitionClient *manualClient, ResultStorageClient *storageClient,
                       int autoRecognitionIntervalMs, bool legacyAutoRecognitionEnabled,
                       DaemonClient *daemonClient, QWidget *parent)
    : QWidget(parent),
      clockLabel_(nullptr),
      videoStatusLabel_(nullptr),
      aiStatusLabel_(nullptr),
      modelLabel_(nullptr),
      videoWidget_(nullptr),
      videoInfo_(nullptr),
      videoWaiting_(nullptr),
      manualClient_(manualClient),
      storageClient_(storageClient),
      pauseButton_(nullptr),
      recognizeButton_(nullptr),
      saveResultButton_(nullptr),
      exitButton_(nullptr),
      debugButton_(nullptr),
      daemonClient_(daemonClient),
      sceneValue_(nullptr),
      peopleValue_(nullptr),
      objectsValue_(nullptr),
      warningValue_(nullptr),
      summaryValue_(nullptr),
      eventValue_(nullptr),
      latencyValue_(nullptr),
      totalTokensValue_(nullptr),
      inputTokensValue_(nullptr),
      outputTokensValue_(nullptr),
      updatedValue_(nullptr),
      tcpValue_(nullptr),
      errorValue_(nullptr),
      clockTimer_(new QTimer(this)),
      autoRecognitionTimer_(new QTimer(this)),
      autoRecognitionIntervalMs_(autoRecognitionIntervalMs),
      legacyAutoRecognitionEnabled_(legacyAutoRecognitionEnabled),
      previewUiState_(PreviewUiState::Live),
      previewStreamState_(0),
      sentinelActive_(false),
      sourceFrameId_(0),
      sourceTimestampNs_(0),
      activeRequestFrameId_(0),
      activeRequestTimestampNs_(0),
      manualRequestSequence_(0),
      autoRequestSequence_(0),
      exitConfirm_(false) {
    setObjectName(QStringLiteral("root"));
    setWindowFlags(Qt::FramelessWindowHint);
    setFixedSize(720, 720);
    buildUi();
    applyStyle();
    showDefaultState();

    connect(clockTimer_, &QTimer::timeout, this, &MainWindow::updateCurrentTime);
    clockTimer_->start(1000);
    autoRecognitionTimer_->setInterval(autoRecognitionIntervalMs_);
    connect(autoRecognitionTimer_, &QTimer::timeout, this, &MainWindow::onAutoRecognitionTimeout);
    connect(storageClient_, &ResultStorageClient::activeEventRecovered, this,
            &MainWindow::onActiveEventRecovered);
    updateCurrentTime();
}

void MainWindow::buildUi() {
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 16, 16, 16);
    rootLayout->setSpacing(14);

    auto *topBar = new QFrame(this);
    topBar->setObjectName(QStringLiteral("topBar"));
    topBar->setFixedHeight(82);
    auto *topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(18, 10, 18, 10);
    topLayout->setSpacing(14);

    auto *title = makeLabel(QStringLiteral("智能视觉终端"), QStringLiteral("mainTitle"), topBar);
    title->setFixedWidth(190);
    topLayout->addWidget(title);

    auto *topInfoLayout = new QVBoxLayout;
    topInfoLayout->setContentsMargins(0, 0, 0, 0);
    topInfoLayout->setSpacing(7);
    auto *statusLayout = new QHBoxLayout;
    statusLayout->setContentsMargins(0, 0, 0, 0);
    statusLayout->setSpacing(12);
    videoStatusLabel_ = makeLabel(QString(), QStringLiteral("statusLabel"), topBar);
    aiStatusLabel_ = makeLabel(QString(), QStringLiteral("statusLabel"), topBar);
    statusLayout->addWidget(videoStatusLabel_);
    statusLayout->addWidget(aiStatusLabel_);
    statusLayout->addStretch();

    auto *metaLayout = new QHBoxLayout;
    metaLayout->setContentsMargins(0, 0, 0, 0);
    metaLayout->setSpacing(8);
    modelLabel_ = makeLabel(QString(), QStringLiteral("metaLabel"), topBar);
    clockLabel_ = makeLabel(QString(), QStringLiteral("clockLabel"), topBar);
    clockLabel_->setFixedWidth(126);
    clockLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    metaLayout->addWidget(modelLabel_, 1);
    metaLayout->addWidget(clockLabel_);
    topInfoLayout->addLayout(statusLayout);
    topInfoLayout->addLayout(metaLayout);
    topLayout->addLayout(topInfoLayout, 1);
    rootLayout->addWidget(topBar);

    auto *middleLayout = new QHBoxLayout;
    middleLayout->setContentsMargins(0, 0, 0, 0);
    middleLayout->setSpacing(12);

    auto *videoPanel = new QFrame(this);
    videoPanel->setObjectName(QStringLiteral("panel"));
    videoPanel->setFixedSize(408, 454);
    auto *videoLayout = new QVBoxLayout(videoPanel);
    videoLayout->setContentsMargins(14, 12, 14, 12);
    videoLayout->setSpacing(6);
    auto *videoHeading = new QHBoxLayout;
    videoHeading->setContentsMargins(0, 0, 0, 0);
    videoHeading->addWidget(
        makeLabel(QStringLiteral("摄像头预览"), QStringLiteral("sectionTitle"), videoPanel));
    videoHeading->addStretch();
    debugButton_ = new QPushButton(QStringLiteral("参数 / 诊断"), videoPanel);
    debugButton_->setObjectName(QStringLiteral("debugButton"));
    debugButton_->setFixedSize(120, 34);
    debugButton_->setFocusPolicy(Qt::NoFocus);
    connect(debugButton_, &QPushButton::clicked, this, [this]() {
        CameraDebugDialog dialog(daemonClient_, this);
        dialog.exec();
    });
    videoHeading->addWidget(debugButton_);
    videoLayout->addLayout(videoHeading);

    videoWidget_ = new VideoWidget(videoPanel);
    videoLayout->addWidget(videoWidget_, 0, Qt::AlignHCenter);

    videoInfo_ = makeLabel(QStringLiteral("预览等待"), QStringLiteral("videoInfo"), videoPanel);
    videoInfo_->setAlignment(Qt::AlignCenter);
    videoLayout->addWidget(videoInfo_);
    videoWaiting_ =
        makeLabel(QStringLiteral("视频输入：离线"), QStringLiteral("videoWaiting"), videoPanel);
    videoWaiting_->setAlignment(Qt::AlignCenter);
    videoLayout->addWidget(videoWaiting_);

    auto *buttonLayout = new QGridLayout;
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setHorizontalSpacing(12);
    buttonLayout->setVerticalSpacing(8);
    pauseButton_ = new QPushButton(QStringLiteral("暂停"), videoPanel);
    pauseButton_->setObjectName(QStringLiteral("pauseButton"));
    pauseButton_->setFixedSize(184, 48);
    pauseButton_->setCheckable(true);
    pauseButton_->setFocusPolicy(Qt::NoFocus);
    pauseButton_->setAutoRepeat(false);
    connect(pauseButton_, &QPushButton::toggled, this, &MainWindow::onPauseToggled);
    buttonLayout->addWidget(pauseButton_, 0, 0);

    recognizeButton_ = new QPushButton(QStringLiteral("识别"), videoPanel);
    recognizeButton_->setObjectName(QStringLiteral("recognizeButton"));
    recognizeButton_->setFixedSize(184, 48);
    recognizeButton_->setFocusPolicy(Qt::NoFocus);
    recognizeButton_->setAutoRepeat(false);
    recognizeButton_->setEnabled(false);
    recognizeButton_->setToolTip(QStringLiteral("请先暂停当前画面"));
    connect(recognizeButton_, &QPushButton::clicked, this, &MainWindow::onRecognizeClicked);
    buttonLayout->addWidget(recognizeButton_, 0, 1);
    saveResultButton_ = new QPushButton(QStringLiteral("保存结果"), videoPanel);
    saveResultButton_->setObjectName(QStringLiteral("saveResultButton"));
    saveResultButton_->setFixedSize(184, 48);
    saveResultButton_->setEnabled(false);
    saveResultButton_->setFocusPolicy(Qt::NoFocus);
    connect(saveResultButton_, &QPushButton::clicked, this, [this]() {
        const bool started =
            storageClient_ &&
            (!currentEventId_.isEmpty() ? storageClient_->saveEvent(currentEventId_)
                                        : storageClient_->save(saveSource_, saveRequestId_));
        if (started) {
            saveResultButton_->setText(QStringLiteral("保存中…"));
            saveResultButton_->setEnabled(false);
        }
    });
    buttonLayout->addWidget(saveResultButton_, 1, 0);

    exitButton_ = new QPushButton(QStringLiteral("退出"), videoPanel);
    exitButton_->setObjectName(QStringLiteral("exitButton"));
    exitButton_->setFixedSize(184, 48);
    exitButton_->setFocusPolicy(Qt::NoFocus);
    connect(exitButton_, &QPushButton::clicked, this, [this]() {
        if (exitConfirm_) {
            if (manualClient_)
                manualClient_->abort();
            if (storageClient_)
                storageClient_->abort();
            emit userExitRequested();
            return;
        }
        exitConfirm_ = true;
        exitButton_->setText(QStringLiteral("确认退出"));
        exitButton_->setProperty("confirm", true);
        exitButton_->style()->unpolish(exitButton_);
        exitButton_->style()->polish(exitButton_);
        QTimer::singleShot(3000, this, [this]() {
            if (!exitConfirm_)
                return;
            exitConfirm_ = false;
            exitButton_->setText(QStringLiteral("退出"));
            exitButton_->setProperty("confirm", false);
            exitButton_->style()->unpolish(exitButton_);
            exitButton_->style()->polish(exitButton_);
        });
    });
    buttonLayout->addWidget(exitButton_, 1, 1);
    videoLayout->addLayout(buttonLayout);
    middleLayout->addWidget(videoPanel);

    auto *resultPanel = new QFrame(this);
    resultPanel->setObjectName(QStringLiteral("panel"));
    resultPanel->setFixedSize(268, 454);
    auto *resultLayout = new QVBoxLayout(resultPanel);
    resultLayout->setContentsMargins(13, 12, 13, 12);
    resultLayout->setSpacing(4);
    resultLayout->addWidget(
        makeLabel(QStringLiteral("识别结果"), QStringLiteral("sectionTitle"), resultPanel));

    sceneValue_ = makeLabel(QString(), QStringLiteral("resultPrimary"), resultPanel);
    peopleValue_ = makeLabel(QString(), QStringLiteral("fieldValue"), resultPanel);
    objectsValue_ = makeLabel(QString(), QStringLiteral("resultDetail"), resultPanel);
    warningValue_ = makeLabel(QString(), QStringLiteral("alertValue"), resultPanel);
    summaryValue_ = makeLabel(QString(), QStringLiteral("summaryValue"), resultPanel);
    eventValue_ = makeLabel(QString(), QStringLiteral("eventValue"), resultPanel);
    eventValue_->setMaximumHeight(22);
    resultLayout->addWidget(eventValue_);
    addResultField(resultLayout, QStringLiteral("场景"), sceneValue_, 48);

    auto *resultStatusLayout = new QHBoxLayout;
    resultStatusLayout->setContentsMargins(0, 0, 0, 0);
    resultStatusLayout->setSpacing(14);
    auto addCompactField = [resultPanel, resultStatusLayout](const QString &caption,
                                                             QLabel *value) {
        auto *field = new QWidget(resultPanel);
        auto *fieldLayout = new QVBoxLayout(field);
        fieldLayout->setContentsMargins(0, 0, 0, 0);
        fieldLayout->setSpacing(0);
        fieldLayout->addWidget(makeLabel(caption, QStringLiteral("fieldCaption"), field));
        fieldLayout->addWidget(value);
        resultStatusLayout->addWidget(field, 1);
    };
    addCompactField(QStringLiteral("人数"), peopleValue_);
    addCompactField(QStringLiteral("状态"), warningValue_);
    resultLayout->addLayout(resultStatusLayout);

    addResultField(resultLayout, QStringLiteral("物品"), objectsValue_, 32);
    addResultField(resultLayout, QStringLiteral("识别摘要"), summaryValue_, 68);
    resultLayout->addStretch();
    middleLayout->addWidget(resultPanel);
    rootLayout->addLayout(middleLayout);

    auto *bottomBar = new QFrame(this);
    bottomBar->setObjectName(QStringLiteral("bottomBar"));
    bottomBar->setFixedHeight(124);
    auto *bottomLayout = new QVBoxLayout(bottomBar);
    bottomLayout->setContentsMargins(14, 10, 14, 9);
    bottomLayout->setSpacing(7);
    auto *metricLayout = new QHBoxLayout;
    metricLayout->setContentsMargins(0, 0, 0, 0);
    metricLayout->setSpacing(9);

    auto makeMetric = [bottomBar](const QString &caption, QLabel *&value) {
        auto *widget = new QWidget(bottomBar);
        auto *layout = new QVBoxLayout(widget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(1);
        auto *captionLabel = makeLabel(caption, QStringLiteral("metricCaption"), widget);
        value = makeLabel(QString(), QStringLiteral("metricValue"), widget);
        layout->addWidget(captionLabel);
        layout->addWidget(value);
        return widget;
    };

    metricLayout->addWidget(makeMetric(QStringLiteral("API 延迟"), latencyValue_), 1);
    metricLayout->addWidget(makeMetric(QStringLiteral("总 Token"), totalTokensValue_), 1);
    metricLayout->addWidget(makeMetric(QStringLiteral("输入 Token"), inputTokensValue_), 1);
    metricLayout->addWidget(makeMetric(QStringLiteral("输出 Token"), outputTokensValue_), 1);
    metricLayout->addWidget(makeMetric(QStringLiteral("更新时间"), updatedValue_), 1);
    bottomLayout->addLayout(metricLayout);

    auto *connectionLayout = new QHBoxLayout;
    connectionLayout->setContentsMargins(0, 0, 0, 0);
    connectionLayout->setSpacing(12);
    tcpValue_ = makeLabel(QString(), QStringLiteral("tcpValue"), bottomBar);
    tcpValue_->setFixedWidth(190);
    errorValue_ = makeLabel(QString(), QStringLiteral("errorValue"), bottomBar);
    errorValue_->setWordWrap(true);
    connectionLayout->addWidget(tcpValue_);
    connectionLayout->addWidget(errorValue_, 1);
    bottomLayout->addLayout(connectionLayout);
    rootLayout->addWidget(bottomBar);
}

void MainWindow::applyStyle() {
    QFile styleFile(QStringLiteral(":/style.qss"));
    if (styleFile.open(QIODevice::ReadOnly | QIODevice::Text))
        setStyleSheet(QString::fromUtf8(styleFile.readAll()));
}

void MainWindow::showDefaultState() {
    const AiResult result;
    sceneValue_->setText(result.scene);
    peopleValue_->setText(QString::number(result.peopleCount));
    objectsValue_->setText(QStringLiteral("无"));
    summaryValue_->setText(result.summary);
    eventValue_->setText(QStringLiteral("等待检测事件"));
    modelLabel_->setText(QStringLiteral("模型  %1").arg(result.model));
    latencyValue_->setText(QStringLiteral("-- ms"));
    totalTokensValue_->setText(QStringLiteral("0"));
    inputTokensValue_->setText(QStringLiteral("0"));
    outputTokensValue_->setText(QStringLiteral("0"));
    updatedValue_->setText(QStringLiteral("--:--:--"));
    errorValue_->setText(QStringLiteral("最近错误：无"));
    setStateLabel(videoStatusLabel_, QStringLiteral("视频离线"), QStringLiteral("offline"));
    setStateLabel(aiStatusLabel_, QStringLiteral("AI 离线"), QStringLiteral("offline"));
    setStateLabel(warningValue_, QStringLiteral("正常"), QStringLiteral("normal"));
    setStateLabel(tcpValue_, QStringLiteral("TCP 未连接"), QStringLiteral("offline"));
    setPreviewUiState(PreviewUiState::Live);
}

void MainWindow::updateAiResult(const AiResult &result) {
    if (result.hasSentinelState) {
        sentinelActive_ = result.sentinelActive;
        updateAutoRecognitionTimer();
    }
    if (result.messageType.startsWith(QStringLiteral("event."))) {
        applyEventResult(result);
        return;
    }
    if (result.messageType == QStringLiteral("health")) {
        updateError(QStringLiteral("事件服务报告检测链路异常"));
        return;
    }
    if (result.source == QStringLiteral("manual")) {
        if (result.requestId.isEmpty() || result.requestId == lastAppliedRequestId_ ||
            result.requestId != activeRequestId_ || result.frameId != activeRequestFrameId_) {
            return;
        }
        applyManualResult(result, result.serverLatencyMs);
        return;
    }
    if (result.source == QStringLiteral("local"))
        return;
    if (previewUiState_ != PreviewUiState::Live)
        return;
    if ((result.source == QStringLiteral("auto") || result.source == QStringLiteral("manual")) &&
        !result.requestId.isEmpty()) {
        saveSource_ = result.source;
        saveRequestId_ = result.requestId;
        saveResultButton_->setText(QStringLiteral("保存结果"));
        saveResultButton_->setEnabled(true);
    }
    sceneValue_->setText(result.scene);
    peopleValue_->setText(QString::number(result.peopleCount));
    objectsValue_->setText(result.objects.isEmpty() ? QStringLiteral("无")
                                                    : result.objects.join(QStringLiteral(", ")));
    summaryValue_->setText(result.summary);
    modelLabel_->setText(QStringLiteral("模型  %1").arg(result.model));
    latencyValue_->setText(QStringLiteral("%1 ms").arg(result.latencyMs));
    totalTokensValue_->setText(QString::number(result.totalTokens));
    inputTokensValue_->setText(QString::number(result.inputTokens));
    outputTokensValue_->setText(QString::number(result.outputTokens));

    const QDateTime parsedTime = QDateTime::fromString(result.timestamp, Qt::ISODate);
    updatedValue_->setText(parsedTime.isValid() ? parsedTime.toString(QStringLiteral("HH:mm:ss"))
                                                : result.timestamp.right(8));
    setStateLabel(warningValue_, result.warning ? QStringLiteral("告警") : QStringLiteral("正常"),
                  result.warning ? QStringLiteral("warning") : QStringLiteral("normal"));

    if (!result.success && !result.errorMessage.isEmpty())
        updateError(result.errorMessage);
    else
        errorValue_->setText(QStringLiteral("最近错误：无"));
}

void MainWindow::updateConnectionState(const QString &state) {
    QString visualState = QStringLiteral("offline");
    QString displayState = QStringLiteral("未连接");
    if (state == QStringLiteral("Connected")) {
        visualState = QStringLiteral("online");
        displayState = QStringLiteral("已连接");
        if (storageClient_)
            storageClient_->recoverActiveEvent();
    } else if (state == QStringLiteral("Connecting")) {
        visualState = QStringLiteral("waiting");
        displayState = QStringLiteral("连接中");
    } else if (state == QStringLiteral("Error")) {
        visualState = QStringLiteral("warning");
        displayState = QStringLiteral("错误");
    }
    setStateLabel(tcpValue_, QStringLiteral("TCP %1").arg(displayState), visualState);
}

void MainWindow::updateCurrentTime() {
    clockLabel_->setText(QDateTime::currentDateTime().toString(QStringLiteral("MM-dd HH:mm:ss")));
}

void MainWindow::updateAiState(const QString &state) {
    QString visualState = QStringLiteral("offline");
    QString displayState = QStringLiteral("AI 离线");
    if (state == QStringLiteral("AI ONLINE")) {
        visualState = QStringLiteral("online");
        displayState = QStringLiteral("AI 在线");
    } else if (state == QStringLiteral("AI STALE")) {
        visualState = QStringLiteral("waiting");
        displayState = QStringLiteral("AI 超时");
    }
    setStateLabel(aiStatusLabel_, displayState, visualState);
}

void MainWindow::updateError(const QString &message) {
    errorValue_->setText(QStringLiteral("最近错误：%1").arg(message.left(100)));
}

void MainWindow::updatePreviewFrame(const QImage &image, qulonglong frameId,
                                    qulonglong sourceTimeNs) {
    sourceFrameId_ = frameId;
    sourceTimestampNs_ = sourceTimeNs;
    videoWidget_->setFrame(image, frameId, sourceTimeNs);
}

void MainWindow::updatePreviewState(int state) {
    previewStreamState_ = state;
    updateVideoStatus();
    videoWidget_->setState(state);
    updateAutoRecognitionTimer();
}

void MainWindow::updatePreviewStats(const QString &line1, const QString &line2) {
    videoInfo_->setText(line1);
    if (previewUiState_ == PreviewUiState::Frozen) {
        videoWaiting_->setText(QStringLiteral("暂停帧 %1   后台最新帧 %2")
                                   .arg(videoWidget_->frozenFrameId())
                                   .arg(sourceFrameId_));
    } else {
        videoWaiting_->setText(line2);
    }
}

void MainWindow::onPauseToggled(bool checked) {
    if (checked) {
        if (!videoWidget_->freezeCurrentFrame()) {
            QSignalBlocker blocker(pauseButton_);
            pauseButton_->setChecked(false);
            updateError(QStringLiteral("暂无可暂停画面"));
            setPreviewUiState(PreviewUiState::Live);
            return;
        }
        setPreviewUiState(PreviewUiState::Frozen);
        return;
    }

    videoWidget_->resumeLivePreview();
    setPreviewUiState(PreviewUiState::Live);
}

void MainWindow::onRecognizeClicked() {
    if (!manualClient_ || manualClient_->isBusy() ||
        (previewUiState_ != PreviewUiState::Frozen &&
         previewUiState_ != PreviewUiState::ResultReady &&
         previewUiState_ != PreviewUiState::Error)) {
        return;
    }

    requestSnapshot_.image = videoWidget_->frozenImageCopy();
    requestSnapshot_.frameId = videoWidget_->frozenFrameId();
    requestSnapshot_.timestampNs = videoWidget_->frozenTimestampNs();
    if (!requestSnapshot_.isValid()) {
        updateError(QStringLiteral("冻结画面无效，无法识别"));
        setPreviewUiState(PreviewUiState::Error);
        return;
    }

    ++manualRequestSequence_;
    activeRequestId_ = QStringLiteral("manual-%1-%2")
                           .arg(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch())
                           .arg(manualRequestSequence_, 4, 10, QLatin1Char('0'));
    activeRequestSource_ = QStringLiteral("manual");
    activeRequestFrameId_ = requestSnapshot_.frameId;
    activeRequestTimestampNs_ = requestSnapshot_.timestampNs;
    setPreviewUiState(PreviewUiState::Recognizing);
    if (!manualClient_->recognize(activeRequestSource_, requestSnapshot_.image, activeRequestId_,
                                  activeRequestFrameId_, activeRequestTimestampNs_)) {
        updateError(QStringLiteral("冻结画面编码失败或识别服务不可用"));
        setPreviewUiState(PreviewUiState::Error);
    }
}

void MainWindow::onAutoRecognitionTimeout() {
    if (!sentinelActive_ || previewUiState_ != PreviewUiState::Live || previewStreamState_ != 2 ||
        !manualClient_ || manualClient_->isBusy()) {
        return;
    }

    const QImage image = videoWidget_->displayedLiveImageCopy();
    const quint64 frameId = videoWidget_->displayedLiveFrameId();
    const quint64 timestampNs = videoWidget_->displayedLiveTimestampNs();
    if (image.isNull() || frameId == 0)
        return;

    ++autoRequestSequence_;
    activeRequestSource_ = QStringLiteral("auto");
    activeRequestId_ = QStringLiteral("auto-%1-%2")
                           .arg(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch())
                           .arg(autoRequestSequence_, 4, 10, QLatin1Char('0'));
    activeRequestFrameId_ = frameId;
    activeRequestTimestampNs_ = timestampNs;
    if (!manualClient_->recognize(activeRequestSource_, image, activeRequestId_,
                                  activeRequestFrameId_, activeRequestTimestampNs_)) {
        updateError(QStringLiteral("自动识别帧编码失败或识别服务不可用"));
    }
}

void MainWindow::updateAutoRecognitionTimer() {
    const bool shouldRun = legacyAutoRecognitionEnabled_ && sentinelActive_ &&
                           previewUiState_ == PreviewUiState::Live && previewStreamState_ == 2;
    if (shouldRun) {
        if (!autoRecognitionTimer_->isActive())
            autoRecognitionTimer_->start(autoRecognitionIntervalMs_);
    } else {
        autoRecognitionTimer_->stop();
    }
}

void MainWindow::applyEventResult(const AiResult &result) {
    if (result.eventId.isEmpty())
        return;
    currentEventId_ = result.eventId;
    currentEventState_ = result.eventState;
    eventValue_->setText(
        QStringLiteral("事件%1 · 云端%2")
            .arg(eventStateText(result.eventState), cloudStateText(result.cloudState)));
    peopleValue_->setText(
        result.maxPeople > result.currentPeople
            ? QStringLiteral("%1 人（最高 %2）").arg(result.currentPeople).arg(result.maxPeople)
            : QStringLiteral("%1 人").arg(result.currentPeople));
    if (!result.summary.isEmpty() && result.summary != QStringLiteral("暂无识别结果")) {
        summaryValue_->setText(result.summary);
        const QString warningText =
            result.warning ? QStringLiteral("告警%1").arg(
                                 result.warningReason.isEmpty()
                                     ? QString()
                                     : QStringLiteral(" · %1").arg(result.warningReason))
                           : QStringLiteral("正常");
        setStateLabel(warningValue_, warningText,
                      result.warning ? QStringLiteral("warning") : QStringLiteral("normal"));
    }
    saveSource_ = QStringLiteral("event");
    saveRequestId_ = result.eventId;
    saveResultButton_->setText(QStringLiteral("保存事件"));
    saveResultButton_->setEnabled(result.bestFrameId > 0 && storageClient_ &&
                                  !storageClient_->isBusy());
}

void MainWindow::onActiveEventRecovered(const QJsonObject &event) {
    AiResult result;
    result.schemaVersion = 1;
    result.messageType = QStringLiteral("event.update");
    result.eventId = event.value(QStringLiteral("event_id")).toString();
    result.eventState = event.value(QStringLiteral("state")).toString();
    result.currentPeople = event.value(QStringLiteral("current_people")).toInt();
    result.maxPeople = event.value(QStringLiteral("max_people")).toInt();
    result.trackCount = event.value(QStringLiteral("track_count")).toInt();
    result.durationMs = static_cast<qint64>(event.value(QStringLiteral("duration_ms")).toDouble());
    result.bestFrameId =
        static_cast<quint64>(event.value(QStringLiteral("best_frame_id")).toDouble());
    result.cloudState = event.value(QStringLiteral("cloud_state")).toString();
    result.summary = event.value(QStringLiteral("summary")).toString();
    result.warning = event.value(QStringLiteral("warning")).toBool(false);
    result.warningReason = event.value(QStringLiteral("warning_reason")).toString();
    applyEventResult(result);
}

void MainWindow::onRecognitionRequestStarted(const QString &source, const QString &requestId,
                                             quint64 frameId, int jpegBytes) {
    Q_UNUSED(jpegBytes)
    if (source != activeRequestSource_ || requestId != activeRequestId_ ||
        frameId != activeRequestFrameId_) {
        return;
    }
    if (source == QStringLiteral("auto")) {
        errorValue_->setText(QStringLiteral("自动识别中：帧 %1").arg(frameId));
        return;
    }
    videoWaiting_->setText(QStringLiteral("暂停帧 %1   正在识别").arg(frameId));
}

void MainWindow::onRecognitionRequestSucceeded(const QString &source, const QString &requestId,
                                               quint64 frameId, const QJsonObject &document,
                                               qint64 elapsedMs) {
    if (source != activeRequestSource_ || requestId != activeRequestId_ ||
        frameId != activeRequestFrameId_ ||
        document.value(QStringLiteral("request_id")).toString() != requestId ||
        static_cast<quint64>(document.value(QStringLiteral("frame_id")).toDouble()) != frameId) {
        return;
    }
    AiResult result;
    QString error;
    if (!AiResultClient::parseMessage(QJsonDocument(document).toJson(QJsonDocument::Compact),
                                      result, error) ||
        !result.success) {
        onRecognitionRequestFailed(source, requestId, frameId, QStringLiteral("识别结果格式错误"),
                                   0, elapsedMs);
        return;
    }
    if (source == QStringLiteral("auto")) {
        if (previewUiState_ == PreviewUiState::Live) {
            updateAiResult(result);
            errorValue_->setText(QStringLiteral("自动识别完成：%1 ms").arg(elapsedMs));
        } else {
            setPreviewUiState(previewUiState_);
        }
        activeRequestSource_.clear();
        return;
    }
    applyManualResult(result, elapsedMs);
    activeRequestSource_.clear();
}

void MainWindow::onRecognitionRequestFailed(const QString &source, const QString &requestId,
                                            quint64 frameId, const QString &message, int httpStatus,
                                            qint64 elapsedMs) {
    if (source != activeRequestSource_ || requestId != activeRequestId_ ||
        frameId != activeRequestFrameId_) {
        return;
    }
    if (source == QStringLiteral("auto")) {
        if (previewUiState_ == PreviewUiState::Live) {
            updateError(
                httpStatus > 0
                    ? QStringLiteral("自动识别失败（HTTP %1）：%2").arg(httpStatus).arg(message)
                    : QStringLiteral("自动识别失败：%1").arg(message));
        } else {
            setPreviewUiState(previewUiState_);
        }
        activeRequestSource_.clear();
        return;
    }
    updateError(httpStatus > 0
                    ? QStringLiteral("识别失败（HTTP %1）：%2").arg(httpStatus).arg(message)
                    : QStringLiteral("识别失败：%1").arg(message));
    videoWaiting_->setText(
        QStringLiteral("暂停帧 %1   请求耗时 %2 ms").arg(frameId).arg(elapsedMs));
    setPreviewUiState(PreviewUiState::Error);
    activeRequestSource_.clear();
}

void MainWindow::setPreviewUiState(PreviewUiState state) {
    previewUiState_ = state;
    const bool frozen = state != PreviewUiState::Live;
    {
        QSignalBlocker blocker(pauseButton_);
        pauseButton_->setChecked(frozen);
    }
    pauseButton_->setText(frozen ? QStringLiteral("继续") : QStringLiteral("暂停"));
    pauseButton_->setEnabled(state != PreviewUiState::Recognizing);
    const bool recognitionBusy = manualClient_ && manualClient_->isBusy();
    recognizeButton_->setEnabled(!recognitionBusy && (state == PreviewUiState::Frozen ||
                                                      state == PreviewUiState::ResultReady ||
                                                      state == PreviewUiState::Error));
    if (state == PreviewUiState::Recognizing)
        recognizeButton_->setText(QStringLiteral("识别中…"));
    else if (state == PreviewUiState::ResultReady)
        recognizeButton_->setText(QStringLiteral("重新识别"));
    else if (state == PreviewUiState::Error)
        recognizeButton_->setText(QStringLiteral("重试识别"));
    else if (recognitionBusy && activeRequestSource_ == QStringLiteral("auto"))
        recognizeButton_->setText(QStringLiteral("自动识别中…"));
    else
        recognizeButton_->setText(QStringLiteral("识别"));
    updateVideoStatus();

    if (frozen) {
        videoWaiting_->setText(QStringLiteral("暂停帧 %1   后台最新帧 %2")
                                   .arg(videoWidget_->frozenFrameId())
                                   .arg(sourceFrameId_));
    }
    updateAutoRecognitionTimer();
}

void MainWindow::updateVideoStatus() {
    if (previewUiState_ != PreviewUiState::Live) {
        setStateLabel(videoStatusLabel_, QStringLiteral("已暂停"), QStringLiteral("waiting"));
        return;
    }

    QString text = QStringLiteral("视频离线");
    QString visualState = QStringLiteral("offline");
    if (previewStreamState_ == 2) {
        text = QStringLiteral("实时");
        visualState = QStringLiteral("online");
    } else if (previewStreamState_ == 1) {
        text = QStringLiteral("视频超时");
        visualState = QStringLiteral("waiting");
    }
    setStateLabel(videoStatusLabel_, text, visualState);
}

void MainWindow::applyManualResult(const AiResult &result, qint64 totalElapsedMs) {
    if (result.requestId != activeRequestId_ || result.frameId != activeRequestFrameId_ ||
        result.requestId == lastAppliedRequestId_) {
        return;
    }
    lastAppliedRequestId_ = result.requestId;
    saveSource_ = result.source;
    saveRequestId_ = result.requestId;
    saveResultButton_->setText(currentEventId_.isEmpty() ? QStringLiteral("保存结果")
                                                         : QStringLiteral("保存事件"));
    saveResultButton_->setEnabled(saveSource_ == QStringLiteral("manual") ||
                                  saveSource_ == QStringLiteral("auto"));
    sceneValue_->setText(result.scene);
    peopleValue_->setText(QString::number(result.peopleCount));
    objectsValue_->setText(result.objects.isEmpty() ? QStringLiteral("无")
                                                    : result.objects.join(QStringLiteral(", ")));
    summaryValue_->setText(result.summary);
    modelLabel_->setText(QStringLiteral("手动识别  帧 %1").arg(result.frameId));
    latencyValue_->setText(QStringLiteral("%1 ms").arg(result.latencyMs));
    totalTokensValue_->setText(QString::number(result.totalTokens));
    inputTokensValue_->setText(QString::number(result.inputTokens));
    outputTokensValue_->setText(QString::number(result.outputTokens));
    updatedValue_->setText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")));
    setStateLabel(warningValue_, result.warning ? QStringLiteral("告警") : QStringLiteral("正常"),
                  result.warning ? QStringLiteral("warning") : QStringLiteral("normal"));
    errorValue_->setText(QStringLiteral("手动识别完成：%1 ms").arg(totalElapsedMs));
    videoWaiting_->setText(QStringLiteral("暂停帧 %1   识别完成").arg(result.frameId));
    setPreviewUiState(PreviewUiState::ResultReady);
}

void MainWindow::onSaveSucceeded(const QString &, const QString &, const QString &, bool) {
    saveResultButton_->setText(QStringLiteral("已保存"));
    saveResultButton_->setEnabled(false);
}
void MainWindow::onSaveFailed(const QString &, const QString &message) {
    saveResultButton_->setText(QStringLiteral("重新保存"));
    saveResultButton_->setEnabled(true);
    updateError(message);
}

void MainWindow::setStateLabel(QLabel *label, const QString &text, const QString &state) {
    label->setText(text);
    label->setProperty("state", QVariant(state));
    label->style()->unpolish(label);
    label->style()->polish(label);
    label->update();
}
