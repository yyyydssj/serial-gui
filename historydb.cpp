#include "historydb.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>

HistoryDb::HistoryDb()
{
    // 每个实例用独立的连接名，避免多实例互相顶掉
    m_connName = QString("history_%1").arg(reinterpret_cast<quintptr>(this));
}

HistoryDb::~HistoryDb()
{
    close();
}

bool HistoryDb::open(const QString &dataDir)
{
    QDir dir(dataDir);
    if (!dir.exists() && !dir.mkpath(".")) {
        m_lastError = QString("无法创建数据目录：%1").arg(dataDir);
        return false;
    }
    m_path = dir.filePath("serial_gui.db");

    QSqlDatabase db = QSqlDatabase::contains(m_connName)
                          ? QSqlDatabase::database(m_connName)
                          : QSqlDatabase::addDatabase("QSQLITE", m_connName);
    db.setDatabaseName(m_path);

    if (!db.open()) {
        m_lastError = db.lastError().text();
        return false;
    }

    // WAL：写入不阻塞读，掉电/崩溃后恢复更稳
    exec("PRAGMA journal_mode=WAL");
    if (!exec("CREATE TABLE IF NOT EXISTS samples ("
              " id INTEGER PRIMARY KEY AUTOINCREMENT,"
              " ts TEXT NOT NULL,"
              " slave INTEGER NOT NULL,"
              " func INTEGER NOT NULL,"
              " addr INTEGER NOT NULL,"
              " value INTEGER NOT NULL,"
              " raw TEXT)")) {
        return false;
    }
    exec("CREATE INDEX IF NOT EXISTS idx_samples_ts ON samples(ts)");
    exec("CREATE INDEX IF NOT EXISTS idx_samples_key ON samples(slave, addr, ts)");

    // 报警事件表：每次「产生 / 解除」各记一行（只在状态变化时写，不会刷屏）
    if (!exec("CREATE TABLE IF NOT EXISTS alarms ("
              " id INTEGER PRIMARY KEY AUTOINCREMENT,"
              " ts TEXT NOT NULL,"
              " slave INTEGER NOT NULL,"
              " addr INTEGER NOT NULL,"
              " kind TEXT NOT NULL,"          // high / low
              " event TEXT NOT NULL,"         // raise / clear
              " value INTEGER NOT NULL,"
              " limit_value INTEGER NOT NULL)")) {
        return false;
    }
    exec("CREATE INDEX IF NOT EXISTS idx_alarms_ts ON alarms(ts)");
    exec("CREATE INDEX IF NOT EXISTS idx_alarms_key ON alarms(slave, addr, ts)");

    m_open = true;
    return true;
}

void HistoryDb::close()
{
    if (QSqlDatabase::contains(m_connName)) {
        {
            QSqlDatabase db = QSqlDatabase::database(m_connName);
            if (db.isOpen()) db.close();
        }
        QSqlDatabase::removeDatabase(m_connName);   // 必须让 db 对象先出作用域
    }
    m_open = false;
}

bool HistoryDb::isOpen() const
{
    return m_open;
}

bool HistoryDb::exec(const QString &sql) const
{
    QSqlQuery q(QSqlDatabase::database(m_connName));
    if (!q.exec(sql)) {
        m_lastError = QString("%1（SQL: %2）").arg(q.lastError().text(), sql);
        return false;
    }
    return true;
}

