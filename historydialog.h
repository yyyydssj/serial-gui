#ifndef HISTORYDIALOG_H
#define HISTORYDIALOG_H

// ============================================================
// HistoryDialog —— 历史数据查看对话框
//
// 界面在 historydialog.ui 里（Qt Designer 可直接编辑）；
// 这个类只放逻辑：按条件查询、填表、导出 CSV。
// ============================================================

#include <QDialog>

class HistoryDb;
class QStandardItemModel;

namespace Ui { class HistoryDialog; }

class HistoryDialog : public QDialog
{
    Q_OBJECT

public:
    explicit HistoryDialog(HistoryDb *db, QWidget *parent = nullptr);
    ~HistoryDialog() override;

private slots:
    void reload();          // 按过滤条件查询并刷新表格
    void exportCurrent();   // 导出当前过滤结果为 CSV

private:
    Ui::HistoryDialog *ui = nullptr;
    HistoryDb         *m_db = nullptr;
    QStandardItemModel *m_model = nullptr;
};

#endif // HISTORYDIALOG_H
