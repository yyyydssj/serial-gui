#ifndef MODBUS_FRAME_H
#define MODBUS_FRAME_H

#endif // MODBUS_FRAME_H
#pragma once

// ============================================================
// Modbus RTU 帧构造与解析（公共实现，全项目只此一份）
//
// 原定义在 modbus_frame_test.cpp 内，抽出为头文件，
// 让自测程序与主站/从站程序共用同一份实现，避免两处不一致。
//
// 帧格式：
//   请求： 从站地址(1) 功能码(1) 数据区(N) CRC低(1) CRC高(1)
//   响应： 从站地址(1) 功能码(1) 数据区(N) CRC低(1) CRC高(1)
//   异常： 从站地址(1) 功能码|0x80(1) 异常码(1) CRC低(1) CRC高(1)
//
// 功能码：
//   0x01 读线圈        请求数据区 = 起始地址(2) 数量(2)
//   0x03 读保持寄存器  请求数据区 = 起始地址(2) 数量(2)
//                      响应数据区 = 字节数(1) 寄存器数据(2*N)
//   0x04 读输入寄存器  格式同 0x03
//   0x06 写单个寄存器  请求数据区 = 寄存器地址(2) 写入值(2)
//                      响应 = 原样回显请求
//
// 字节序：寄存器地址、数量、寄存器值都是高位在前（大端）
//         CRC 是低字节在前（小端）
// ============================================================

#include <QByteArray>
#include <QString>
#include <QVector>

#include "modbus_crc.h"   // CRC16、appendCrc、checkCrc

// ---------------- 请求帧构造 ----------------
// 01/02 读线圈、03 读保持寄存器、04 读输入寄存器：地址 + 数量
inline QByteArray buildReadRequest(quint8 slave, quint8 func, quint16 startAddr, quint16 count)
{
    QByteArray f;
    f.append(static_cast<char>(slave));
    f.append(static_cast<char>(func));
    f.append(static_cast<char>((startAddr >> 8) & 0xFF));   // 地址高字节
    f.append(static_cast<char>(startAddr & 0xFF));          // 地址低字节
    f.append(static_cast<char>((count >> 8) & 0xFF));       // 数量高字节
    f.append(static_cast<char>(count & 0xFF));              // 数量低字节
    return appendCrc(f);
}

// 06 写单个保持寄存器：地址 + 写入值
inline QByteArray buildWriteSingleRequest(quint8 slave, quint16 regAddr, quint16 value)
{
    QByteArray f;
    f.append(static_cast<char>(slave));
    f.append(static_cast<char>(0x06));
    f.append(static_cast<char>((regAddr >> 8) & 0xFF));
    f.append(static_cast<char>(regAddr & 0xFF));
    f.append(static_cast<char>((value >> 8) & 0xFF));
    f.append(static_cast<char>(value & 0xFF));
    return appendCrc(f);
}

// ---------------- 响应帧构造（从站侧用，也用于造测试数据）----------------
inline QByteArray buildReadResponse(quint8 slave, quint8 func, const QVector<quint16> &regs)
{
    QByteArray f;
    f.append(static_cast<char>(slave));
    f.append(static_cast<char>(func));
    f.append(static_cast<char>(regs.size() * 2));           // 字节数
    for (quint16 v : regs) {
        f.append(static_cast<char>((v >> 8) & 0xFF));       // 寄存器值高位在前
        f.append(static_cast<char>(v & 0xFF));
    }
    return appendCrc(f);
}

// ---------------- 响应帧解析 ----------------
struct ParsedResponse {
    bool ok = false;              // 帧是否可解析（校验通过 + 格式合法）
    bool isException = false;     // 是否为异常响应
    quint8 slave = 0;             // ← 从站报文里"自称"的地址
    quint8 func = 0;              // ← 从站报文里"自称"的功能码
    quint8 exceptionCode = 0;     // isException 时有效
    QVector<quint16> regs;        // 读响应解析出的寄存器值
    QString error;                // ok == false 时的原因
};

inline ParsedResponse parseResponse(const QByteArray &frame, quint8 expectSlave, quint8 expectFunc)
{
    ParsedResponse r;

    if (frame.size() < 4) {
        r.error = "帧太短（<4 字节）";
        return r;
    }
    if (!checkCrc(frame)) {
        r.error = "CRC 校验失败";
        return r;
    }

    r.slave = static_cast<quint8>(frame.at(0));
    r.func  = static_cast<quint8>(frame.at(1));

    if (r.slave != expectSlave) {
        r.error = QString("从站地址不匹配：收到 %1，期望 %2")
                      .arg(r.slave).arg(expectSlave);
        return r;
    }

    // 异常响应：功能码最高位置 1
    if (r.func == (expectFunc | 0x80)) {
        if (frame.size() < 5) {
            r.error = "异常响应帧长度不足";
            return r;
        }
        r.isException = true;
        r.exceptionCode = static_cast<quint8>(frame.at(2));
        r.ok = true;
        return r;
    }

    if (r.func != expectFunc) {
        r.error = QString("功能码不匹配：收到 %1，期望 %2")
                      .arg(r.func).arg(expectFunc);
        return r;
    }

    if (expectFunc == 0x03 || expectFunc == 0x04) {
        const int byteCount = static_cast<quint8>(frame.at(2));
        // 帧长 = 地址1 + 功能码1 + 字节数1 + 数据N + CRC2
        if (frame.size() != 3 + byteCount + 2) {
            r.error = QString("字节数与帧长不符：字节数=%1，帧长=%2")
                          .arg(byteCount).arg(frame.size());
            return r;
        }
        if (byteCount % 2 != 0) {
            r.error = "字节数为奇数，寄存器数量不合法";
            return r;
        }
        r.regs.reserve(byteCount / 2);
        for (int i = 0; i < byteCount; i += 2) {
            const quint8 hi = static_cast<quint8>(frame.at(3 + i));
            const quint8 lo = static_cast<quint8>(frame.at(3 + i + 1));
            r.regs.append(static_cast<quint16>((hi << 8) | lo));
        }
        r.ok = true;
        return r;
    }

    if (expectFunc == 0x06) {
        if (frame.size() != 8) {
            r.error = QString("0x06 响应帧长应为 8，实际 %1").arg(frame.size());
            return r;
        }
        const quint8 hi = static_cast<quint8>(frame.at(4));
        const quint8 lo = static_cast<quint8>(frame.at(5));
        r.regs.append(static_cast<quint16>((hi << 8) | lo));
        r.ok = true;
        return r;
    }

    r.error = QString("本步骤未支持的功能码解析：%1").arg(expectFunc);
    return r;
}