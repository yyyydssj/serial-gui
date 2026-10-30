#include "alarmengine.h"

void AlarmEngine::setRules(const QVector<AlarmRule> &rules)
{
    m_rules = rules;
    // 规则变了，存量状态一律作废：让下一次 update 按新规则重新判定。
    // （否则改完阈值后，旧的回差状态会让判定结果看起来"不生效"）
    m_state.clear();
    m_limit.clear();
}

void AlarmEngine::reset()
{
    m_state.clear();
    m_limit.clear();
}

const AlarmRule *AlarmEngine::findRule(int slave, int addr) const
{
    for (const AlarmRule &r : m_rules) {
        if (r.enabled && r.slave == slave && r.addr == addr)
            return &r;
    }
    return nullptr;
}

AlarmEngine::Level AlarmEngine::levelOf(int slave, int addr) const
{
    return m_state.value(key(slave, addr), Level::Normal);
}

int AlarmEngine::alarmCount() const
{
    int n = 0;
    for (auto it = m_state.constBegin(); it != m_state.constEnd(); ++it) {
        if (it.value() != Level::Normal)
            ++n;
    }
    return n;
}

QString AlarmEngine::levelText(Level lv)
{
    switch (lv) {
    case Level::High: return QStringLiteral("超上限");
    case Level::Low:  return QStringLiteral("超下限");
    default:          return QStringLiteral("正常");
    }
}

AlarmEngine::Result AlarmEngine::update(int slave, int addr, int value)
{
    Result res;
    const int k = key(slave, addr);
    const Level prev = m_state.value(k, Level::Normal);
    const int prevLimit = m_limit.value(k, 0);   // 上次报警依据的阈值（解除时要用它）
    const AlarmRule *rule = findRule(slave, addr);

    Level next = Level::Normal;
    int limit = 0;

    if (rule) {
        res.hasRule = true;
        const int hyst = rule->hysteresis;

        if (rule->hasHigh && value > rule->high) {
            // 越上限
            next = Level::High;
            limit = rule->high;
        } else if (rule->hasLow && value < rule->low) {
            // 越下限
            next = Level::Low;
            limit = rule->low;
        } else if (prev == Level::High && rule->hasHigh && value > rule->high - hyst) {
            // 已经因超上限报警，且还在回差带内 → 维持报警
            next = Level::High;
            limit = rule->high;
        } else if (prev == Level::Low && rule->hasLow && value < rule->low + hyst) {
            // 已经因超下限报警，且还在回差带内 → 维持报警
            next = Level::Low;
            limit = rule->low;
        } else {
            next = Level::Normal;
            // 解除时保留「原报警依据的阈值」：否则记录里会出现"解除事件阈值 = 0"，
            // 事后不知道解除的是哪条阈值。
            if (prev == Level::High || prev == Level::Low)
                limit = (prevLimit != 0) ? prevLimit
                                         : (prev == Level::High ? rule->high : rule->low);
        }
    }

    res.level   = next;
    res.from    = prev;
    res.limit   = limit;
    res.changed = (next != prev);
    res.raised  = res.changed && next != Level::Normal;
    res.cleared = res.changed && next == Level::Normal;

    m_state[k] = next;
    if (next == Level::Normal)
        m_limit.remove(k);
    else
        m_limit[k] = limit;

    return res;
}