bool HistoryDb::insertSamples(int slave, int func, int addr,
                              const QVector<quint16> &regs, const QString &rawHex)
{
    if (!m_open) {
        m_lastError = "数据库未打开";
        return false;
    }

    QSqlDatabase db = QSqlDatabase::database(m_connName);
    // 显式事务：一次轮询的多行要么都写进去，要么都不写
    if (!db.transaction()) {
        m_lastError = db.lastError().text();
        return false;
    }

    const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    QSqlQuery q(db);
    q.prepare("INSERT INTO samples(ts, slave, func, addr, value, raw) "
              "VALUES(?, ?, ?, ?, ?, ?)");
    for (int i = 0; i < regs.size(); ++i) {
        q.bindValue(0, ts);
        q.bindValue(1, slave);
        q.bindValue(2, func);
        q.bindValue(3, addr + i);
        q.bindValue(4, static_cast<int>(regs.at(i)));
        q.bindValue(5, rawHex);
        if (!q.exec()) {
            m_lastError = q.lastError().text();
            db.rollback();
            return false;
        }
    }

    if (!db.commit()) {
        m_lastError = db.lastError().text();
        db.rollback();
        return false;
    }
    return true;
}

static QString buildWhere(const QString &from, const QString &to, int slave, int addr,
                          QVariantList &binds)
{
    QString where = " WHERE 1=1";
    if (!from.isEmpty()) { where += " AND ts >= ?"; binds << from; }
    if (!to.isEmpty())   { where += " AND ts <= ?"; binds << to; }
    if (slave > 0)       { where += " AND slave = ?"; binds << slave; }
    if (addr >= 0)       { where += " AND addr = ?";  binds << addr; }
    return where;
}

QList<SampleRow> HistoryDb::query(const QString &from, const QString &to,
                                  int slave, int addr, int limit) const
{
    QList<SampleRow> rows;
    if (!m_open) {
        m_lastError = "数据库未打开";
        return rows;
    }

    QVariantList binds;
    QString sql = "SELECT ts, slave, func, addr, value, IFNULL(raw,'') FROM samples"
                  + buildWhere(from, to, slave, addr, binds)
                  + " ORDER BY id DESC";
    if (limit > 0) sql += QString(" LIMIT %1").arg(limit);

    QSqlQuery q(QSqlDatabase::database(m_connName));
    q.prepare(sql);
    for (int i = 0; i < binds.size(); ++i) q.bindValue(i, binds.at(i));

    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return rows;
    }
    while (q.next()) {
        SampleRow r;
        r.ts    = q.value(0).toString();
        r.slave = q.value(1).toInt();
        r.func  = q.value(2).toInt();
        r.addr  = q.value(3).toInt();
        r.value = q.value(4).toInt();
        r.raw   = q.value(5).toString();
        rows << r;
    }
    return rows;
}

