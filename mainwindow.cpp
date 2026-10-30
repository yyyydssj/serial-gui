#include "mainwindow.h"

#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QSerialPortInfo>
#include <QSettings>
#include <QStatusBar>
#include <QVBoxLayout>

#include "historydb.h"
#include "historydialog.h"
#include "modbus_frame.h"   // 复用 buildReadResponse 造原始帧文本
#include "serialworker.h"
#include "ui_mainwindow.h"  // 由 mainwindow.ui 经 uic 生成（CMake AUTOUIC）

// ============================================================
// 历史数据查看对话框（按时间/从站/地址过滤，QTableView 显示）

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    // 下面这些是「运行期才知道」的设置，.ui 里表达不了，所以留在代码里：
    setupRuntimeUi();

    // 跨线程信号槽要传自定义类型（设备列表），队列连接要求它先注册成元类型，
    // 否则运行时只会打印 "Cannot queue arguments of type ..." 然后静默丢消息。
    qRegisterMetaType<QVector<DeviceSpec>>("QVector<DeviceSpec>");

    // ---------------- 启动工作线程 ----------------
    // 标准 Qt 线程模型：worker 用 moveToThread 挪到工作线程；
    // 工作线程有自己的 exec()，串口的 readyRead / 定时器在那边正常派发，
    // 因此界面这边完全不需要 processEvents，主线程也永远不会被串口阻塞。
    m_thread = new QThread(this);
    m_worker = new SerialWorker;          // 注意：没有 parent，稍后 moveToThread
    m_worker->moveToThread(m_thread);

    connect(m_thread, &QThread::started, m_worker, &SerialWorker::init);

    // 界面 → 工作线程（这些信号都是「发给 worker 的槽」，自动走队列连接）
    connect(this, &MainWindow::openPortRequested, m_worker, &SerialWorker::openPort);
    connect(this, &MainWindow::closePortRequested, m_worker, &SerialWorker::closePort);
    connect(this, &MainWindow::startPollRequested, m_worker, &SerialWorker::startPoll);
    connect(this, &MainWindow::stopPollRequested, m_worker, &SerialWorker::stopPoll);

    // 工作线程 → 界面
    connect(m_worker, &SerialWorker::logMessage, this, &MainWindow::onLog);
    connect(m_worker, &SerialWorker::dataReady, this, &MainWindow::onDataReady);
    connect(m_worker, &SerialWorker::statusChanged, this, &MainWindow::onStatus);
    connect(m_worker, &SerialWorker::portStateChanged, this, &MainWindow::onPortState);
    connect(m_worker, &SerialWorker::pollStats, this, &MainWindow::onPollStats);
    connect(m_worker, &SerialWorker::pollingStateChanged, this, &MainWindow::onPollingState);
    connect(m_worker, &SerialWorker::writeFinished, this, &MainWindow::onWriteFinished);

    // 线程收尾：先停轮询、再退线程（否则工作线程里的 QSerialPort 可能在错误的线程析构）
    connect(m_thread, &QThread::finished, m_worker, [this]() {
        m_worker->stopPoll();
        m_worker->closePort();
    });

    m_thread->start();
    onRefreshPorts();
    // 数据库位置来自配置文件（上次选过就沿用，没选过就用默认位置）
    m_dbDir = loadDbDirSetting();
    refreshDbPathLabel();
    onLog("就绪：选好串口和参数后点「打开串口」，再点「开始轮询」。");
}

MainWindow::~MainWindow()
{
    if (m_thread) {
        // 先请求中断：worker 里 doOneRequest 的阻塞等待每轮会检查一次，
        // 能把关闭延迟从"一个串口超时"压到 ≤50ms。只 quit() 是不够的——
        // quit() 要等当前这次请求处理完才生效。
        m_thread->requestInterruption();
        m_thread->quit();
        if (!m_thread->wait(3000)) {
            // 3 秒没退，说明还卡在阻塞调用里（超时配得很大）。再多等一轮；
            // 绝不能在"线程还活着"时 delete worker，也不能让 QThread 被析构。
            if (!m_thread->wait(5000)) {
                // 极端兜底：把线程对象从父对象上摘下来，交给进程退出回收。
                // 宁可泄漏一个 worker，也不跨线程析构（那是必崩）。
                m_thread->setParent(nullptr);
                return;
            }
        }
    }
    delete m_worker;      // 线程已确认结束，此时在主线程删除是安全的
}

