#include "attendance_window.h"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>

AttendanceWindow::AttendanceWindow(const QString &snapshotSocket, const QUrl &uploadUrl, const QByteArray &token, QWidget *parent)
    : QMainWindow(parent), snapshot_(snapshotSocket), uploader_(uploadUrl, token, this) {
    setWindowTitle(QStringLiteral("人脸考勤"));
    setFixedSize(720, 720);
    QWidget *page = new QWidget(this); page->setObjectName("page"); setCentralWidget(page);
    QVBoxLayout *layout = new QVBoxLayout(page); layout->setContentsMargins(28, 26, 28, 28); layout->setSpacing(16);
    QLabel *title = new QLabel(QStringLiteral("人脸考勤")); title->setObjectName("title"); layout->addWidget(title);
    QLabel *hint = new QLabel(QStringLiteral("请将人脸置于取景框中央，再选择签到或签退")); hint->setObjectName("hint"); layout->addWidget(hint);
    preview_ = new QLabel(QStringLiteral("等待抓拍")); preview_->setObjectName("preview"); preview_->setAlignment(Qt::AlignCenter); preview_->setMinimumHeight(410); layout->addWidget(preview_, 1);
    status_ = new QLabel(QStringLiteral("状态：就绪")); status_->setObjectName("status"); status_->setWordWrap(true); layout->addWidget(status_);
    QHBoxLayout *actions = new QHBoxLayout; actions->setSpacing(14);
    checkIn_ = new QPushButton(QStringLiteral("签到")); checkIn_->setObjectName("primary");
    checkOut_ = new QPushButton(QStringLiteral("签退")); checkOut_->setObjectName("secondary");
    actions->addWidget(checkIn_); actions->addWidget(checkOut_); layout->addLayout(actions);
    connect(checkIn_, &QPushButton::clicked, this, [this] { captureAndUpload(QStringLiteral("check_in")); });
    connect(checkOut_, &QPushButton::clicked, this, [this] { captureAndUpload(QStringLiteral("check_out")); });
    connect(&uploader_, &AttendanceHttpClient::finished, this, &AttendanceWindow::uploadFinished);
}

void AttendanceWindow::setFaceBoundingBox(const QRectF &box) {
    const bool valid = box.x() >= 0.0 && box.y() >= 0.0 && box.width() > 0.0 &&
                       box.height() > 0.0 && box.right() <= 1.0 && box.bottom() <= 1.0;
    faceBox_ = valid ? box : QRectF();
}

void AttendanceWindow::captureAndUpload(const QString &type) {
    QImage image; QString error;
    checkIn_->setEnabled(false); checkOut_->setEnabled(false);
    if (faceBox_.isEmpty()) {
        uploadFinished(false, QStringLiteral("未检测到人脸，无法抓拍"));
        return;
    }
    status_->setText(QStringLiteral("状态：正在抓取高分辨率人脸区域…"));
    if (!snapshot_.capture(float(faceBox_.x()), float(faceBox_.y()),
                           float(faceBox_.width()), float(faceBox_.height()),
                           &image, &error)) { uploadFinished(false, error); return; }
    preview_->setPixmap(QPixmap::fromImage(image).scaled(preview_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    status_->setText(QStringLiteral("状态：正在进行身份核验…"));
    uploader_.verify(image, type);
}

void AttendanceWindow::uploadFinished(bool success, const QString &message) {
    status_->setText(success ? QStringLiteral("状态：考勤完成\n%1").arg(message) : QStringLiteral("状态：%1").arg(message));
    checkIn_->setEnabled(true); checkOut_->setEnabled(true);
}
