#include "historydialog.h"
#include "ui_historydialog.h"   // 由 historydialog.ui 经 uic 生成（CMake AUTOUIC）

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QMessageBox>
#include <QStandardItemModel>

#include "historydb.h"

HistoryDialog::HistoryDialog(HistoryDb *db, QWidget *parent)
    : QDialog(parent), ui(new Ui::HistoryDialog), m_db(db)
{
    ui->setupUi(this);

    // 数据模型必须在代码里建（.ui 只能描述界面，不能放数据模型）
    m_model = new QStandardItemModel(0, 6, this);
    m_model->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("从站"),
                                        QStringLiteral("功能码"), QStringLiteral("寄存器地址"),
                                        QStringLiteral("值(十进制)"), QStringLiteral("值(十六进制)")});
    ui->m_view->setModel(m_model);
    // 列宽自适应：属于 QHeaderView 的属性，.ui 里设不了，放在代码里
    ui->m_view->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    connect(ui->m_queryBtn, &QPushButton::clicked, this, &HistoryDialog::reload);
    connect(ui->m_exportBtn, &QPushButton::clicked, this, &HistoryDialog::exportCurrent);
    connect(ui->m_kindBox, &QComboBox::currentIndexChanged, this, &HistoryDialog::reload);

    reload();   // 打开就先显示一批
}

HistoryDialog::~HistoryDialog()
{
    delete ui;
}

void HistoryDialog::reload()
{
    const QString from = ui->m_fromEdit->text().trimmed();
    const QString to   = ui->m_toEdit->text().trimmed();
    const int slave    = ui->m_slaveSpin->value();
    const int limit    = ui->m_limitSpin->value();
    const bool alarms  = (ui->m_kindBox->currentIndex() != 0);

    // 地址过滤只对采样数据有意义（报警表按 从站 + 地址 存，但查询接口没做地址过滤）
    ui->m_addrSpin->setEnabled(!alarms);
    ui->labelAddr->setEnabled(!alarms);

    m_model->removeRows(0, m_model->rowCount());

    if (!alarms) {
        // ---------------- 采样数据 ----------------
        m_model->setColumnCount(6);
        m_model->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("从站"),
                                            QStringLiteral("功能码"), QStringLiteral("寄存器地址"),
                                            QStringLiteral("值(十进制)"), QStringLiteral("值(十六进制)")});
        const QList<SampleRow> rows = m_db->query(from, to, slave, ui->m_addrSpin->value(), limit);
        for (const SampleRow &r : rows) {
            QList<QStandardItem *> items;
            items << new QStandardItem(r.ts)
                  << new QStandardItem(QString::number(r.slave))
                  << new QStandardItem(QString("0x%1").arg(r.func, 2, 16, QChar('0')))
                  << new QStandardItem(QString::number(r.addr))
                  << new QStandardItem(QString::number(r.value))
                  // 注意：只把数字部分转大写，否则 "0x" 会变成 "0X"
                  << new QStandardItem("0x" + QString::number(r.value, 16).rightJustified(4, QChar('0')).toUpper());
            m_model->appendRow(items);
        }
        ui->m_info->setText(QString("采样数据：共 %1 条（库内总计 %2 条）")
                                .arg(rows.size()).arg(m_db->count()));
    } else {
        // ---------------- 报警记录 ----------------
        m_model->setColumnCount(7);
        m_model->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("从站"),
                                            QStringLiteral("寄存器地址"), QStringLiteral("类型"),
                                            QStringLiteral("事件"), QStringLiteral("值"),
                                            QStringLiteral("阈值")});
        const QList<AlarmEvent> rows = m_db->queryAlarms(from, to, slave, limit);
        for (const AlarmEvent &a : rows) {
            QList<QStandardItem *> items;
            items << new QStandardItem(a.ts)
                  << new QStandardItem(QString::number(a.slave))
                  << new QStandardItem(QString::number(a.addr))
                  << new QStandardItem(a.kind == QLatin1String("low") ? QStringLiteral("超下限")
                                                                     : QStringLiteral("超上限"))
                  << new QStandardItem(a.event == QLatin1String("raise") ? QStringLiteral("产生")
                                                                        : QStringLiteral("解除"))
                  << new QStandardItem(QString::number(a.value))
                  << new QStandardItem(QString::number(a.limit));
            m_model->appendRow(items);
        }
        ui->m_info->setText(QString("报警记录：共 %1 条（库内总计 %2 条）")
                                .arg(rows.size()).arg(m_db->alarmCount()));
    }
}

void HistoryDialog::exportCurrent()
{
    const bool alarms = (ui->m_kindBox->currentIndex() != 0);
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出 CSV"),
        QDir(QCoreApplication::applicationDirPath())
            .filePath(alarms ? QStringLiteral("alarms_export.csv") : QStringLiteral("history_export.csv")),
        QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty()) return;

    const QString from = ui->m_fromEdit->text().trimmed();
    const QString to   = ui->m_toEdit->text().trimmed();
    const int slave    = ui->m_slaveSpin->value();

    const int n = alarms
                      ? m_db->exportAlarmsCsv(path, from, to, slave)
                      : m_db->exportCsv(path, from, to, slave, ui->m_addrSpin->value());
    if (n < 0)
        QMessageBox::warning(this, QStringLiteral("导出失败"), m_db->lastError());
    else
        QMessageBox::information(this, QStringLiteral("导出完成"),
                                 QString("已导出 %1 行到：\n%2").arg(n).arg(path));
}