// .ui 里只能描述「静态」的界面结构与属性；下面这些必须写在代码里：
//   1. 带 userData 的下拉项（.ui 的 item 只能写文本，写不了 QVariant 的 userData）
//   2. 运行期才知道的控件（状态栏右侧那个统计标签）
//   3. 控件到槽函数的连接
void MainWindow::setupRuntimeUi()
{
    const int bauds[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
    for (int b : bauds)
        ui->m_baudBox->addItem(QString::number(b), b);
    ui->m_baudBox->setCurrentText(QStringLiteral("115200"));

    ui->m_dbPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);//设置文本可拖选（复制）

    // 表格列宽铺满窗口：属于 QHeaderView 的属性，.ui 里设不了（上一轮重构漏过这一句）
    ui->m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->m_deviceTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->m_alarmTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    // 「下发阈值到设备」按钮（0x06 写单个寄存器）
    connect(ui->m_sendTargetBtn, &QPushButton::clicked,
            this, &MainWindow::onSendTargetClicked);

    // 设备列表：默认放一台（保持「单设备」开箱即用的体验）
    addDeviceRow(1, 3, 0, 2);

    // 报警规则：默认放一条演示规则（从站 1 / 地址 0：上限 100、回差 5）
    {
        AlarmRule demo;
        demo.slave = 1;
        demo.addr = 0;
        demo.hasLow = false;      // 只查上限
        demo.hasHigh = true;
        demo.high = 100;
        demo.hysteresis = 5;
        addAlarmRow(demo);
    }

    // 状态栏右侧的累计统计（运行期创建，所以不在 .ui 里）
    statusBar()->showMessage(QStringLiteral("未连接"));
    m_statsLabel = new QLabel(QStringLiteral("成功0/失败0/重试0 ｜ 报警中0(累计0)"), this);
    statusBar()->addPermanentWidget(m_statsLabel);

    connect(ui->m_refreshBtn, &QPushButton::clicked, this, &MainWindow::onRefreshPorts);
    connect(ui->m_openBtn, &QPushButton::clicked, this, &MainWindow::onOpenClicked);
    connect(ui->m_addDeviceBtn, &QPushButton::clicked, this, &MainWindow::onAddDevice);
    connect(ui->m_delDeviceBtn, &QPushButton::clicked, this, &MainWindow::onDeleteDevice);
    connect(ui->m_addAlarmBtn, &QPushButton::clicked, this, &MainWindow::onAddAlarmRule);
    connect(ui->m_delAlarmBtn, &QPushButton::clicked, this, &MainWindow::onDeleteAlarmRule);
    connect(ui->m_startBtn, &QPushButton::clicked, this, &MainWindow::onStartPollClicked);
    connect(ui->m_stopBtn, &QPushButton::clicked, this, &MainWindow::onStopPollClicked);
    connect(ui->m_recordBox, &QCheckBox::toggled, this, &MainWindow::onRecordToggled);
    connect(ui->m_chooseDbDirBtn, &QPushButton::clicked, this, [this]() { chooseDbDir(); });
    connect(ui->m_historyBtn, &QPushButton::clicked, this, &MainWindow::onShowHistory);
    connect(ui->m_exportBtn, &QPushButton::clicked, this, &MainWindow::onExportCsv);
    connect(ui->m_clearBtn, &QPushButton::clicked, this, &MainWindow::onClearDb);

    // 编辑设备表（双击改单元格）时，把「启用」列同步成复选框状态
    connect(ui->m_deviceTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (item && item->column() == 0)
            item->setText(item->checkState() == Qt::Checked ? QStringLiteral("是")
                                                            : QStringLiteral("否"));
    });
}

int MainWindow::currentBaud() const
{
    return ui->m_baudBox->currentData().toInt();
}

