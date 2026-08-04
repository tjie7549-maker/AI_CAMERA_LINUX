#include "main_window.h"
#include "video_widget.h"

#include <QDateTime>
#include <QFile>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>

namespace {
QLabel *makeLabel(const QString &text, const QString &objectName,
                  QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(objectName);
    return label;
}

void addResultField(QVBoxLayout *layout, const QString &caption,
                    QLabel *value, int maximumHeight = 48)
{
    auto *captionLabel = makeLabel(caption, QStringLiteral("fieldCaption"),
                                   value->parentWidget());
    layout->addWidget(captionLabel);
    value->setWordWrap(true);
    value->setMaximumHeight(maximumHeight);
    layout->addWidget(value);
}
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent),
      clockLabel_(nullptr),
      videoStatusLabel_(nullptr),
      aiStatusLabel_(nullptr),
      modelLabel_(nullptr),
      videoWidget_(nullptr),
      videoInfo_(nullptr),
      videoWaiting_(nullptr),
      sceneValue_(nullptr),
      peopleValue_(nullptr),
      objectsValue_(nullptr),
      warningValue_(nullptr),
      warningReasonValue_(nullptr),
      summaryValue_(nullptr),
      latencyValue_(nullptr),
      totalTokensValue_(nullptr),
      inputTokensValue_(nullptr),
      outputTokensValue_(nullptr),
      updatedValue_(nullptr),
      tcpValue_(nullptr),
      errorValue_(nullptr),
      clockTimer_(new QTimer(this))
{
    setObjectName(QStringLiteral("root"));
    setWindowFlags(Qt::FramelessWindowHint);
    setFixedSize(720, 720);
    buildUi();
    applyStyle();
    showDefaultState();

    connect(clockTimer_, &QTimer::timeout,
            this, &MainWindow::updateCurrentTime);
    clockTimer_->start(1000);
    updateCurrentTime();
}

void MainWindow::buildUi()
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 16, 16, 16);
    rootLayout->setSpacing(14);

    auto *topBar = new QFrame(this);
    topBar->setObjectName(QStringLiteral("topBar"));
    topBar->setFixedHeight(82);
    auto *topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(18, 10, 18, 10);
    topLayout->setSpacing(14);

    auto *title = makeLabel(QStringLiteral("智能视觉终端"),
                            QStringLiteral("mainTitle"), topBar);
    title->setFixedWidth(286);
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
    videoLayout->setSpacing(9);
    videoLayout->addWidget(makeLabel(QStringLiteral("摄像头预览"),
                                     QStringLiteral("sectionTitle"), videoPanel));

    videoWidget_ = new VideoWidget(videoPanel);
    videoLayout->addWidget(videoWidget_, 0, Qt::AlignHCenter);

    videoInfo_ = makeLabel(QStringLiteral("预览等待"),
                           QStringLiteral("videoInfo"), videoPanel);
    videoInfo_->setAlignment(Qt::AlignCenter);
    videoLayout->addWidget(videoInfo_);
    videoWaiting_ = makeLabel(QStringLiteral("视频输入：离线"),
                              QStringLiteral("videoWaiting"), videoPanel);
    videoWaiting_->setAlignment(Qt::AlignCenter);
    videoLayout->addWidget(videoWaiting_);
    videoLayout->addStretch();
    middleLayout->addWidget(videoPanel);

    auto *resultPanel = new QFrame(this);
    resultPanel->setObjectName(QStringLiteral("panel"));
    resultPanel->setFixedSize(268, 454);
    auto *resultLayout = new QVBoxLayout(resultPanel);
    resultLayout->setContentsMargins(13, 12, 13, 12);
    resultLayout->setSpacing(2);
    resultLayout->addWidget(makeLabel(QStringLiteral("识别结果"),
                                      QStringLiteral("sectionTitle"), resultPanel));

    sceneValue_ = makeLabel(QString(), QStringLiteral("fieldValue"), resultPanel);
    peopleValue_ = makeLabel(QString(), QStringLiteral("fieldValue"), resultPanel);
    objectsValue_ = makeLabel(QString(), QStringLiteral("fieldValue"), resultPanel);
    warningValue_ = makeLabel(QString(), QStringLiteral("alertValue"), resultPanel);
    warningReasonValue_ = makeLabel(QString(), QStringLiteral("fieldValue"), resultPanel);
    summaryValue_ = makeLabel(QString(), QStringLiteral("summaryValue"), resultPanel);
    addResultField(resultLayout, QStringLiteral("场景"), sceneValue_, 42);
    addResultField(resultLayout, QStringLiteral("人数"), peopleValue_, 30);
    addResultField(resultLayout, QStringLiteral("物体"), objectsValue_, 54);
    addResultField(resultLayout, QStringLiteral("告警"), warningValue_, 30);
    addResultField(resultLayout, QStringLiteral("原因"), warningReasonValue_, 48);
    addResultField(resultLayout, QStringLiteral("摘要"), summaryValue_, 72);
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

void MainWindow::applyStyle()
{
    QFile styleFile(QStringLiteral(":/style.qss"));
    if (styleFile.open(QIODevice::ReadOnly | QIODevice::Text))
        setStyleSheet(QString::fromUtf8(styleFile.readAll()));
}

