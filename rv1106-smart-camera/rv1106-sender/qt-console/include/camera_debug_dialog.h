#ifndef CAMERA_DEBUG_DIALOG_H
#define CAMERA_DEBUG_DIALOG_H

#include <QDialog>
#include <QMap>
#include <QPair>

class DaemonClient;
class QLabel;
class QCheckBox;
class QTextEdit;
class QPushButton;
class QVBoxLayout;

class CameraDebugDialog : public QDialog {
    Q_OBJECT
   public:
    explicit CameraDebugDialog(DaemonClient *client, QWidget *parent = nullptr);
   public slots:
    void updateStatus(const QString &json);
    void showDaemonError(const QString &message);

   private:
    void addControl(QVBoxLayout *layout, const QString &title, const QString &id, int minimum,
                    int maximum);
    void editControl(const QString &id);
    void restoreDefaults();
    DaemonClient *client_;
    QLabel *overview_, *driver_, *error_;
    QCheckBox *autoAe_;
    QTextEdit *events_;
    QPushButton *restoreDefaults_, *modeButton_;
    bool restoreConfirm_, debugActive_;
    QMap<QString, QPushButton *> controlButtons_;
    QMap<QString, QString> controlTitles_;
    QMap<QString, QPair<int, int> > controlRanges_;
};

#endif
