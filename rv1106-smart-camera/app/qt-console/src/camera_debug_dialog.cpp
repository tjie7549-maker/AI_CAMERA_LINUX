#include "camera_debug_dialog.h"
#include "daemon_client.h"

#include <QCheckBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

namespace {
class NumericKeypadDialog : public QDialog {
public:
    NumericKeypadDialog(const QString &title, int current, int minimum, int maximum, QWidget *parent)
        : QDialog(parent), minimum_(minimum), maximum_(maximum)
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::Dialog);
        setFixedSize(480, 438);
        setObjectName(QStringLiteral("numericKeypad"));
        setStyleSheet(QStringLiteral(
            "QDialog#numericKeypad{background:#17212b;color:#edf2f5;font-family:'Droid Sans Fallback';border:2px solid #4d7f90;}"
            "QLabel#padTitle{font-size:24px;font-weight:700;} QLabel#padRange{font-size:15px;color:#9fb4c0;}"
            "QLineEdit#padValue{background:#0c141b;border:1px solid #5b8394;color:#55d4e8;font-size:32px;font-weight:700;padding:6px;text-align:right;}"
            "QPushButton{background:#263b48;border:1px solid #527080;border-radius:5px;color:#f2f7fa;font-size:25px;font-weight:700;}"
            "QPushButton:pressed{background:#1a2c36;border-color:#76d9e8;}"
            "QPushButton#padConfirm{background:#167f93;border-color:#6ce5f1;} QPushButton#padCancel{background:#4a3438;border-color:#b67579;}"));
        auto *root = new QVBoxLayout(this); root->setContentsMargins(18, 15, 18, 15); root->setSpacing(10);
        auto *caption = new QLabel(title + QStringLiteral("：请输入数值"), this); caption->setObjectName(QStringLiteral("padTitle")); root->addWidget(caption);
        auto *range = new QLabel(QStringLiteral("可设置范围：%1 ～ %2").arg(minimum).arg(maximum), this); range->setObjectName(QStringLiteral("padRange")); root->addWidget(range);
        value_ = new QLineEdit(QString::number(qBound(minimum, current, maximum)), this);
        value_->setObjectName(QStringLiteral("padValue")); value_->setReadOnly(true); value_->setAlignment(Qt::AlignRight | Qt::AlignVCenter); root->addWidget(value_);

        auto *keys = new QGridLayout; keys->setHorizontalSpacing(8); keys->setVerticalSpacing(8);
        const QStringList labels = {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"),
                                    QStringLiteral("4"), QStringLiteral("5"), QStringLiteral("6"),
                                    QStringLiteral("7"), QStringLiteral("8"), QStringLiteral("9"),
                                    QStringLiteral("清除"), QStringLiteral("0"), QStringLiteral("退格")};
        for (int i = 0; i < labels.size(); ++i) {
            auto *key = new QPushButton(labels.at(i), this); key->setFixedHeight(48);
            connect(key, &QPushButton::clicked, this, [this, key]() {
                QString text = value_->text(); const QString action = key->text();
                if (action == QStringLiteral("清除")) text.clear();
                else if (action == QStringLiteral("退格")) text.chop(1);
                else if (text.size() < 7) text += action;
                value_->setText(text);
            });
            keys->addWidget(key, i / 3, i % 3);
        }
        root->addLayout(keys);
        auto *actions = new QHBoxLayout; actions->setSpacing(10);
        auto *cancel = new QPushButton(QStringLiteral("取消"), this); cancel->setObjectName(QStringLiteral("padCancel")); cancel->setFixedHeight(50);
        auto *confirm = new QPushButton(QStringLiteral("确认修改"), this); confirm->setObjectName(QStringLiteral("padConfirm")); confirm->setFixedHeight(50);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(confirm, &QPushButton::clicked, this, [this]() {
            bool ok = false; const int parsed = value_->text().toInt(&ok);
            if (!ok || parsed < minimum_ || parsed > maximum_) { value_->setText(QStringLiteral("范围错误")); return; }
            accept();
        });
        actions->addWidget(cancel); actions->addWidget(confirm); root->addLayout(actions);
    }
    int value() const { return value_->text().toInt(); }
private:
    QLineEdit *value_;
    int minimum_, maximum_;
};
}

