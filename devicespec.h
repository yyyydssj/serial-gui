#ifndef DEVICESPEC_H
#define DEVICESPEC_H

// ============================================================
// DeviceSpec —— 一台被轮询的从站设备（界面与工作线程共用）
//
// 为什么单独放一个头文件：
//   界面（主线程）负责收集/编辑设备列表，工作线程负责按列表轮询。
//   两边都要用同一个结构，所以抽出来——否则会各写一份、字段对不上。
// ============================================================

#include <QMetaType>
#include <QString>
#include <QVector>

struct DeviceSpec {
    bool enabled = true;     // 是否参与轮询
    int  slave   = 1;        // 从站地址 1~247
    int  func    = 3;        // 功能码：3 = 读保持寄存器，4 = 读输入寄存器
    int  addr    = 0;        // 起始寄存器地址 0~65535
    int  count   = 2;        // 读几个寄存器 1~125

    bool isValid() const
    {
        return slave >= 1 && slave <= 247
               && (func == 3 || func == 4)
               && addr >= 0 && addr <= 65535
               && count >= 1 && count <= 125;
    }

    // 日志/提示用的简短描述
    QString describe() const
    {
        return QString("从站 %1：功能码 0x%2，起始 %3，寄存器数 %4")
            .arg(slave)
            .arg(func, 2, 16, QChar('0'))
            .arg(addr)
            .arg(count);
    }
};

// 跨线程信号槽传自定义类型，必须声明为元类型（否则队列连接会拒绝投递）
Q_DECLARE_METATYPE(DeviceSpec)

#endif // DEVICESPEC_H