void MainWindow::onRefreshPorts()
{
    const QString keep = ui->m_portBox->currentText();
    ui->m_portBox->clear();
    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : ports) {
        // 描述里带上设备名，便于区分虚拟串口 / CH340 / 蓝牙串口
        ui->m_portBox->addItem(QString("%1  %2").arg(info.portName(), info.description()),
                           info.portName());
    }
    if (ui->m_portBox->count() == 0)
        onLog("没有检测到任何串口");
    else if (!keep.isEmpty())
        ui->m_portBox->setCurrentText(keep);
}

void MainWindow::onOpenClicked()
{
    if (!m_connected) {
        if (ui->m_portBox->count() == 0) {
            onLog("没有可用串口，先插好设备再点「刷新」");
            return;
        }
        const QString name = ui->m_portBox->currentData().toString();
        onLog(QString("请求打开 %1 @ %2").arg(name).arg(currentBaud()));
        emit openPortRequested(name, currentBaud());
    } else {
        emit closePortRequested();
    }
}

// 把设备表的单元格文本解析成整数（支持 "0x03" 这种十六进制写法），解析不了就用 fallback
static int cellToInt(QTableWidget *table, int row, int col, int fallback)
{
    const QTableWidgetItem *item = table->item(row, col);
    if (!item) return fallback;
    const QString s = item->text().trimmed();
    bool ok = false;
    const int v = s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)
                      ? s.mid(2).toInt(&ok, 16)
                      : s.toInt(&ok, 10);
    return ok ? v : fallback;
}

void MainWindow::onStartPollClicked()
{
    const QVector<DeviceSpec> devices = collectDevices();
    if (devices.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("没有可轮询的设备"),
                             QStringLiteral("请至少勾选一台设备，并确认参数合法：\n"
                                            "从站 1~247、功能码 3 或 4、起始 0~65535、寄存器数 1~125。"));
        return;
    }
    prepareResultTable(devices);

    // 报警规则：重新下发（引擎会清空存量报警状态，按新阈值重新判定）
    const QVector<AlarmRule> rules = collectAlarmRules();
    m_alarm.setRules(rules);
    m_alarmRaised = 0;
    if (!rules.isEmpty())
        onLog(QString("已载入 %1 条报警规则").arg(rules.size()));
    refreshStatsLabel();

    emit startPollRequested(devices,
                            ui->m_timeoutSpin->value(),
                            ui->m_intervalSpin->value(),
                            ui->m_maxPollSpin->value());     // 0 = 不限
}

void MainWindow::onStopPollClicked()
{
    emit stopPollRequested();
}

// ---------------- 设备列表（多设备轮询）----------------

void MainWindow::addDeviceRow(int slave, int func, int addr, int count)
{
    const int row = ui->m_deviceTable->rowCount();//返回表格当前已有的行数
    ui->m_deviceTable->insertRow(row);//在指定索引位置插入一个空行

    // 第 0 列是「启用」复选框（必须是 checkable 的 item，才能勾）
    auto *enableItem = new QTableWidgetItem(QStringLiteral("是"));
    enableItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    enableItem->setCheckState(Qt::Checked);
    enableItem->setTextAlignment(Qt::AlignCenter);
    ui->m_deviceTable->setItem(row, 0, enableItem);

    auto put = [&](int col, const QString &text) {
        auto *item = new QTableWidgetItem(text);
        item->setTextAlignment(Qt::AlignCenter);
        ui->m_deviceTable->setItem(row, col, item);
    };
    put(1, QString::number(slave));
    put(2, QString("0x%1").arg(func, 2, 16, QChar('0')));
    put(3, QString::number(addr));
    put(4, QString::number(count));
}

void MainWindow::onAddDevice()
{
    // 默认沿用最后一台的功能码/起始/寄存器数，从站地址 +1（连续编号是最常见的情况）
    int nextSlave = 1, func = 3, addr = 0, count = 2;
    const int last = ui->m_deviceTable->rowCount() - 1;
    if (last >= 0) {
        nextSlave = qMin(247, cellToInt(ui->m_deviceTable, last, 1, 1) + 1);
        func  = cellToInt(ui->m_deviceTable, last, 2, 3);
        addr  = cellToInt(ui->m_deviceTable, last, 3, 0);
        count = cellToInt(ui->m_deviceTable, last, 4, 2);
    }
    addDeviceRow(nextSlave, func, addr, count);
    onLog(QString("已添加设备：从站 %1（当前共 %2 台）")
              .arg(nextSlave).arg(ui->m_deviceTable->rowCount()));
}

