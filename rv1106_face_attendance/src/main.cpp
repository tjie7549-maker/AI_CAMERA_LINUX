#include <QApplication>
#include <QFile>
#include <QUrl>
#include "attendance_window.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QFile styleFile(QStringLiteral(":/style.qss"));
    if (styleFile.open(QIODevice::ReadOnly))
        app.setStyleSheet(QString::fromUtf8(styleFile.readAll()));
    QString socket = QStringLiteral("/tmp/rv1106_face_snapshot.sock");
    QUrl uploadUrl(QStringLiteral("http://192.168.50.1:9012/v1/face/verify"));
    QString tokenFile = QStringLiteral("/userdata/ai_camera/attendance.token");
    for (int i = 1; i + 1 < argc; i += 2) {
        const QString option = QString::fromLocal8Bit(argv[i]); const QString value = QString::fromLocal8Bit(argv[i + 1]);
        if (option == "--snapshot-socket") socket = value;
        else if (option == "--upload-url") uploadUrl = QUrl(value);
        else if (option == "--token-file") tokenFile = value;
    }
    QFile file(tokenFile);
    if (!file.open(QIODevice::ReadOnly)) return 2;
    const QByteArray token = file.readAll().trimmed();
    if (token.size() < 16) return 2;
    AttendanceWindow window(socket, uploadUrl, token); window.show();
    return app.exec();
}