void MainWindow::showDefaultState()
{
    const AiResult result;
    sceneValue_->setText(result.scene);
    peopleValue_->setText(QString::number(result.peopleCount));
    objectsValue_->setText(QStringLiteral("无"));
    warningReasonValue_->setText(QStringLiteral("-"));
    summaryValue_->setText(result.summary);
    modelLabel_->setText(QStringLiteral("模型  %1").arg(result.model));
    latencyValue_->setText(QStringLiteral("-- ms"));
    totalTokensValue_->setText(QStringLiteral("0"));
    inputTokensValue_->setText(QStringLiteral("0"));
    outputTokensValue_->setText(QStringLiteral("0"));
    updatedValue_->setText(QStringLiteral("--:--:--"));
    errorValue_->setText(QStringLiteral("最近错误：无"));
    setStateLabel(videoStatusLabel_, QStringLiteral("视频离线"),
                  QStringLiteral("offline"));
    setStateLabel(aiStatusLabel_, QStringLiteral("AI 离线"),
                  QStringLiteral("offline"));
    setStateLabel(warningValue_, QStringLiteral("正常"),
                  QStringLiteral("normal"));
    setStateLabel(tcpValue_, QStringLiteral("TCP 未连接"),
                  QStringLiteral("offline"));
}

void MainWindow::updateAiResult(const AiResult &result)
{
    sceneValue_->setText(result.scene);
    peopleValue_->setText(QString::number(result.peopleCount));
    objectsValue_->setText(result.objects.isEmpty()
                               ? QStringLiteral("无")
                               : result.objects.join(QStringLiteral(", ")));
    warningReasonValue_->setText(result.warning
                                     ? (result.warningReason.isEmpty()
                                            ? QStringLiteral("未说明告警原因")
                                            : result.warningReason)
                                     : QStringLiteral("-"));
    summaryValue_->setText(result.summary);
    modelLabel_->setText(QStringLiteral("模型  %1").arg(result.model));
    latencyValue_->setText(QStringLiteral("%1 ms").arg(result.latencyMs));
    totalTokensValue_->setText(QString::number(result.totalTokens));
    inputTokensValue_->setText(QString::number(result.inputTokens));
    outputTokensValue_->setText(QString::number(result.outputTokens));

    const QDateTime parsedTime = QDateTime::fromString(result.timestamp, Qt::ISODate);
    updatedValue_->setText(parsedTime.isValid()
                               ? parsedTime.toString(QStringLiteral("HH:mm:ss"))
                               : result.timestamp.right(8));
    setStateLabel(warningValue_,
                  result.warning ? QStringLiteral("告警")
                                 : QStringLiteral("正常"),
                  result.warning ? QStringLiteral("warning")
                                 : QStringLiteral("normal"));

    if (!result.success && !result.errorMessage.isEmpty())
        updateError(result.errorMessage);
    else
        errorValue_->setText(QStringLiteral("最近错误：无"));
}

void MainWindow::updateConnectionState(const QString &state)
{
    QString visualState = QStringLiteral("offline");
    QString displayState = QStringLiteral("未连接");
    if (state == QStringLiteral("Connected")) {
        visualState = QStringLiteral("online");
        displayState = QStringLiteral("已连接");
    } else if (state == QStringLiteral("Connecting")) {
        visualState = QStringLiteral("waiting");
        displayState = QStringLiteral("连接中");
    } else if (state == QStringLiteral("Error")) {
        visualState = QStringLiteral("warning");
        displayState = QStringLiteral("错误");
    }
    setStateLabel(tcpValue_, QStringLiteral("TCP %1").arg(displayState),
                  visualState);
}

void MainWindow::updateCurrentTime()
{
    clockLabel_->setText(QDateTime::currentDateTime().toString(
        QStringLiteral("MM-dd HH:mm:ss")));
}

void MainWindow::updateAiState(const QString &state)
{
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

void MainWindow::updateError(const QString &message)
{
    errorValue_->setText(QStringLiteral("最近错误：%1").arg(message.left(100)));
}

void MainWindow::updatePreviewFrame(const QImage &image, qulonglong frameId,
                                    qulonglong sourceTimeNs)
{
    videoWidget_->setFrame(image, frameId, sourceTimeNs);
}

void MainWindow::updatePreviewState(int state)
{
    QString text = QStringLiteral("视频离线");
    QString visualState = QStringLiteral("offline");
    QString detail = QStringLiteral("视频输入：离线");

    if (state == 2) {
        text = QStringLiteral("视频在线");
        visualState = QStringLiteral("online");
        detail = QStringLiteral("视频输入：共享内存正常");
    } else if (state == 1) {
        text = QStringLiteral("视频超时");
        visualState = QStringLiteral("waiting");
        detail = QStringLiteral("视频输入：超过 1 秒未更新");
    }
    setStateLabel(videoStatusLabel_, text, visualState);
    videoWaiting_->setText(detail);
    videoWidget_->setState(state);
}

void MainWindow::updatePreviewStats(const QString &line1, const QString &line2)
{
    videoInfo_->setText(line1);
    videoWaiting_->setText(line2);
}

void MainWindow::setStateLabel(QLabel *label, const QString &text,
                               const QString &state)
{
    label->setText(text);
    label->setProperty("state", QVariant(state));
    label->style()->unpolish(label);
    label->style()->polish(label);
    label->update();
}