void MainWindow::onDeleteDevice()
{
    const int row = ui->m_deviceTable->currentRow();
    if (row < 0) {
        QMessageBox::information(this, QStringLiteral("未选中"),
                                 QStringLiteral("请先在设备列表里选中一行再删除。"));
        return;
    }
    ui->m_deviceTable->removeRow(row);
    onLog(QString("已删除一行设备（当前共 %1 台）").arg(ui->m_deviceTable->rowCount()));
}

QVector<DeviceSpec> MainWindow::collectDevices()
{
    QVector<DeviceSpec> out;
    for (int row = 0; row < ui->m_deviceTable->rowCount(); ++row) {
        const QTableWidgetItem *enable = ui->m_deviceTable->item(row, 0);
        if (!enable || enable->checkState() != Qt::Checked)
            continue;                       // 没勾「启用」的不参与轮询

        DeviceSpec d;
        d.enabled = true;
        d.slave = cellToInt(ui->m_deviceTable, row, 1, 1);
        d.func  = cellToInt(ui->m_deviceTable, row, 2, 3);
        d.addr  = cellToInt(ui->m_deviceTable, row, 3, 0);
        d.count = cellToInt(ui->m_deviceTable, row, 4, 2);

        if (!d.isValid()) {
            onLog(QString("设备表第 %1 行参数不合法，已跳过：%2").arg(row + 1).arg(d.describe()));
            continue;
        }
        out << d;
    }
    return out;
}

void MainWindow::prepareResultTable(const QVector<DeviceSpec> &devices)
{
    m_rowIndex.clear();
    ui->m_table->setRowCount(0);

    int row = 0;
    for (const DeviceSpec &d : devices) {
        for (int i = 0; i < d.count; ++i) {
            ui->m_table->insertRow(row);
            auto put = [&](int col, const QString &text) {
                auto *item = new QTableWidgetItem(text);
                if (col == 0 || col >= 4) item->setTextAlignment(Qt::AlignCenter);
                ui->m_table->setItem(row, col, item);
            };
            put(0, QString::number(d.slave));
            put(1, QString("0x%1  (%2)").arg(d.addr + i, 4, 16, QChar('0')).arg(d.addr + i));
            put(2, QStringLiteral("-"));
            put(3, QStringLiteral("-"));
            put(4, AlarmEngine::levelText(AlarmEngine::Level::Normal));   // 状态列
            put(5, QStringLiteral("-"));
            m_rowIndex.insert(rowKey(d.slave, d.addr + i), row);
            ++row;
        }
    }
    onLog(QString("结果表已按设备列表重建：%1 台设备，共 %2 行").arg(devices.size()).arg(row));
}

void MainWindow::onLog(const QString &text)
{
    ui->m_log->appendPlainText(QString("[%1] %2")
                               .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), text));
}

