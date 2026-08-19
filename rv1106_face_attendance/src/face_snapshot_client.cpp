#include "face_snapshot_client.h"

#include <QLocalSocket>

FaceSnapshotClient::FaceSnapshotClient(const QString &socketPath) : socketPath_(socketPath) {}

bool FaceSnapshotClient::capture(float x, float y, float width, float height, QImage *image, QString *error) const {
    QLocalSocket socket;
    socket.connectToServer(socketPath_);
    if (!socket.waitForConnected(800)) { *error = QStringLiteral("取图服务未连接：%1").arg(socket.errorString()); return false; }
    socket.write(QStringLiteral("capture %1 %2 %3 %4\n").arg(x, 0, 'f', 4).arg(y, 0, 'f', 4).arg(width, 0, 'f', 4).arg(height, 0, 'f', 4).toLatin1());
    socket.flush();
    if (!socket.waitForReadyRead(1000)) { *error = QStringLiteral("取图服务没有返回数据"); return false; }
    QByteArray header = socket.readLine();
    QList<QByteArray> fields = header.trimmed().split(' ');
    bool ok = false;
    if (fields.size() != 4 || fields[0] != "OK") { *error = QStringLiteral("取图失败：%1").arg(QString::fromLatin1(header.trimmed())); return false; }
    int widthPx = fields[1].toInt(&ok); if (!ok) { *error = QStringLiteral("取图协议宽度错误"); return false; }
    int heightPx = fields[2].toInt(&ok); if (!ok) { *error = QStringLiteral("取图协议高度错误"); return false; }
    qint64 bytes = fields[3].toLongLong(&ok);
    if (!ok || widthPx < 112 || heightPx < 112 || (widthPx & 1) || (heightPx & 1) || bytes != qint64(widthPx) * heightPx * 3 / 2) { *error = QStringLiteral("取图协议数据非法"); return false; }
    QByteArray nv12 = socket.readAll();
    while (nv12.size() < bytes && socket.waitForReadyRead(1000)) nv12 += socket.readAll();
    if (nv12.size() != bytes) { *error = QStringLiteral("取图数据不完整"); return false; }
    QImage yuv(reinterpret_cast<const uchar *>(nv12.constData()), widthPx, heightPx, QImage::Format_NV12);
    *image = yuv.convertToFormat(QImage::Format_RGB888).copy();
    return !image->isNull();
}
