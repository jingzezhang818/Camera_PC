#include "widget.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QStringList>
#include <QDebug>

#include <clocale>

namespace {

QtMessageHandler g_defaultMessageHandler = nullptr;

bool isIgnorableCameraParamWarning(const QString &msg)
{
    return msg.startsWith(QStringLiteral("Unable to query the parameter info: QCameraImageProcessingControl::"));
}

void appMessageHandler(QtMsgType type,
                       const QMessageLogContext &context,
                       const QString &msg)
{
    if (type == QtWarningMsg && isIgnorableCameraParamWarning(msg)) {
        return;
    }

    if (g_defaultMessageHandler) {
        g_defaultMessageHandler(type, context, msg);
        return;
    }

    qt_message_output(type, context, msg);
}

void setupChineseUiFont(QApplication &app)
{
    const QStringList candidates = {
        "Noto Sans CJK SC",
        "WenQuanYi Micro Hei",
        "Microsoft YaHei",
        "SimHei",
        "AR PL UKai CN",
        "Source Han Sans SC"
    };

    const QStringList installed = QFontDatabase().families();
    for (const QString &name : candidates) {
        for (const QString &family : installed) {
            if (family.compare(name, Qt::CaseInsensitive) == 0) {
                QFont font(family);
                app.setFont(font);
                qInfo().noquote() << QString("[UI] Using CJK font: %1").arg(family);
                return;
            }
        }
    }

    qWarning() << "[UI] No preferred CJK font found. Chinese text may render as boxes.";
}

} // namespace

// 程序入口：
// 1) 创建 QApplication（负责事件循环、窗口系统交互、Qt 资源管理）；
// 2) 创建主窗口 Widget；
// 3) 显示主窗口并进入事件循环。
int main(int argc, char *argv[])
{
    // 跟随系统 locale，避免非 UTF-8 运行环境导致文本行为异常。
    std::setlocale(LC_ALL, "");

    g_defaultMessageHandler = qInstallMessageHandler(appMessageHandler);

    QApplication a(argc, argv);
    setupChineseUiFont(a);

    // 主界面对象在栈上创建，应用退出时会自动析构。
    Widget w;
    w.show();

    // 启动 Qt 主事件循环。该函数返回时表示程序准备退出。
    return a.exec();
}