void MainWindow::onDataReady(int slave, int func, int addr, const QVector<quint16> &regs)
{
    const QString now = QDateTime::currentDateTime().toString("HH:mm:ss");

    for (int i = 0; i < regs.size(); ++i) {
        const quint16 v = regs.at(i);

        // 先按 (从站, 地址) 找行；找不到就补一行，保证数据不会因为改过列表而丢掉
        int row = m_rowIndex.value(rowKey(slave, addr + i), -1);
        if (row < 0) {
            row = ui->m_table->rowCount();
            ui->m_table->insertRow(row);
            m_rowIndex.insert(rowKey(slave, addr + i), row);
        }

        auto setItem = [&](int col, const QString &text) {
            QTableWidgetItem *item = ui->m_table->item(row, col);
            if (!item) {
                item = new QTableWidgetItem;
                ui->m_table->setItem(row, col, item);
            }
            item->setText(text);
        };
        setItem(0, QString::number(slave));
        setItem(1, QString("0x%1  (%2)").arg(addr + i, 4, 16, QChar('0')).arg(addr + i));
        setItem(2, QString::number(v));
        setItem(3, "0x" + QString::number(v, 16).rightJustified(4, QChar('0')).toUpper());
        setItem(5, now);

        // ---------- 报警判定 ----------
        const AlarmEngine::Result ar = m_alarm.update(slave, addr + i, v);
        const bool alarming = (ar.level != AlarmEngine::Level::Normal);
        setItem(4, AlarmEngine::levelText(ar.level));

        // 标红：报警时把「值」两列和「状态」列涂红，恢复正常时清掉背景
        const QBrush alarmBrush(QColor(255, 205, 205));
        for (int col : {2, 3, 4}) {
            if (QTableWidgetItem *item = ui->m_table->item(row, col))
                item->setBackground(alarming ? alarmBrush : QBrush());
        }

        if (ar.changed) {
            if (ar.raised) {
                ++m_alarmRaised;
                onLog(QString("⚠ 报警：从站 %1 地址 %2 值 %3，超过%4 %5")
                          .arg(slave).arg(addr + i).arg(v)
                          .arg(ar.level == AlarmEngine::Level::High ? QStringLiteral("上限")
                                                                    : QStringLiteral("下限"))
                          .arg(ar.limit));
                if (ui->m_beepBox->isChecked())
                    QApplication::beep();
            } else if (ar.cleared) {
                onLog(QString("✔ 报警解除：从站 %1 地址 %2 值 %3 已回到正常范围")
                          .arg(slave).arg(addr + i).arg(v));
            }
            refreshStatsLabel();

            // 报警事件随「记录数据」开关一起写库（产生与解除各记一行）
            if (ui->m_recordBox->isChecked() && ensureDbOpen()) {
                // 是超上限还是超下限：产生时看结果电平，解除时看变化前的电平（from）
                const QString kind = (ar.level == AlarmEngine::Level::Low
                                      || ar.from == AlarmEngine::Level::Low)
                                         ? QStringLiteral("low") : QStringLiteral("high");
                if (!m_db->insertAlarm(slave, addr + i, kind,
                                       ar.raised ? QStringLiteral("raise")
                                                 : QStringLiteral("clear"),
                                       v, ar.limit))
                    onLog(QString("写报警事件失败：%1").arg(m_db->lastError()));
            }
        }
    }

    // 勾选了「记录数据」才写库
    if (ui->m_recordBox->isChecked() && ensureDbOpen()) {
        // 顺手把响应帧原文也存下来，便于事后核对（复用已验证的组帧函数）
        const QString rawHex = QString::fromLatin1(
            buildReadResponse(static_cast<quint8>(slave), static_cast<quint8>(func), regs)
                .toHex(' ')).toUpper();
        if (!m_db->insertSamples(slave, func, addr, regs, rawHex)) {
            onLog(QString("写数据库失败：%1").arg(m_db->lastError()));
            ui->m_recordBox->setChecked(false);   // 失败就停止记录，避免每轮都刷错误
            QMessageBox::warning(this, QStringLiteral("写数据库失败"), m_db->lastError());
        }
    }
}

// ---------------- 数据落地相关 ----------------

bool MainWindow::ensureDbOpen()
{
    if (m_db && m_db->isOpen()) return true;

    if (!m_db) m_db = new HistoryDb;
    // m_dbDir 在构造函数里从配置文件读入；没配过则用默认值（exe 旁边的 data/）
    if (!m_db->open(m_dbDir)) {
        QMessageBox::warning(this, QStringLiteral("打开数据库失败"), m_db->lastError());
        return false;
    }
    refreshDbPathLabel();
    onLog(QString("数据库已打开：%1（现有 %2 条记录）").arg(m_db->path()).arg(m_db->count()));
    return true;
}

// ---------------- 数据库位置与配置文件 ----------------

// 配置文件路径：exe 同目录的 serial_gui.ini（明文，可直接看/改）
// 用 IniFormat 而不是默认的注册表，方便学习和排查
static QString settingsFilePath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath("serial_gui.ini");
}

// 默认位置：exe 同目录的 data/
static QString defaultDbDir()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath("data");
}

