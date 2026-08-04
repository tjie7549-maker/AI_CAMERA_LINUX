#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFontDatabase>
#include <QHostAddress>
#include <QScreen>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

#include "ai_result.h"
#include "ai_result_client.h"
#include "main_window.h"
#include "status_controller.h"

namespace {
int signalPipe[2] = {-1, -1};

void handleSignal(int signalNumber)
{
    const unsigned char value = static_cast<unsigned char>(signalNumber);
    const int savedErrno = errno;
    if (signalPipe[1] >= 0)
        (void)write(signalPipe[1], &value, sizeof(value));
    errno = savedErrno;
}

bool installSignalHandlers()
{
    sigset_t signalSet;
    sigemptyset(&signalSet);
    sigaddset(&signalSet, SIGINT);
    sigaddset(&signalSet, SIGTERM);
    sigprocmask(SIG_UNBLOCK, &signalSet, nullptr);

    if (pipe(signalPipe) != 0)
        return false;
    for (int fd : signalPipe) {
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    struct sigaction action = {};
    action.sa_handler = handleSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    return sigaction(SIGINT, &action, nullptr) == 0 &&
           sigaction(SIGTERM, &action, nullptr) == 0;
}
} // namespace

int main(int argc, char *argv[])
{
    if (!installSignalHandlers()) {
        std::fprintf(stderr, "Failed to install signal handlers.\n");
        return 1;
    }

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("rv1106_ai_ui"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));
    qRegisterMetaType<AiResult>("AiResult");

    const QString cjkFontPath =
        QCoreApplication::applicationDirPath() +
        QStringLiteral("/fonts/DroidSansFallbackFull.ttf");
    const int cjkFontId = QFontDatabase::addApplicationFont(cjkFontPath);
    if (cjkFontId < 0) {
        qWarning("Failed to load CJK font: %s",
                 cjkFontPath.toLocal8Bit().constData());
    } else {
        qInfo("#Font loaded: %s",
              QFontDatabase::applicationFontFamilies(cjkFontId)
                  .join(QStringLiteral(", "))
                  .toLocal8Bit()
                  .constData());
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("RV1106 720x720 AI vision terminal"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption serverIpOption(
        QStringList() << QStringLiteral("server-ip"),
        QStringLiteral("ROCK 2A TCP server IPv4 address"),
        QStringLiteral("address"), QStringLiteral("192.168.50.1"));
    QCommandLineOption serverPortOption(
        QStringList() << QStringLiteral("server-port"),
        QStringLiteral("ROCK 2A TCP server port"),
        QStringLiteral("port"), QStringLiteral("9000"));
    parser.addOption(serverIpOption);
    parser.addOption(serverPortOption);
    parser.process(app);

    const QString serverIp = parser.value(serverIpOption);
    QHostAddress serverAddress;
    if (!serverAddress.setAddress(serverIp) ||
        serverAddress.protocol() != QAbstractSocket::IPv4Protocol) {
        std::fprintf(stderr, "Invalid --server-ip: %s\n",
                     serverIp.toLocal8Bit().constData());
        return 2;
    }

    bool portOk = false;
    const uint portValue = parser.value(serverPortOption).toUInt(&portOk);
    if (!portOk || portValue == 0 || portValue > 65535) {
        std::fprintf(stderr, "Invalid --server-port: %s\n",
                     parser.value(serverPortOption).toLocal8Bit().constData());
        return 2;
    }

    const QSize screenSize = app.primaryScreen()->size();
    if (screenSize != QSize(720, 720)) {
        std::fprintf(stderr, "Warning: expected 720x720 screen, got %dx%d.\n",
                     screenSize.width(), screenSize.height());
    }

    MainWindow window;
    StatusController statusController;
    AiResultClient client(serverIp, static_cast<quint16>(portValue));

    QObject::connect(&client, &AiResultClient::resultReceived,
                     &window, &MainWindow::updateAiResult);
    QObject::connect(&client, &AiResultClient::resultReceived,
                     &statusController, [&statusController](const AiResult &) {
                         statusController.markAiResultReceived();
                     });
    QObject::connect(&client, &AiResultClient::connectionStateChanged,
                     &window, &MainWindow::updateConnectionState);
    QObject::connect(&client, &AiResultClient::errorOccurred,
                     &window, &MainWindow::updateError);
    QObject::connect(&statusController, &StatusController::aiStateChanged,
                     &window, &MainWindow::updateAiState);
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     &client, &AiResultClient::stop);

    QSocketNotifier signalNotifier(signalPipe[0], QSocketNotifier::Read, &app);
    QObject::connect(&signalNotifier, &QSocketNotifier::activated,
                     [&app](int fd) {
                         unsigned char values[16];
                         while (read(fd, values, sizeof(values)) > 0) {
                         }
                         app.quit();
                     });

    QApplication::setOverrideCursor(Qt::BlankCursor);
    window.showFullScreen();
    QTimer::singleShot(0, &client, &AiResultClient::start);
    const int result = app.exec();

    close(signalPipe[0]);
    close(signalPipe[1]);
    return result;
}
