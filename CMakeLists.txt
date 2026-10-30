cmake_minimum_required(VERSION 3.21)
project(serial_gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt 的信号槽需要 moc 生成元对象代码；MainWindow / SerialWorker / HistoryDialog 都带 Q_OBJECT
set(CMAKE_AUTOMOC ON)
# 界面用 Qt Designer 的 .ui 文件描述，需要 uic 生成 ui_mainwindow.h / ui_historydialog.h
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core SerialPort Widgets Sql)

# ---------------- 串口 Modbus RTU 上位机（图形界面版）----------------
# 架构：MainWindow（主线程，只管界面）+ SerialWorker（工作线程，只管串口通信）
#       HistoryDb（主线程，SQLite 数据落地）
# 入口是 gui_main.cpp
add_executable(serial_gui
    gui_main.cpp
    mainwindow.cpp
    mainwindow.ui          # 界面描述：由 uic 生成 ui_mainwindow.h（AUTOUIC）
    historydialog.cpp
    historydialog.h        # 带 Q_OBJECT，需要 AUTOMOC 处理
    historydialog.ui       # 历史数据对话框的界面描述
    serialworker.cpp
    serialworker.h         # 带 Q_OBJECT，需要 AUTOMOC 处理
    historydb.cpp
    alarmengine.cpp
    alarmengine.h          # 报警判定引擎（纯逻辑）
    alarmrule.h
    devicespec.h           # 界面与工作线程共用的设备描述结构
    modbus_frame.h
)
target_link_libraries(serial_gui PRIVATE Qt6::Core Qt6::SerialPort Qt6::Widgets Qt6::Sql)
