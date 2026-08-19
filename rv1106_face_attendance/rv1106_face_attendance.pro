QT += core gui widgets network
CONFIG += c++11 release
CONFIG -= app_bundle
TEMPLATE = app
TARGET = rv1106_face_attendance

SOURCES += src/main.cpp src/attendance_window.cpp src/face_snapshot_client.cpp src/attendance_http_client.cpp
HEADERS += include/attendance_window.h include/face_snapshot_client.h include/attendance_http_client.h
RESOURCES += resources/resources.qrc
INCLUDEPATH += include
