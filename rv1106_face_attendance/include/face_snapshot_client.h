#ifndef FACE_SNAPSHOT_CLIENT_H
#define FACE_SNAPSHOT_CLIENT_H

#include <QImage>
#include <QString>

class FaceSnapshotClient {
public:
    explicit FaceSnapshotClient(const QString &socketPath);
    bool capture(float x, float y, float width, float height, QImage *image, QString *error) const;
private:
    QString socketPath_;
};

#endif