QString MainWindow::loadDbDirSetting()
{
    QSettings s(settingsFilePath(), QSettings::IniFormat);
    const QString dir = s.value("database/dir").toString().trimmed();
    return dir.isEmpty() ? defaultDbDir() : dir;
}

void MainWindow::saveDbDirSetting(const QString &dir)
{
    QSettings s(settingsFilePath(), QSettings::IniFormat);
    s.setValue("database/dir", dir);
    s.sync();
}

void MainWindow::refreshDbPathLabel()
{
    if (m_db && m_db->isOpen())
        ui->m_dbPathLabel->setText(m_db->path());
    else
        ui->m_dbPathLabel->setText(QString("%1（尚未打开）")
                                   .arg(QDir(m_dbDir).filePath("serial_gui.db")));
}

void MainWindow::chooseDbDir()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择数据库保存位置"), m_dbDir);
    if (dir.isEmpty()) return;              // 用户取消

    // 已有数据库连接：先关掉再切换（否则新位置不会生效）
    if (m_db && m_db->isOpen()) {
        if (ui->m_recordBox->isChecked()) {
            ui->m_recordBox->setChecked(false);
            onLog("切换数据库位置：已停止记录");
        }
        m_db->close();
        onLog(QString("已关闭原数据库：%1").arg(m_db->path()));
    }
    m_dbDir = dir;
    saveDbDirSetting(dir);
    refreshDbPathLabel();
    onLog(QString("数据库位置已改为：%1（已写入配置文件 serial_gui.ini）")
              .arg(QDir(dir).filePath("serial_gui.db")));

    // 顺手打开一次，让界面立刻显示新路径、也验证目录可写
    if (!ensureDbOpen())
        QMessageBox::warning(this, QStringLiteral("切换失败"),
                             QStringLiteral("新位置无法创建数据库，请看日志。"));
}

void MainWindow::onRecordToggled(bool on)
{
    if (!on) {
        onLog("已停止记录数据");
        return;
    }
    if (!ensureDbOpen()) {
        ui->m_recordBox->setChecked(false);   // 打开失败就恢复未勾选
        return;
    }
    onLog("开始记录数据到数据库");
}

void MainWindow::onShowHistory()
{
    if (!ensureDbOpen()) return;
    // 非模态：主窗口保持可用。
    // 用模态 exec() 时会启动嵌套事件循环并禁用父窗口，表现为「历史窗口开着时主界面拖不动」。
    auto *dlg = new HistoryDialog(m_db, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);   // 关闭即 delete，避免每次点开泄漏一个对话框
    dlg->show();
}

void MainWindow::onExportCsv()
{
    if (!ensureDbOpen()) return;
    if (m_db->count() <= 0) {
        QMessageBox::information(this, QStringLiteral("没有数据"), QStringLiteral("数据库里还没有记录。"));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出全部数据"),
        QDir(QCoreApplication::applicationDirPath()).filePath("history_export.csv"),
        QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty()) return;

    const int n = m_db->exportCsv(path, QString(), QString(), 0, -1);
    if (n < 0)
        QMessageBox::warning(this, QStringLiteral("导出失败"), m_db->lastError());
    else
        QMessageBox::information(this, QStringLiteral("导出完成"),
                                 QString("已导出 %1 行到：\n%2").arg(n).arg(path));
}

void MainWindow::onClearDb()
{
    if (!ensureDbOpen()) return;
    const int n = m_db->count();
    if (n <= 0) {
        QMessageBox::information(this, QStringLiteral("没有数据"), QStringLiteral("数据库里还没有记录。"));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("确认清空"),
                              QString("将删除全部 %1 条记录，且无法恢复。继续吗？").arg(n))
        != QMessageBox::Yes)
        return;
    const int deleted = m_db->clearAll();
    if (deleted < 0)
        QMessageBox::warning(this, QStringLiteral("清空失败"), m_db->lastError());
    else
        onLog(QString("已清空数据库（删除 %1 条）").arg(deleted));
}

void MainWindow::onStatus(const QString &text)
{
    statusBar()->showMessage(text);
}