CameraDebugDialog::CameraDebugDialog(DaemonClient *client, QWidget *parent)
    : QDialog(parent), client_(client), overview_(new QLabel), driver_(new QLabel), error_(new QLabel),
      autoAe_(new QCheckBox(QStringLiteral("自动曝光 / 自动增益（ISP 控制）"))), events_(new QTextEdit),
      restoreDefaults_(nullptr), modeButton_(nullptr), restoreConfirm_(false), debugActive_(false)
{
    setObjectName(QStringLiteral("cameraDebugDialog"));
    setWindowFlags(Qt::FramelessWindowHint | Qt::Dialog); setFixedSize(720, 720);
    setStyleSheet(QStringLiteral(
        "QDialog#cameraDebugDialog{background:#10151b;color:#edf2f5;font-family:'Droid Sans Fallback';}"
        "QFrame#debugHeader,QFrame#debugCard{background:#1a222c;border:1px solid #344351;border-radius:6px;}"
        "QLabel#debugTitle{font-size:24px;font-weight:700;color:#f7fafc;} QLabel#debugHint{font-size:13px;color:#9fb1bd;}"
        "QLabel#debugStatus{font-size:19px;font-weight:700;color:#4bd197;} QLabel#debugValue{font-size:17px;color:#e8f0f5;} QLabel#debugError{color:#ff9b8f;font-size:14px;}"
        "QPushButton#debugBack{background:#243946;border:1px solid #5a889b;border-radius:4px;color:#e6eef2;font-size:17px;font-weight:700;padding:7px 14px;}"
        "QTabWidget::pane{border:1px solid #344351;background:#17212b;} QTabBar::tab{background:#202d38;color:#aebcc6;min-width:104px;padding:9px 5px;font-size:16px;} QTabBar::tab:selected{background:#2b596b;color:#f2f7fa;font-weight:700;}"
        "QCheckBox{font-size:18px;color:#edf2f5;padding:8px;} QCheckBox::indicator{width:26px;height:26px;border:1px solid #7d99a8;background:#0f171e;} QCheckBox::indicator:checked{background:#28a9c0;border-color:#a5f1f8;}"
        "QPushButton#controlRow{background:#14232d;border:1px solid #416b7d;border-radius:5px;color:#eef6f8;text-align:left;padding:8px 16px;font-size:18px;font-weight:700;} QPushButton#controlRow:pressed{background:#1e4350;border-color:#72d8e5;} QPushButton#controlRow:disabled{background:#172027;border-color:#303d46;color:#82909a;}"
        "QPushButton#restoreDefaults{background:#513a32;border:1px solid #d48768;border-radius:5px;color:#fff0e9;font-size:18px;font-weight:700;padding:8px;} QPushButton#restoreDefaults:pressed{background:#76483b;}"
        "QPushButton#modeButton{background:#176276;border:1px solid #71dcea;border-radius:5px;color:#effcff;font-size:18px;font-weight:700;padding:8px;} QPushButton#modeButton:pressed{background:#124959;}"
        "QTextEdit{background:#0f171e;color:#cbd8df;border:1px solid #344351;font-size:14px;}"));

    auto *root = new QVBoxLayout(this); root->setContentsMargins(14, 14, 14, 14); root->setSpacing(9);
    auto *header = new QFrame(this); header->setObjectName(QStringLiteral("debugHeader")); header->setFixedHeight(60);
    auto *headerLayout = new QHBoxLayout(header); headerLayout->setContentsMargins(15, 7, 12, 7);
    auto *titleBox = new QVBoxLayout; titleBox->setSpacing(1);
    auto *title = new QLabel(QStringLiteral("相机参数与诊断"), header); title->setObjectName(QStringLiteral("debugTitle"));
    auto *hint = new QLabel(QStringLiteral("实时读取 SC3336；所有修改经 camera-daemon 写入"), header); hint->setObjectName(QStringLiteral("debugHint"));
    titleBox->addWidget(title); titleBox->addWidget(hint); headerLayout->addLayout(titleBox, 1);
    auto *back = new QPushButton(QStringLiteral("返回"), header); back->setObjectName(QStringLiteral("debugBack")); back->setFixedSize(96, 40);
    connect(back, &QPushButton::clicked, this, [this]() { if (debugActive_) client_->exitDebug(); accept(); }); headerLayout->addWidget(back); root->addWidget(header);

    auto *tabs = new QTabWidget(this); root->addWidget(tabs, 1);
    auto *overviewPage = new QWidget(tabs); auto *overviewLayout = new QVBoxLayout(overviewPage); overviewLayout->setContentsMargins(16, 16, 16, 16); overviewLayout->setSpacing(10);
    auto *statusCard = new QFrame(overviewPage); statusCard->setObjectName(QStringLiteral("debugCard")); auto *statusLayout = new QVBoxLayout(statusCard); statusLayout->setContentsMargins(15, 12, 15, 12);
    overview_->setObjectName(QStringLiteral("debugStatus")); overview_->setWordWrap(true); statusLayout->addWidget(overview_); overviewLayout->addWidget(statusCard);
    auto *metricCard = new QFrame(overviewPage); metricCard->setObjectName(QStringLiteral("debugCard")); auto *metricLayout = new QVBoxLayout(metricCard); metricLayout->setContentsMargins(15, 12, 15, 12);
    driver_->setObjectName(QStringLiteral("debugValue")); driver_->setWordWrap(true); metricLayout->addWidget(driver_); overviewLayout->addWidget(metricCard); overviewLayout->addStretch(); tabs->addTab(overviewPage, QStringLiteral("状态"));

    auto *controlPage = new QWidget(tabs); auto *controlLayout = new QVBoxLayout(controlPage); controlLayout->setContentsMargins(14, 10, 14, 12); controlLayout->setSpacing(7);
    modeButton_ = new QPushButton(QStringLiteral("进入参数调节"), controlPage); modeButton_->setObjectName(QStringLiteral("modeButton")); modeButton_->setFixedHeight(48);
    connect(modeButton_, &QPushButton::clicked, this, [this]() { if (debugActive_) client_->exitDebug(); else client_->enterDebug(); }); controlLayout->addWidget(modeButton_);
    controlLayout->addWidget(autoAe_);
    auto *modeHint = new QLabel(QStringLiteral("视频与 AI 始终使用同一条项目采集管线；进入后可修改参数，返回预览会保留当前参数且不会重启视频。"), controlPage); modeHint->setObjectName(QStringLiteral("debugHint")); modeHint->setWordWrap(true); controlLayout->addWidget(modeHint);
    auto *scroll = new QScrollArea(controlPage); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto *controlList = new QWidget(scroll); auto *listLayout = new QVBoxLayout(controlList); listLayout->setContentsMargins(1, 1, 1, 1); listLayout->setSpacing(7);
    addControl(listLayout, QStringLiteral("曝光时间（行）"), QStringLiteral("exposure"), 1, 1624);
    addControl(listLayout, QStringLiteral("模拟增益"), QStringLiteral("analogue_gain"), 128, 99614);
    addControl(listLayout, QStringLiteral("垂直消隐 VBLANK"), QStringLiteral("vblank"), 64, 31471);
    addControl(listLayout, QStringLiteral("水平翻转（0/1）"), QStringLiteral("hflip"), 0, 1);
    addControl(listLayout, QStringLiteral("垂直翻转（0/1）"), QStringLiteral("vflip"), 0, 1);
    addControl(listLayout, QStringLiteral("测试图（0=关闭）"), QStringLiteral("test_pattern"), 0, 4);
    listLayout->addStretch(); scroll->setWidget(controlList); controlLayout->addWidget(scroll, 1);
    restoreDefaults_ = new QPushButton(QStringLiteral("恢复进入页时的自动参数"), controlPage); restoreDefaults_->setObjectName(QStringLiteral("restoreDefaults")); restoreDefaults_->setFixedHeight(48);
    connect(restoreDefaults_, &QPushButton::clicked, this, &CameraDebugDialog::restoreDefaults); controlLayout->addWidget(restoreDefaults_);
    tabs->addTab(controlPage, QStringLiteral("参数调节"));

    auto *driverPage = new QWidget(tabs); auto *driverLayout = new QVBoxLayout(driverPage); driverLayout->setContentsMargins(18, 18, 18, 18);
    auto *driverInfo = new QLabel(QStringLiteral("SC3336 驱动统计会在 debugfs 补丁刷写后显示。\n这里不伪造 I2C 失败数或启动耗时。"), driverPage); driverInfo->setObjectName(QStringLiteral("debugValue")); driverInfo->setWordWrap(true); driverLayout->addWidget(driverInfo); driverLayout->addStretch(); tabs->addTab(driverPage, QStringLiteral("驱动"));
    auto *eventsPage = new QWidget(tabs); auto *eventsLayout = new QVBoxLayout(eventsPage); eventsLayout->setContentsMargins(13, 13, 13, 13); events_->setReadOnly(true); events_->setPlaceholderText(QStringLiteral("等待 daemon 状态和事件…")); eventsLayout->addWidget(events_); tabs->addTab(eventsPage, QStringLiteral("日志"));
    error_->setObjectName(QStringLiteral("debugError")); error_->setWordWrap(true); error_->setMinimumHeight(24); root->addWidget(error_);

    connect(autoAe_, &QCheckBox::toggled, client_, &DaemonClient::setAutoAe);
    connect(client_, &DaemonClient::statusReceived, this, &CameraDebugDialog::updateStatus);
    connect(client_, &DaemonClient::requestFailed, this, &CameraDebugDialog::showDaemonError);
    client_->requestStatus();
}

