#include "serialworker.h"

#include <QElapsedTimer>
#include <QThread>

#include "modbus_frame.h"   // 复用已验证的帧构造 / 解析 / CRC

// 单次请求最多尝试几次（1 次首发 + 重试）
static const int kMaxAttempts = 3;

SerialWorker::SerialWorker(QObject *parent) : QObject(parent) {}

void SerialWorker::init()
{
    m_port = new QSerialPort(this);
    QObject::connect(m_port, &QSerialPort::errorOccurred, this, &SerialWorker::onSerialError);

    m_pollTimer = new QTimer(this);
    QObject::connect(m_pollTimer, &QTimer::timeout, this, &SerialWorker::onPollTimeout);

    emit logMessage("通信线程已启动");
}

void SerialWorker::openPort(const QString &portName, int baud)
{
    if (m_port->isOpen()) {
        emit logMessage("串口已经打开，先关闭再重开");
        closePort();
    }

    m_portName = portName;
    m_baud = baud;

    // ★ 串口参数必须在 open() 之前设置（老坑：曾漏调用，实际跑成系统默认 9600）
    m_port->setPortName(portName);
    m_port->setBaudRate(baud);
    m_port->setDataBits(QSerialPort::Data8);
    m_port->setParity(QSerialPort::NoParity);
    m_port->setStopBits(QSerialPort::OneStop);
    m_port->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_port->open(QIODevice::ReadWrite)) {
        emit logMessage(QString("打开 %1 失败：%2").arg(portName, m_port->errorString()));
        emit statusChanged(QString("打开失败：%1").arg(m_port->errorString()));
        emit portStateChanged(false);
        return;
    }

    m_port->clear();
    m_connected = true;      // ★ 必须置位，否则 startPoll() 会拒绝启动
    emit logMessage(QString("已打开 %1：%2 8N1").arg(portName).arg(baud));
    emit statusChanged(QString("已连接 %1 @ %2").arg(portName).arg(baud));
    emit portStateChanged(true);
}

void SerialWorker::closePort()
{
    stopPoll();
    if (m_connected) {
        m_port->close();
        m_connected = false;
        emit logMessage(QString("已关闭 %1").arg(m_portName));
        emit statusChanged("未连接");
        emit portStateChanged(false);
    }
}

void SerialWorker::onSerialError(QSerialPort::SerialPortError e)
{
    if (e == QSerialPort::NoError) return;   // 打开成功时也会发一次 NoError，不是错误
    emit logMessage(QString("串口错误：[%1] %2").arg(e).arg(m_port->errorString()));
    emit statusChanged(QString("串口错误：%1").arg(m_port->errorString()));

    // 不可恢复的故障（设备被拔 / 资源不可用 / 权限问题）：停轮询 + 关端口。
    // 否则界面会"以为还在采集"，实际上每一台设备都在超时失败，用户只能靠失败计数猜。
    // 注意不含 TimeoutError：读写超时是瞬时的，重试机制自己会处理，不能因此断线。
    const bool fatal = (e == QSerialPort::ResourceError
                        || e == QSerialPort::DeviceNotFoundError
                        || e == QSerialPort::PermissionError);
    if (!fatal || !m_connected) return;

    if (m_polling) {
        stopPoll();
        emit logMessage(QStringLiteral("检测到串口故障，已自动停止轮询"));
    }
    m_port->close();
    m_connected = false;
    emit portStateChanged(false);
    emit statusChanged(QStringLiteral("串口故障：已断开，请检查连接后重新打开串口"));
}