void MainWindow::onPortState(bool open)
{
    m_connected = open;
    ui->m_openBtn->setText(open ? QStringLiteral("关闭串口") : QStringLiteral("打开串口"));
    // 未连接时不能开始轮询；连接状态变化时按钮随之更新
    ui->m_startBtn->setEnabled(open && !m_polling);
    ui->m_stopBtn->setEnabled(open && m_polling);
    if (!open) {
        m_polling = false;
        ui->m_stopBtn->setEnabled(false);
    }
}

void MainWindow::onPollingState(bool polling)
{
    m_polling = polling;
    ui->m_startBtn->setEnabled(m_connected && !polling);
    ui->m_stopBtn->setEnabled(m_connected && polling);
    // 轮询期间设备列表不再允许修改，避免「改了列表但实际轮询的还是旧列表」
    ui->m_deviceTable->setEnabled(!polling);
    ui->m_addDeviceBtn->setEnabled(!polling);
    ui->m_delDeviceBtn->setEnabled(!polling);
    ui->m_alarmTable->setEnabled(!polling);
    ui->m_addAlarmBtn->setEnabled(!polling);
    ui->m_delAlarmBtn->setEnabled(!polling);
}

void MainWindow::onPollStats(int success, int fail, int retry)
{
    m_statOk = success;
    m_statFail = fail;
    m_statRetry = retry;
    refreshStatsLabel();
}

void MainWindow::refreshStatsLabel()
{
    // 把「当前还在报警的寄存器数」也显示出来，一眼能看到现场状态
    const int nowAlarming = m_alarm.alarmCount();
    m_statsLabel->setText(QString("成功%1/失败%2/重试%3 ｜ 报警中%4(累计%5)")
                              .arg(m_statOk).arg(m_statFail).arg(m_statRetry)
                              .arg(nowAlarming).arg(m_alarmRaised));
}

// ---------------- 报警规则 ----------------

void MainWindow::addAlarmRow(const AlarmRule &rule)
{
    const int row = ui->m_alarmTable->rowCount();
    ui->m_alarmTable->insertRow(row);

    // 第 0 列：启用（复选框）
    auto *enableItem = new QTableWidgetItem(rule.enabled ? QStringLiteral("是") : QStringLiteral("否"));
    enableItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    enableItem->setCheckState(rule.enabled ? Qt::Checked : Qt::Unchecked);
    enableItem->setTextAlignment(Qt::AlignCenter);
    ui->m_alarmTable->setItem(row, 0, enableItem);

    auto put = [&](int col, const QString &text) {
        auto *item = new QTableWidgetItem(text);
        item->setTextAlignment(Qt::AlignCenter);
        ui->m_alarmTable->setItem(row, col, item);
    };
    put(1, QString::number(rule.slave));
    put(2, QString::number(rule.addr));
    // 下限/上限留空 = 不检查这一侧
    put(3, rule.hasLow  ? QString::number(rule.low)  : QString());
    put(4, rule.hasHigh ? QString::number(rule.high) : QString());
    put(5, QString::number(rule.hysteresis));
}

void MainWindow::onAddAlarmRule()
{
    // 默认沿用最后一行的阈值，从站/地址取设备表第一台（最常见的用法：给第一个寄存器加个上限）
    AlarmRule r;
    const int last = ui->m_alarmTable->rowCount() - 1;
    if (last >= 0) {
        r.slave = cellToInt(ui->m_alarmTable, last, 1, 1);
        r.addr  = cellToInt(ui->m_alarmTable, last, 2, 0) + 1;   // 下一个寄存器
        r.hasLow  = !ui->m_alarmTable->item(last, 3)->text().trimmed().isEmpty();
        r.low     = cellToInt(ui->m_alarmTable, last, 3, 0);
        r.hasHigh = !ui->m_alarmTable->item(last, 4)->text().trimmed().isEmpty();
        r.high    = cellToInt(ui->m_alarmTable, last, 4, 100);
        r.hysteresis = cellToInt(ui->m_alarmTable, last, 5, 0);
    }
    addAlarmRow(r);
    onLog(QString("已添加报警规则：%1").arg(r.describe()));
}

