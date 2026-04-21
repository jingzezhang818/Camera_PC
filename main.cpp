#include "widget.h"

#include <QApplication>

// 程序入口：
// 1) 创建 QApplication（负责事件循环、窗口系统交互、Qt 资源管理）；
// 2) 创建主窗口 Widget；
// 3) 显示主窗口并进入事件循环。
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // 主界面对象在栈上创建，应用退出时会自动析构。
    Widget w;
    w.show();

    // 启动 Qt 主事件循环。该函数返回时表示程序准备退出。
    return a.exec();
}
