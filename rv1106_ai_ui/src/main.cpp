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
#include "manual_recognition_client.h"
#include "result_storage_client.h"
#include "preview_shm_reader.h"
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
    QCommandLineOption previewShmOption(
        QStringList() << QStringLiteral("preview-shm"),
        QStringLiteral("POSIX shared-memory preview name"),
        QStringLiteral("name"), QStringLiteral("/ai_cam_preview"));
    QCommandLineOption previewTimeoutOption(
        QStringList() << QStringLiteral("preview-timeout-ms"),
        QStringLiteral("Preview stale timeout in milliseconds"),
        QStringLiteral("milliseconds"), QStringLiteral("1000"));
    QCommandLineOption recognizeUrlOption(
        QStringList() << QStringLiteral("recognize-url"),
        QStringLiteral("ROCK 2A manual-recognition HTTP URL"),
        QStringLiteral("url"), QStringLiteral("http://192.168.50.1:9001/recognize"));
    QCommandLineOption autoRecognitionIntervalOption(
        QStringList() << QStringLiteral("auto-recognition-interval-ms"),
        QStringLiteral("Automatic cloud-recognition interval while live"),
        QStringLiteral("milliseconds"), QStringLiteral("30000"));
    QCommandLineOption eventApiUrlOption(
        QStringList() << QStringLiteral("event-api-url"),
        QStringLiteral("ROCK 2A event HTTP base URL"),
        QStringLiteral("url"), QStringLiteral("http://192.168.50.1:9011"));
    QCommandLineOption legacyAutoRecognitionOption(
        QStringList() << QStringLiteral("enable-legacy-auto-recognition"),
        QStringLiteral("Enable the legacy fixed-interval recognition timer"));
    parser.addOption(serverIpOption);
    parser.addOption(serverPortOption);
    parser.addOption(previewShmOption);
    parser.addOption(previewTimeoutOption);
    parser.addOption(recognizeUrlOption);
    parser.addOption(autoRecognitionIntervalOption);
    parser.addOption(eventApiUrlOption);
    parser.addOption(legacyAutoRecognitionOption);
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
    bool previewTimeoutOk = false;
    const int previewTimeoutMs = parser.value(previewTimeoutOption).toInt(&previewTimeoutOk);
    if (!previewTimeoutOk || previewTimeoutMs < 100) {
        std::fprintf(stderr, "Invalid --preview-timeout-ms: %s\n",
                     parser.value(previewTimeoutOption).toLocal8Bit().constData());
        return 2;
    }
    const QString previewShm = parser.value(previewShmOption);
    if (!previewShm.startsWith(QLatin1Char('/'))) {
        std::fprintf(stderr, "--preview-shm must start with /\n");
        return 2;
    }
    const QUrl recognizeUrl(parser.value(recognizeUrlOption));
    if (!recognizeUrl.isValid() || recognizeUrl.scheme() != QStringLiteral("http") ||
        recognizeUrl.host().isEmpty() || recognizeUrl.path() != QStringLiteral("/recognize")) {
        std::fprintf(stderr, "Invalid --recognize-url: %s\n",
                     parser.value(recognizeUrlOption).toLocal8Bit().constData());
        return 2;
    }
    bool autoIntervalOk = false;
    const int autoRecognitionIntervalMs =
        parser.value(autoRecognitionIntervalOption).toInt(&autoIntervalOk);
    if (!autoIntervalOk || autoRecognitionIntervalMs < 1000) {
        std::fprintf(stderr, "Invalid --auto-recognition-interval-ms: %s\n",
                     parser.value(autoRecognitionIntervalOption).toLocal8Bit().constData());
        return 2;
    }
    const QUrl eventApiUrl(parser.value(eventApiUrlOption));
    if (!eventApiUrl.isValid() || eventApiUrl.scheme() != QStringLiteral("http") ||
        eventApiUrl.host().isEmpty()) {
        std::fprintf(stderr, "Invalid --event-api-url: %s\n",
                     parser.value(eventApiUrlOption).toLocal8Bit().constData());
        return 2;
    }

    const QSize screenSize = app.primaryScreen()->size();
    if (screenSize != QSize(720, 720)) {
        std::fprintf(stderr, "Warning: expected 720x720 screen, got %dx%d.\n",
                     screenSize.width(), screenSize.height());
    }

    ManualRecognitionClient manualRecognitionClient;
    manualRecognitionClient.setRecognizeUrl(recognizeUrl);
    ResultStorageClient storageClient;
    QUrl saveUrl(recognizeUrl);
    saveUrl.setPath(QStringLiteral("/save-result"));
    storageClient.setUrl(saveUrl);
    storageClient.setEventApiBase(eventApiUrl);
    MainWindow window(&manualRecognitionClient, &storageClient,
                      autoRecognitionIntervalMs,
                      parser.isSet(legacyAutoRecognitionOption));
    StatusController statusController;
    AiResultClient client(serverIp, static_cast<quint16>(portValue));
    PreviewShmReader previewReader(previewShm, previewTimeoutMs);

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
    QObject::connect(&previewReader, &PreviewShmReader::frameReady,
                     &window, &MainWindow::updatePreviewFrame);
    QObject::connect(&previewReader, &PreviewShmReader::stateChanged,
                     &window, &MainWindow::updatePreviewState);
    QObject::connect(&previewReader, &PreviewShmReader::statsChanged,
                     &window, &MainWindow::updatePreviewStats);
    QObject::connect(&manualRecognitionClient, &ManualRecognitionClient::requestStarted,
                     &window, &MainWindow::onRecognitionRequestStarted);
    QObject::connect(&manualRecognitionClient, &ManualRecognitionClient::requestSucceeded,
                     &window, &MainWindow::onRecognitionRequestSucceeded);
    QObject::connect(&manualRecognitionClient, &ManualRecognitionClient::requestFailed,
                     &window, &MainWindow::onRecognitionRequestFailed);
    QObject::connect(&storageClient,&ResultStorageClient::succeeded,&window,&MainWindow::onSaveSucceeded);
    QObject::connect(&storageClient,&ResultStorageClient::failed,&window,&MainWindow::onSaveFailed);
    QObject::connect(&window,&MainWindow::userExitRequested,&app,[&app](){ app.exit(42); });
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     &client, &AiResultClient::stop);
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     &previewReader, &PreviewShmReader::stop);
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     &manualRecognitionClient, &ManualRecognitionClient::abort);

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
    QTimer::singleShot(0, &previewReader, &PreviewShmReader::start);
    const int result = app.exec();

    close(signalPipe[0]);
    close(signalPipe[1]);
    return result;
}
