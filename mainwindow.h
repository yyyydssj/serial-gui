#ifndef MAINWINDOW_H
#define MAINWINDOW_H

// ============================================================
// MainWindow —— 界面（跑在主线程）
//
// 只负责：
//   1. 收集用户输入（串口、波特率、从站、功能码、地址、数量、超时、间隔）
//   2. 把指令通过【信号】发给工作线程里的 SerialWorker
//   3. 把 SerialWorker 回传的数据/日志显示出来
//
// 界面上不出现任何串口操作，工作线程里不出现任何控件 —— 这是跨线程的基本纪律。
// ============================================================

#include <QCheckBox>
#include <QComboBox>
#include <QHash>
#include <QLabel>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QThread>
#include <QVector>

#include "alarmengine.h"
#include "alarmrule.h"
#include "devicespec.h"

class SerialWorker;
class HistoryDb;

namespace Ui { class MainWindow; }

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    // 界面 → 工作线程的指令（跨线程自动走队列连接）
    void openPortRequested(const QString &portName, int baud);
    void closePortRequested();
    // devices = 要轮询的设备列表；maxPoll = 轮询轮数上限（0 = 不限）
    void startPollRequested(const QVector<DeviceSpec> &devices,
                            int timeoutMs, int intervalMs, int maxPoll);
    void stopPollRequested();

private slots:
    void onRefreshPorts();
    void onOpenClicked();
    void onStartPollClicked();
    void onStopPollClicked();
    void onAddDevice();                 // 设备列表：添加一行
    void onDeleteDevice();              // 设备列表：删除选中行
    void onAddAlarmRule();              // 报警规则：添加一行
    void onDeleteAlarmRule();           // 报警规则：删除选中行
    void onSendTargetClicked();         // 把阈值下发到设备（Modbus 0x06）
    void onWriteFinished(bool ok, int slave, int regAddr, int value, const QString &message);
    void onRecordToggled(bool on);      // 勾选/取消「记录数据」
    void onShowHistory();
    void onExportCsv();
    void onClearDb();

    // 以下槽都在主线程执行（跨线程信号自动排队投递）
    void onLog(const QString &text);
    void onDataReady(int slave, int func, int addr, const QVector<quint16> &regs);
    void onStatus(const QString &text);
    void onPortState(bool open);
    void onPollStats(int success, int fail, int retry);
    void onPollingState(bool polling);

private:
    void setupRuntimeUi();   // .ui 之外的运行期设置（下拉项 userData、状态栏统计、信号槽连接）
    int currentBaud() const;
    bool ensureDbOpen();     // 惰性打开数据库（第一次使用时才创建文件）
    QString loadDbDirSetting();                 // 从配置文件读数据库目录
    void saveDbDirSetting(const QString &dir);  // 写回配置文件
    void refreshDbPathLabel();                  // 刷新界面上显示的数据库路径
    void chooseDbDir();                         // 「选择数据库位置」按钮

    // 多设备轮询相关
    void addDeviceRow(int slave, int func, int addr, int count);  // 往设备表加一行
    QVector<DeviceSpec> collectDevices();                         // 读设备表 → 设备列表（只含启用的）
    void prepareResultTable(const QVector<DeviceSpec> &devices);  // 按设备列表重建结果表
    static int rowKey(int slave, int addr) { return slave * 65536 + addr; }

    // 报警相关
    void addAlarmRow(const AlarmRule &rule);      // 往报警规则表加一行
    QVector<AlarmRule> collectAlarmRules();       // 读报警规则表 → 规则列表
    void refreshStatsLabel();                     // 刷新状态栏统计（含报警数）

    // 界面由 mainwindow.ui 描述（Designer 可直接编辑），这里只持有生成的界面对象。
    // 所有控件通过 ui->xxx 访问，例如 ui->m_startBtn。
    Ui::MainWindow *ui = nullptr;

    // 不在 .ui 里的少量成员（运行期动态产生的）
    QLabel         *m_statsLabel = nullptr;   // 状态栏右侧的累计统计

    // 数据落地相关
    HistoryDb      *m_db        = nullptr;   // 复用一个连接（放在界面线程）
    QString         m_dbDir;                 // 当前数据库目录（来自配置文件）

    // 通信线程
    QThread      *m_thread = nullptr;
    SerialWorker *m_worker = nullptr;
    bool          m_connected = false;
    bool          m_polling = false;

    // 结果表定位：(从站, 寄存器地址) → 结果表行号
    // 用 rowKey() 合成一个 int 当键，避免 QPair 做 QHash 键的哈希问题
    QHash<int, int> m_rowIndex;

    // 报警
    AlarmEngine m_alarm;             // 判定引擎（有状态：靠它实现回差）
    int m_statOk = 0;                // 状态栏用：最近一次的统计
    int m_statFail = 0;
    int m_statRetry = 0;
    int m_alarmRaised = 0;           // 本次轮询累计产生的报警次数
};

#endif // MAINWINDOW_H