void CameraDebugDialog::addControl(QVBoxLayout *layout, const QString &title, const QString &id, int minimum, int maximum)
{
    auto *row = new QPushButton(title + QStringLiteral("\n读取中…"), this); row->setObjectName(QStringLiteral("controlRow")); row->setFixedHeight(58); row->setFocusPolicy(Qt::NoFocus);
    connect(row, &QPushButton::clicked, this, [this, id]() { editControl(id); });
    layout->addWidget(row); controlButtons_.insert(id, row); controlTitles_.insert(id, title); controlRanges_.insert(id, qMakePair(minimum, maximum));
}

void CameraDebugDialog::editControl(const QString &id)
{
    if (autoAe_->isChecked() && (id == QStringLiteral("exposure") || id == QStringLiteral("analogue_gain"))) {
        showDaemonError(QStringLiteral("请先关闭“自动曝光 / 自动增益”，再手动修改该参数。")); return;
    }
    QPushButton *button = controlButtons_.value(id); const QPair<int, int> range = controlRanges_.value(id);
    bool ok = false; const int current = button->property("controlValue").toInt(&ok);
    NumericKeypadDialog editor(controlTitles_.value(id), ok ? current : range.first, range.first, range.second, this);
    if (editor.exec() == QDialog::Accepted) client_->setControl(id, editor.value());
}

