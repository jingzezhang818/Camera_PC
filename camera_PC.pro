QT += core gui widgets multimedia multimediawidgets

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

msvc {
    # MSVC 下统一按 UTF-8 解释源码中的字符串字面量，
    # 避免中文日志/界面文本在不同机器上出现乱码。
    QMAKE_CFLAGS += /utf-8
    QMAKE_CXXFLAGS += /utf-8
}

win32-g++ {
    # MinGW 下也显式指定输入与执行字符集，
    # 保持与 MSVC 的 UTF-8 行为一致。
    QMAKE_CFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8
    QMAKE_CXXFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8
}

# 如需强制禁止使用旧 API，可开启如下宏（当前未启用）：
# DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000

SOURCES += \
    cameraprobe.cpp \
    main.cpp \
    widget.cpp

HEADERS += \
    cameraprobe.h \
    widget.h

FORMS += \
    widget.ui

# XDMA 封装库头文件与库文件路径。
INCLUDEPATH += $$PWD/driver
win32 {
LIBS += -L$$PWD/driver -lXDMA_MoreB
}

# 目标部署目录规则（保留 qmake 默认模板结构）。
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