void SerialWorker::startPoll(const QVector<DeviceSpec> &devices,
                             int timeoutMs, int intervalMs, int maxPoll)
{
    if (!m_connected) {
        emit logMessage("还没连接串口，无法开始轮询");
        return;
    }
    if (devices.isEmpty()) {
        emit logMessage("设备列表为空，无法开始轮询");
        return;
    }

    m_devices = devices;
    m_deviceIndex = 0;
    m_round = 0;
    m_timeoutMs = timeoutMs;
    m_intervalMs = intervalMs;
    m_maxPoll = maxPoll;
    m_successCount = m_failCount = m_retryCount = 0;

    m_polling = true;
    emit logMessage(QString("开始轮询 %1 台设备：间隔 %2 ms，超时 %3 ms，轮数 %4")
                        .arg(m_devices.size()).arg(m_intervalMs).arg(m_timeoutMs)
                        .arg(m_maxPoll > 0 ? QString::number(m_maxPoll) : QStringLiteral("不限")));
    for (const DeviceSpec &d : m_devices)
        emit logMessage(QString("    · %1").arg(d.describe()));
    emit statusChanged(m_maxPoll > 0 ? QString("正在轮询 %1 台设备（共 %2 轮）")
                                           .arg(m_devices.size()).arg(m_maxPoll)
                                     : QString("正在轮询 %1 台设备（持续采集）")
                                           .arg(m_devices.size()));
    emit pollingStateChanged(true);

    onPollTimeout();                 // 立即发第一个请求，不用等第一个间隔
    if (m_polling)                   // 只轮一轮时，上面那次可能已经结束了
        m_pollTimer->start(m_intervalMs);
}

void SerialWorker::stopPoll()
{
    if (!m_polling) return;
    m_polling = false;
    m_pollTimer->stop();
    emit logMessage(QString("停止轮询（累计成功 %1，失败 %2，重试 %3）")
                        .arg(m_successCount).arg(m_failCount).arg(m_retryCount));
    emit statusChanged("已连接（未轮询）");
    emit pollingStateChanged(false);
}

QByteArray SerialWorker::buildRequest(const DeviceSpec &dev) const
{
    return buildReadRequest(static_cast<quint8>(dev.slave),
                            static_cast<quint8>(dev.func),
                            static_cast<quint16>(dev.addr),
                            static_cast<quint16>(dev.count));
}

void SerialWorker::onPollTimeout()
{
    if (!m_polling || m_devices.isEmpty()) return;
    if (m_deviceIndex < 0 || m_deviceIndex >= m_devices.size())
        m_deviceIndex = 0;                      // 防御：索引越界就从头来

    // 定时器是「周期性」的，每次到点访问列表里的下一台设备；
    // 所以这里只处理一台，不用重启定时器。
    const DeviceSpec dev = m_devices.at(m_deviceIndex);

    bool ok = false;
    int attempts = 0;

    for (int i = 0; i < kMaxAttempts && m_polling; ++i) {
        ++attempts;
        if (doOneRequest(dev)) { ok = true; break; }
        if (i + 1 < kMaxAttempts) {
            ++m_retryCount;
            emit logMessage(QString("  [从站 %1] 第 %2 次重试…").arg(dev.slave).arg(i + 1));
        }
    }

    if (ok) {
        ++m_successCount;
    } else {
        ++m_failCount;
        emit logMessage(QString("[从站 %1] 本轮失败（共尝试 %2 次）").arg(dev.slave).arg(attempts));
    }
    emit pollStats(m_successCount, m_failCount, m_retryCount);

    // 推进到下一台；全部访问完算「一轮」
    ++m_deviceIndex;
    if (m_deviceIndex >= m_devices.size()) {
        m_deviceIndex = 0;
        ++m_round;
        emit roundFinished(m_round, m_successCount, m_failCount);
        emit logMessage(QString("第 %1 轮完成（累计成功 %2，失败 %3，重试 %4）")
                            .arg(m_round).arg(m_successCount).arg(m_failCount).arg(m_retryCount));

        // 达到轮数上限就自动停止（maxPoll = 0 表示不限，持续采集）
        if (m_maxPoll > 0 && m_round >= m_maxPoll) {
            emit logMessage(QString("已完成设定的 %1 轮，自动停止").arg(m_maxPoll));
            stopPoll();
        }
    }
}

