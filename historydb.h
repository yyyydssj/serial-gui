#ifndef HISTORYDB_H
#define HISTORYDB_H

// ============================================================
// HistoryDb —— 采集数据落地（SQLite 封装）
//
// 为什么放在界面线程：单条 INSERT 是微秒级，WAL 模式下不阻塞读；
// 通信线程只管收发，写库放在主线程最简单。等数据量真的很大（每秒上千点）
// 再把写库挪到独立线程——接口不用变。
//
// 表结构（一行一个寄存器值，SQL 查询/统计最方便）：
//   samples(id, ts, slave, func, addr, value, raw)
// ============================================================

#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

struct SampleRow {
    QString ts;        // 'YYYY-MM-DD HH:MM:SS.zzz'
    int     slave = 0;
    int     func = 0;
    int     addr = 0;
    int     value = 0; // 原始 16 位值
    QString raw;       // 原始响应帧 hex（便于事后核对）
};

// 一条报警事件（产生 / 解除）
struct AlarmEvent {
    QString ts;
    int     slave = 0;
    int     addr = 0;
    QString kind;      // "high" 超上限 / "low" 超下限
    QString event;     // "raise" 产生 / "clear" 解除
    int     value = 0; // 触发/解除时的值
    int     limit = 0; // 依据的阈值
};

class HistoryDb
{
public:
    HistoryDb();
    ~HistoryDb();

    // 打开（或创建）数据库文件；dataDir 不存在会自动创建。成功返回 true。
    bool open(const QString &dataDir);
    void close();
    bool isOpen() const;
    QString path() const { return m_path; }
    QString lastError() const { return m_lastError; }

    // 写入一批数据（一次轮询的所有寄存器）
    bool insertSamples(int slave, int func, int addr,
                       const QVector<quint16> &regs, const QString &rawHex);

    // 查询（limit <= 0 表示不限）
    QList<SampleRow> query(const QString &from, const QString &to,
                           int slave, int addr, int limit) const;

    // 导出为 CSV；返回导出的行数，失败返回 -1
    int exportCsv(const QString &csvPath, const QString &from, const QString &to,
                  int slave, int addr) const;

    // 清空全部数据；返回删除的行数，失败返回 -1
    int clearAll();

    int count() const;

    // ---------------- 报警事件 ----------------
    // 写入一条报警事件（产生或解除）
    bool insertAlarm(int slave, int addr, const QString &kind,
                     const QString &event, int value, int limit);
    // 查询报警事件（from/to 留空表示不限时间；slave <= 0 表示不限从站）
    QList<AlarmEvent> queryAlarms(const QString &from, const QString &to,
                                  int slave, int limit) const;
    // 导出报警事件为 CSV；返回行数，失败返回 -1
    int exportAlarmsCsv(const QString &csvPath, const QString &from, const QString &to,
                        int slave) const;
    int alarmCount() const;

private:
    bool exec(const QString &sql) const;

    mutable QString m_lastError;
    QString    m_path;
    QString    m_connName;
    bool       m_open = false;
};

#endif // HISTORYDB_H
