#pragma once

#include <QByteArray>

// ============================================================
// Modbus RTU 标准 CRC16（公共实现，全项目只此一份）
//
// 算法参数（Modbus 标准规定，不能自己改）：
//   初始值        0xFFFF
//   生成多项式    0xA001（即 0x8005 按位反转后的形式，配合右移使用）
//   处理顺序      每字节先与 CRC 低 8 位异或，再逐位处理 8 次
//   字节序        算出的 16 位值放进帧尾时低字节在前：[CRC低][CRC高]
//
// 用 inline 定义在头文件里：多个 .cpp 同时 include 不会造成重复定义。
// ============================================================

inline quint16 modbusCrc16(const QByteArray &data)
{
    quint16 crc = 0xFFFF;                       // 初始值
    for (char byte : data) {
        crc ^= static_cast<quint8>(byte);       // 必须转无符号，否则 0xAA 会被当成负数
        for (int i = 0; i < 8; ++i) {           // 逐位处理 8 次
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001;                  // 多项式
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;                                 // 低字节在低 8 位
}

// 给一帧追加 CRC（低字节在前），返回带 CRC 的完整帧
inline QByteArray appendCrc(const QByteArray &frameWithoutCrc)
{
    const quint16 crc = modbusCrc16(frameWithoutCrc);
    QByteArray frame = frameWithoutCrc;
    frame.append(static_cast<char>(crc & 0xFF));         // CRC 低字节
    frame.append(static_cast<char>((crc >> 8) & 0xFF));  // CRC 高字节
    return frame;
}

// 校验整帧的 CRC：帧长至少 4 字节，且末两字节与本帧前部算出的 CRC 一致
inline bool checkCrc(const QByteArray &frame)
{
    if (frame.size() < 4) {
        return false;
    }
    const QByteArray body = frame.left(frame.size() - 2);
    const quint16 crc = modbusCrc16(body);
    const quint8 lo = static_cast<quint8>(frame.at(frame.size() - 2));
    const quint8 hi = static_cast<quint8>(frame.at(frame.size() - 1));
    return (lo == (crc & 0xFF)) && (hi == ((crc >> 8) & 0xFF));
}