void MainWindow::onDeleteAlarmRule()
{
    const int row = ui->m_alarmTable->currentRow();
    if (row < 0) {
        QMessageBox::information(this, QStringLiteral("未选中"),
                                 QStringLiteral("请先在报警规则里选中一行再删除。"));
        return;
    }
    ui->m_alarmTable->removeRow(row);
    onLog(QString("已删除一行报警规则（当前共 %1 条）").arg(ui->m_alarmTable->rowCount()));
}

QVector<AlarmRule> MainWindow::collectAlarmRules()
{
    QVector<AlarmRule> out;
    for (int row = 0; row < ui->m_alarmTable->rowCount(); ++row) {
        const QTableWidgetItem *enable = ui->m_alarmTable->item(row, 0);
        if (!enable || enable->checkState() != Qt::Checked)
            continue;

        AlarmRule r;
        r.enabled = true;
        r.slave = cellToInt(ui->m_alarmTable, row, 1, 1);
        r.addr  = cellToInt(ui->m_alarmTable, row, 2, 0);

        // 下限/上限：格子留空表示这一侧不检查
        const QTableWidgetItem *lowItem  = ui->m_alarmTable->item(row, 3);
        const QTableWidgetItem *highItem = ui->m_alarmTable->item(row, 4);
        const bool hasLow  = lowItem  && !lowItem->text().trimmed().isEmpty();
        const bool hasHigh = highItem && !highItem->text().trimmed().isEmpty();
        r.hasLow  = hasLow;
        r.low     = hasLow  ? cellToInt(ui->m_alarmTable, row, 3, 0)   : 0;
        r.hasHigh = hasHigh;
        r.high    = hasHigh ? cellToInt(ui->m_alarmTable, row, 4, 100) : 0;
        r.hysteresis = cellToInt(ui->m_alarmTable, row, 5, 0);

        if (!r.isValid()) {
            onLog(QString("报警规则第 %1 行不合法，已跳过（需至少检查一侧、且下限 ≤ 上限）").arg(row + 1));
            continue;
        }
        out << r;
    }
    return out;
}

// ============================================================
// 下发阈值到设备（Modbus 0x06 写单个保持寄存器）
//
// 为什么需要它：设备侧报警（蜂鸣器）用的是【设备自己】的阈值寄存器，
// 界面上的「报警规则」只是 PC 侧的判定，两者互不影响。
// 要把界面里设的阈值真正作用到设备上，必须走 0x06 写下去。
//
// 注意：跨线程调用 worker 的槽必须用 QueuedConnection，
//       直接在界面线程里调 m_worker->writeRegister() 会在界面线程执行串口操作。
// ============================================================
void MainWindow::onSendTargetClicked()
{
    if (!m_connected) {
        onLog(QStringLiteral("下发阈值失败：还没连接串口"));
        return;
    }

    const int slave = ui->m_sendSlaveSpin->value();
    const int reg   = ui->m_sendRegSpin->value();
    const double celsius = ui->m_targetSpinBox->value();
    const int value = qRound(celsius * 10.0);      // 设备侧用 ℃×10 存整数

    if (value < 0 || value > 65535) {
        onLog(QString("下发阈值失败：%1 ℃ 换算后超出 16 位范围").arg(celsius, 0, 'f', 1));
        return;
    }

    onLog(QString("下发阈值：从站 %1，寄存器 %2 = %3（%4 ℃）")
              .arg(slave).arg(reg).arg(value).arg(celsius, 0, 'f', 1));

    QMetaObject::invokeMethod(m_worker, "writeRegister", Qt::QueuedConnection,
                              Q_ARG(int, slave), Q_ARG(int, reg), Q_ARG(int, value));
}

void MainWindow::onWriteFinished(bool ok, int slave, int regAddr, int value,
                                 const QString &message)
{
    if (ok) {
        onStatus(QString("阈值已下发：从站 %1 寄存器 %2 = %3（%4 ℃）")
                     .arg(slave).arg(regAddr).arg(value)
                     .arg(value / 10.0, 0, 'f', 1));
    } else {
        onStatus(QString("阈值下发失败：%1").arg(message));
    }
}