void CameraDebugDialog::restoreDefaults()
{
    if (!restoreConfirm_) {
        restoreConfirm_ = true; restoreDefaults_->setText(QStringLiteral("再次点按确认：恢复自动快照"));
        QTimer::singleShot(3000, this, [this]() { if (restoreConfirm_) { restoreConfirm_ = false; restoreDefaults_->setText(QStringLiteral("恢复进入页时的自动参数")); } });
        return;
    }
    restoreConfirm_ = false; restoreDefaults_->setText(QStringLiteral("恢复中…")); restoreDefaults_->setEnabled(false); client_->restoreDefaults();
}

void CameraDebugDialog::updateStatus(const QString &json)
{
    QJsonParseError error; const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) { showDaemonError(QStringLiteral("daemon 返回格式无效")); return; }
    const QJsonObject o = doc.object(); const bool ae = o.value(QStringLiteral("auto_ae")).toBool(true);
    debugActive_ = o.value(QStringLiteral("mode")).toString() == QStringLiteral("DEBUG");
    autoAe_->blockSignals(true); autoAe_->setChecked(ae); autoAe_->blockSignals(false);
    autoAe_->setEnabled(debugActive_);
    modeButton_->setText(debugActive_ ? QStringLiteral("返回预览（保持当前参数）") : QStringLiteral("进入参数调节"));
    const QJsonObject values = o.value(QStringLiteral("controls")).toObject();
    for (auto it = controlButtons_.constBegin(); it != controlButtons_.constEnd(); ++it) {
        const int value = values.value(it.key()).toInt(-1); const bool locked = !debugActive_ || (ae && (it.key() == QStringLiteral("exposure") || it.key() == QStringLiteral("analogue_gain")));
        it.value()->setEnabled(!locked); it.value()->setProperty("controlValue", value);
        it.value()->setText(controlTitles_.value(it.key()) + QStringLiteral("\n当前值：%1%2").arg(value < 0 ? QStringLiteral("读取失败") : QString::number(value)).arg(debugActive_ ? QStringLiteral("    点按输入") : QStringLiteral("    展示模式只读")));
    }
    restoreDefaults_->setEnabled(debugActive_); if (!restoreConfirm_) restoreDefaults_->setText(QStringLiteral("恢复进入页时的自动参数"));
    const int pipelinePid = o.value(QStringLiteral("pipeline_pid")).toInt(-1); const QString policy = o.value(QStringLiteral("state")).toString(QStringLiteral("未知"));
    if (pipelinePid > 0) {
        overview_->setText(debugActive_ ? QStringLiteral("● 参数调节模式：项目管线独占 SC3336") : QStringLiteral("● 展示模式：项目本地预览与 AI 正在供流"));
        driver_->setText(QStringLiteral("预览由主界面实时显示\n曝光与增益：%1\n%2 PID：%3    异常计数：%4")
                         .arg(ae ? QStringLiteral("自动 ISP 控制") : QStringLiteral("手动控制"))
                         .arg(debugActive_ ? QStringLiteral("参数调节") : QStringLiteral("本地预览"))
                         .arg(pipelinePid).arg(o.value(QStringLiteral("failures")).toInt()));
        error_->clear();
    } else {
        overview_->setText(QStringLiteral("● 媒体链路未运行    策略：%1").arg(policy));
        driver_->setText(QStringLiteral("请先启动摄像头管线。当前值为驱动回读，不会伪造控制结果。"));
        error_->setText(QStringLiteral("未检测到 camera pipeline；请确认预览已启动。"));
    }
    events_->append(QStringLiteral("状态更新：%1").arg(json));
}

void CameraDebugDialog::showDaemonError(const QString &message)
{
    restoreDefaults_->setEnabled(debugActive_); if (!restoreConfirm_) restoreDefaults_->setText(QStringLiteral("恢复进入页时的自动参数"));
    error_->setText(message); events_->append(QStringLiteral("错误：%1").arg(message));
}
