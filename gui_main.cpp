#include "mainwindow.h"

#include <QApplication>

// GUI 入口。
// 与命令行版（modbus_master.cpp）最大的差别：
//   GUI 主线程天然「住」在下面这个事件循环里，所以界面、串口、定时器的信号槽
//   全部自动生效，不需要手写 processEvents；而串口通信被放进了工作线程。
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    MainWindow w;
    w.show();

    return QApplication::exec();
}