bool SerialWorker::doOneRequest(const DeviceSpec &dev)
{
    // ① 发请求前清空接收缓冲：把上一次的残渣读走，
    //    否则迟到的响应 / 脏字节会变成这一次的「第一帧」，导致错位
    const QByteArray stale = m_port->readAll();
    if (!stale.isEmpty())
        emit logMessage(QString("  [从站 %1] 清空接收缓冲：丢掉残留 %2 字节")
                            .arg(dev.slave).arg(stale.size()));

    const QByteArray req = buildRequest(dev);
    emit logMessage(QString("  [从站 %1] → %2")
                        .arg(dev.slave)
                        .arg(QString::fromLatin1(req.toHex(' ')).toUpper()));
    if (m_port->write(req) != req.size()) {
        emit logMessage(QString("  [从站 %1] 写入失败：%2").arg(dev.slave).arg(m_port->errorString()));
        return false;
    }
    if (!m_port->waitForBytesWritten(m_timeoutMs)) {
        emit logMessage(QString("  [从站 %1] 写入超时：%2").arg(dev.slave).arg(m_port->errorString()));
        return false;
    }

    // ② 按「期望长度」判帧收完整一帧（分片到达也能补齐）
    QByteArray buf;
    const int wantLen = 3 + 2 * dev.count + 2;   // 正常读响应：地址+功能+字节数+数据+CRC
    const int excLen  = 5;                        // 异常响应固定 5 字节
    QElapsedTimer t;
    t.start();

    while (true) {
        // 关闭程序时 ~MainWindow 会 requestInterruption()：
        // 阻塞等待最坏要等满一个 timeout，这里每轮检查一次，把它压到 ≤50ms
        if (QThread::currentThread()->isInterruptionRequested())
            return false;
        if (buf.size() >= 5 && static_cast<quint8>(buf.at(0)) == dev.slave) {
            const quint8 f = static_cast<quint8>(buf.at(1));
            if (f == static_cast<quint8>(dev.func | 0x80) && buf.size() >= excLen) break;
            if ((f == 0x03 || f == 0x04) && buf.size() >= wantLen) break;
        }
        if (t.elapsed() >= m_timeoutMs) break;
        const int remain = m_timeoutMs - static_cast<int>(t.elapsed());
        if (remain <= 0) break;
        if (!m_port->waitForReadyRead(qMax(1, qMin(remain, 50)))) {
            if (buf.isEmpty() && t.elapsed() >= m_timeoutMs) break;
        }
        buf.append(m_port->readAll());
    }

    if (buf.isEmpty()) {
        emit logMessage(QString("  [从站 %1] 响应超时（%2 ms 内没收到任何字节）")
                            .arg(dev.slave).arg(m_timeoutMs));
        return false;
    }
    if (!checkCrc(buf)) {
        emit logMessage(QString("  [从站 %1] CRC 校验失败（收到 %2 字节）：%3")
                            .arg(dev.slave)
                            .arg(buf.size())
                            .arg(QString::fromLatin1(buf.toHex(' ')).toUpper()));
        return false;
    }
    if (buf.size() < 5) {
        emit logMessage(QString("  [从站 %1] 响应帧太短（%2 字节）").arg(dev.slave).arg(buf.size()));
        return false;
    }

    // ③ 解析（复用已验证的解析器）
    const ParsedResponse r = parseResponse(buf, static_cast<quint8>(dev.slave),
                                           static_cast<quint8>(dev.func));
    if (!r.ok) {
        emit logMessage(QString("  [从站 %1] 解析失败：%2").arg(dev.slave).arg(r.error));
        return false;
    }
    if (r.isException) {
        emit logMessage(QString("  [从站 %1] 从站返回异常响应：异常码 0x%2")
                            .arg(dev.slave)
                            .arg(r.exceptionCode, 2, 16, QChar('0')));
        return false;
    }
    if (r.regs.size() != dev.count) {
        emit logMessage(QString("  [从站 %1] 寄存器个数不符：请求 %2，收到 %3")
                            .arg(dev.slave).arg(dev.count).arg(r.regs.size()));
        return false;
    }

    // ④ 成功：原始帧写日志，数据交给界面
    emit logMessage(QString("  [从站 %1] ← %2")
                        .arg(dev.slave)
                        .arg(QString::fromLatin1(buf.toHex(' ')).toUpper()));
    emit dataReady(dev.slave, dev.func, dev.addr, r.regs);
    return true;
}

