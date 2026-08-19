#ifndef ATTENDANCE_WINDOW_H
#define ATTENDANCE_WINDOW_H

#include <QMainWindow>
#include <QRectF>
#include "attendance_http_client.h"
#include "face_snapshot_client.h"

class QLabel;
class QPushButton;

class AttendanceWindow : public QMainWindow {
    Q_OBJECT
public:
    AttendanceWindow(const QString &snapshotSocket, const QUrl &uploadUrl, const QByteArray &token, QWidget *parent = nullptr);
    // The caller must provide a verified face detector result in normalized coordinates.
    void setFaceBoundingBox(const QRectF &box);
private slots:
    void captureAndUpload(const QString &type);
    void uploadFinished(bool success, const QString &message);
private:
    FaceSnapshotClient snapshot_;
    AttendanceHttpClient uploader_;
    QLabel *preview_;
    QLabel *status_;
    QPushButton *checkIn_;
    QPushButton *checkOut_;
    QRectF faceBox_;
};

#endif