int HistoryDb::exportCsv(const QString &csvPath, const QString &from, const QString &to,
                         int slave, int addr) const
{
    const QList<SampleRow> rows = query(from, to, slave, addr, 0);
    if (!m_open) return -1;

    // 注意用 setGenerateByteOrderMark(true)：Excel 打开中文 CSV 才不会乱码
    QFile f(csvPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_lastError = QString("无法写入 %1：%2").arg(csvPath, f.errorString());
        return -1;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts.setGenerateByteOrderMark(true);
    ts << "时间,从站,功能码,寄存器地址,值(十进制),值(十六进制),原始帧\n";
    for (const SampleRow &r : rows) {
        // 注意：只把数字部分转大写，否则 "0x" 会被一起变成 "0X"
        const QString hexVal = "0x" + QString::number(r.value, 16).rightJustified(4, QChar('0')).toUpper();
        ts << r.ts << ','
           << r.slave << ','
           << QString("0x%1").arg(r.func, 2, 16, QChar('0')) << ','
           << r.addr << ','
           << r.value << ','
           << hexVal << ','
           << r.raw << '\n';
    }
    ts.flush();
    f.close();
    return rows.size();
}

int HistoryDb::clearAll()
{
    if (!m_open) {
        m_lastError = "数据库未打开";
        return -1;
    }
    // 采样数据和报警记录一起清空（「清空数据」按钮的语义就是全清）
    const int before = count() + alarmCount();
    if (!exec("DELETE FROM samples")) return -1;
    if (!exec("DELETE FROM alarms")) return -1;
    if (!exec("VACUUM")) { /* VACUUM 失败不影响数据已删除，忽略 */ }
    return before;
}

int HistoryDb::count() const
{
    if (!m_open) return -1;
    QSqlQuery q(QSqlDatabase::database(m_connName));
    if (!q.exec("SELECT COUNT(*) FROM samples") || !q.next()) {
        m_lastError = q.lastError().text();
        return -1;
    }
    return q.value(0).toInt();
}

// ---------------- 报警事件 ----------------

bool HistoryDb::insertAlarm(int slave, int addr, const QString &kind,
                            const QString &event, int value, int limit)
{
    if (!m_open) {
        m_lastError = "数据库未打开";
        return false;
    }
    const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    QSqlQuery q(QSqlDatabase::database(m_connName));
    q.prepare("INSERT INTO alarms(ts, slave, addr, kind, event, value, limit_value) "
              "VALUES(?, ?, ?, ?, ?, ?, ?)");
    q.bindValue(0, ts);
    q.bindValue(1, slave);
    q.bindValue(2, addr);
    q.bindValue(3, kind);
    q.bindValue(4, event);
    q.bindValue(5, value);
    q.bindValue(6, limit);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

QList<AlarmEvent> HistoryDb::queryAlarms(const QString &from, const QString &to,
                                         int slave, int limit) const
{
    QList<AlarmEvent> rows;
    if (!m_open) {
        m_lastError = "数据库未打开";
        return rows;
    }

    QVariantList binds;
    QString where = " WHERE 1=1";
    if (!from.isEmpty()) { where += " AND ts >= ?";  binds << from; }
    if (!to.isEmpty())   { where += " AND ts <= ?";  binds << to; }
    if (slave > 0)       { where += " AND slave = ?"; binds << slave; }

    QString sql = "SELECT ts, slave, addr, kind, event, value, limit_value FROM alarms"
                  + where + " ORDER BY id DESC";
    if (limit > 0) sql += QString(" LIMIT %1").arg(limit);

    QSqlQuery q(QSqlDatabase::database(m_connName));
    q.prepare(sql);
    for (int i = 0; i < binds.size(); ++i) q.bindValue(i, binds.at(i));
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return rows;
    }
    while (q.next()) {
        AlarmEvent a;
        a.ts    = q.value(0).toString();
        a.slave = q.value(1).toInt();
        a.addr  = q.value(2).toInt();
        a.kind  = q.value(3).toString();
        a.event = q.value(4).toString();
        a.value = q.value(5).toInt();
        a.limit = q.value(6).toInt();
        rows << a;
    }
    return rows;
}

int HistoryDb::alarmCount() const
{
    if (!m_open) return -1;
    QSqlQuery q(QSqlDatabase::database(m_connName));
    if (!q.exec("SELECT COUNT(*) FROM alarms") || !q.next()) {
        m_lastError = q.lastError().text();
        return -1;
    }
    return q.value(0).toInt();
}

int HistoryDb::exportAlarmsCsv(const QString &csvPath, const QString &from, const QString &to,
                               int slave) const
{
    const QList<AlarmEvent> rows = queryAlarms(from, to, slave, 0);
    if (!m_open) return -1;

    QFile f(csvPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_lastError = QString("无法写入 %1：%2").arg(csvPath, f.errorString());
        return -1;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts.setGenerateByteOrderMark(true);       // Excel 打开中文不乱码
    ts << "时间,从站,寄存器地址,类型,事件,值,阈值\n";
    for (const AlarmEvent &a : rows) {
        ts << a.ts << ','
           << a.slave << ','
           << a.addr << ','
           << (a.kind == QLatin1String("low") ? QStringLiteral("超下限") : QStringLiteral("超上限")) << ','
           << (a.event == QLatin1String("raise") ? QStringLiteral("产生") : QStringLiteral("解除")) << ','
           << a.value << ','
           << a.limit << '\n';
    }
    ts.flush();
    f.close();
    return rows.size();
}
