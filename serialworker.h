#ifndef SERIALWORKER_H
#define SERIALWORKER_H

// ============================================================
// SerialWorker —— 串口通信工作对象（要被 moveToThread 到工作线程）
//
// 为什么要有它：
//   界面线程必须保持响应。若把串口收发放在界面线程里同步等待，
//   每次请求最长会阻塞 timeout × (1 + 重试次数)，界面就会「冻住」。
//   所以通信全部搬到这里，由工作线程自己的事件循环负责 readyRead / QTimer。
//
// 本类只做通信，不碰任何界面控件（跨线程直接操作控件是未定义行为）。
// 与界面之间只通过信号槽通信；跨线程信号默认是队列连接，Qt 自动排队，无需加锁。
//
// 请求—响应采用「同步等待」写法（waitForReadyRead + 按长度判帧）：
//   在工作线程里阻塞是安全的，它阻塞不了界面。
// ============================================================

#include <QByteArray>
#include <QObject>
#include <QSerialPort>
#include <QTimer>
#include <QVector>

#include "devicespec.h"

class SerialWorker : public QObject
{
    Q_OBJECT

public:
    explicit SerialWorker(QObject *parent = nullptr);

public slots:
    void init();     // 线程启动后调用一次：创建定时器与串口对象
    void openPort(const QString &portName, int baud);
    void closePort();
    // devices   = 要轮询的设备列表（按顺序依次访问）
    // intervalMs = 两次请求之间的间隔
    // maxPoll   = 轮询多少「轮」后自动停止（一轮 = 把所有设备各访问一次）；0 表示不限
    void startPoll(const QVector<DeviceSpec> &devices,
                   int timeoutMs, int intervalMs, int maxPoll);
    void stopPoll();

    // 写单个保持寄存器（0x06）：例如给设备下发温度上限阈值。
    // 在工作线程里同步发送并等回显（与 doOneRequest 同一套判帧思路）。
    void writeRegister(int slave, int regAddr, int value);

signals:
    void writeFinished(bool ok, int slave, int regAddr, int value, const QString &message);
    void logMessage(const QString &text);                 // 日志（界面 append 到日志区）
    void dataReady(int slave, int func, int addr, const QVector<quint16> &regs);  // 一次成功读取
    void statusChanged(const QString &text);              // 状态栏文字
    void portStateChanged(bool open);                     // 串口开关状态
    void pollStats(int success, int fail, int retry);     // 累计统计
    void pollingStateChanged(bool polling);               // 轮询开/停（界面据此更新按钮）
    void roundFinished(int round, int success, int fail); // 走完一圈（所有设备各访问一次）

private slots:
    void onPollTimeout();       // 轮询定时器到点 → 给「当前这台设备」发一次请求
    void onSerialError(QSerialPort::SerialPortError e);

private:
    bool doOneRequest(const DeviceSpec &dev);   // 发一次请求并等响应；成功返回 true
    QByteArray buildRequest(const DeviceSpec &dev) const;

    QSerialPort *m_port = nullptr;
    QTimer      *m_pollTimer = nullptr;
    bool         m_connected = false;
    bool         m_polling = false;

    // 轮询参数
    QVector<DeviceSpec> m_devices;   // 设备列表（只留启用的）
    int m_deviceIndex = 0;           // 本轮轮到第几台
    int m_round = 0;                 // 已完成的轮数
    int m_timeoutMs = 500;
    int m_intervalMs = 1000;
    int m_maxPoll = 0;               // 轮数上限；0 = 不限
    int m_baud = 115200;
    QString m_portName;

    // 累计统计
    int m_successCount = 0;
    int m_failCount = 0;
    int m_retryCount = 0;
};

#endif // SERIALWORKER_H
