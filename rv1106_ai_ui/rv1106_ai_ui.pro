QT += core gui widgets network

CONFIG += c++11 release
CONFIG -= app_bundle

TEMPLATE = app
TARGET = rv1106_ai_ui

SOURCES += \
    src/main.cpp \
    src/main_window.cpp \
    src/ai_result_client.cpp \
    src/status_controller.cpp

HEADERS += \
    include/main_window.h \
    include/ai_result.h \
    include/ai_result_client.h \
    include/status_controller.h

RESOURCES += \
    resources/resources.qrc

INCLUDEPATH += \
    include
