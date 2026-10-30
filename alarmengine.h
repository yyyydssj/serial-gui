#ifndef ALARMENGINE_H
#define ALARMENGINE_H

// ============================================================
// AlarmEngine —— 报警判定引擎
//
// 为什么单独一个类：
//   报警判定是「有状态的逻辑」（要记住当前是否已报警，才能实现回差），
//   把它和界面/数据库分开，就能用单元测试把所有边界情况覆盖掉，
//   而不用靠人点界面去试。alarm_engine_test 就是它的测试。
//
// 它不碰界面、不碰数据库、不碰串口 —— 只吃 (从站, 地址, 值)，吐判定结果。
// ============================================================

#include <QHash>
#include <QString>
#include <QVector>

#include "alarmrule.h"

class AlarmEngine
{
public:
    enum class Level { Normal, High, Low };

    struct Result {
        Level level = Level::Normal;  // 本次判定后的电平
        Level from  = Level::Normal;  // 本次判定前的电平（界面据此记录是超上限还是超下限）
        bool  changed = false;        // 相比上次是否变化
        bool  raised = false;         // 本次产生报警（正常 → 报警）
        bool  cleared = false;        // 本次解除报警（报警 → 正常）
        int   limit = 0;              // 触发/维持/解除所依据的阈值（**解除时也是原阈值，不是 0**）
        bool  hasRule = false;        // 是否命中了规则
    };

    void setRules(const QVector<AlarmRule> &rules);   // 更新规则（会清空存量报警状态，重新判定）
    Result update(int slave, int addr, int value);    // 喂一个采样值
    Level levelOf(int slave, int addr) const;
    int alarmCount() const;                           // 当前处于报警状态的寄存器个数
    void reset();                                     // 清空全部报警状态

    static QString levelText(Level lv);               // 正常 / 超上限 / 超下限

private:
    static int key(int slave, int addr) { return slave * 65536 + addr; }
    const AlarmRule *findRule(int slave, int addr) const;

    QVector<AlarmRule> m_rules;
    QHash<int, Level>  m_state;    // key → 当前电平
    QHash<int, int>    m_limit;    // key → 当前报警依据的阈值
};

#endif // ALARMENGINE_H
