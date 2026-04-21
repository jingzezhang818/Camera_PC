QT += core gui widgets multimedia multimediawidgets

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

msvc {
    # Ensure source literals are decoded as UTF-8 on MSVC.
    QMAKE_CFLAGS += /utf-8
    QMAKE_CXXFLAGS += /utf-8
}

win32-g++ {
    # Keep MinGW behavior consistent with UTF-8 source files.
    QMAKE_CFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8
    QMAKE_CXXFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8
}

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    cameraprobe.cpp \
    main.cpp \
    widget.cpp

HEADERS += \
    cameraprobe.h \
    widget.h

FORMS += \
    widget.ui

INCLUDEPATH += $$PWD/driver
win32 {
LIBS += -L$$PWD/driver -lXDMA_MoreB
}

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