// ============================================================
// 写单个保持寄存器（0x06）——给设备下发阈值等参数
//
// 正常响应是「原样回显」8 字节，所以这里按 8 字节判帧；
// 与读请求一样，发送前要先清掉接收缓冲里的残渣，避免帧错位。
// ============================================================
void SerialWorker::writeRegister(int slave, int regAddr, int value)
{
    const auto fail = [&](const QString &msg) {
        emit logMessage(QString("  [从站 %1] 写寄存器 %2 = %3 失败：%4")
                            .arg(slave).arg(regAddr).arg(value).arg(msg));
        emit writeFinished(false, slave, regAddr, value, msg);
    };

    if (!m_connected || m_port == nullptr || !m_port->isOpen()) {
        fail(QStringLiteral("串口未打开"));
        return;
    }
    if (regAddr < 0 || regAddr > 0xFFFF || value < 0 || value > 0xFFFF) {
        fail(QStringLiteral("寄存器地址或数值超出 0~65535"));
        return;
    }

    const quint8  u8slave = static_cast<quint8>(slave);
    const quint16 u16reg  = static_cast<quint16>(regAddr);
    const quint16 u16val  = static_cast<quint16>(value);
    const QByteArray req = buildWriteSingleRequest(u8slave, u16reg, u16val);

    const QByteArray stale = m_port->readAll();
    if (!stale.isEmpty())
        emit logMessage(QString("  [从站 %1] 清空接收缓冲：丢掉残留 %2 字节")
                            .arg(slave).arg(stale.size()));

    emit logMessage(QString("  [从站 %1] 写寄存器 %2 = %3（0x06）→ %4")
                        .arg(slave).arg(regAddr).arg(value)
                        .arg(QString::fromLatin1(req.toHex(' ')).toUpper()));

    if (m_port->write(req) != req.size()) {
        fail(QString("写入失败：%1").arg(m_port->errorString()));
        return;
    }
    if (!m_port->waitForBytesWritten(m_timeoutMs)) {
        fail(QString("写入超时：%1").arg(m_port->errorString()));
        return;
    }

    QByteArray buf;
    QElapsedTimer t;
    t.start();
    while (buf.size() < 8 && t.elapsed() < m_timeoutMs) {
        const int remain = m_timeoutMs - static_cast<int>(t.elapsed());
        if (remain <= 0) break;
        if (!m_port->waitForReadyRead(qMax(1, qMin(remain, 50)))) {
            if (buf.isEmpty() && t.elapsed() >= m_timeoutMs) break;
        }
        buf.append(m_port->readAll());
    }

    if (buf.isEmpty()) {
        fail(QString("响应超时（%1 ms 内没收到任何字节）").arg(m_timeoutMs));
        return;
    }
    if (buf.size() < 8) {
        fail(QString("响应不完整（%1 字节，应为 8）").arg(buf.size()));
        return;
    }

    const ParsedResponse r = parseResponse(buf.left(8), u8slave, 0x06);
    if (!r.ok) {
        fail(QString("解析失败：%1").arg(r.error));
        return;
    }
    if (r.isException) {
        fail(QString("从站返回异常响应：异常码 0x%1")
                 .arg(r.exceptionCode, 2, 16, QChar('0')));
        return;
    }
    if (r.regs.isEmpty() || r.regs.first() != u16val) {
        fail(QString("回显值不符：期望 %1，收到 %2")
                 .arg(u16val).arg(r.regs.isEmpty() ? -1 : r.regs.first()));
        return;
    }

    emit logMessage(QString("  [从站 %1] ← %2  写入成功")
                        .arg(slave)
                        .arg(QString::fromLatin1(buf.left(8).toHex(' ')).toUpper()));
    emit writeFinished(true, slave, regAddr, value,
                       QString("寄存器 %1 已写为 %2").arg(regAddr).arg(value));
}
