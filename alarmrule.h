#ifndef ALARMRULE_H
#define ALARMRULE_H

// ============================================================
// AlarmRule —— 一条报警规则：对某个 (从站, 寄存器地址) 的值判上下限
//
// 约定：用 hasLow / hasHigh 开关「这一侧要不要检查」——
//   界面上「下限」或「上限」留空的格子，就解析成 hasXxx = false。
//
// 回差（hysteresis）的**确切语义**（边界值必须写死，否则实现和测试会各按各的约定，
// 这次就是单测把「恰好等于 上限−回差 时到底解不解除」这个没说清的点逼了出来）：
//   · 超上限：值 > 上限            → 报警；降到 值 ≤ 上限 − 回差 → 才解除
//   · 超下限：值 < 下限            → 报警；升到 值 ≥ 下限 + 回差 → 才解除
//   · 回差为 0 时退化成「不再越限就立刻解除」，与报警条件严格互补、无缝隙。
//
// 例：上限 100、回差 5 → 101 报警；96 仍报警（在回差带内）；
//     恰好 95（= 100−5）就解除；解除后要重新超过 100 才会再报警。
//
// 没有回差的话，值在阈值附近抖动会反复「报警 / 解除」刷屏，现场噪声下非常常见。
// ============================================================

#include <QString>
#include <QStringList>

struct AlarmRule {
    bool enabled = true;
    int  slave = 1;              // 从站地址
    int  addr = 0;               // 寄存器地址
    bool hasLow = false;         // 是否检查下限
    int  low = 0;
    bool hasHigh = true;         // 是否检查上限
    int  high = 100;
    int  hysteresis = 0;         // 回差（≥0）

    bool isValid() const
    {
        return slave >= 1 && slave <= 247
               && addr >= 0 && addr <= 65535
               && (hasLow || hasHigh)        // 两侧都不查就没意义
               && low <= high
               && hysteresis >= 0;
    }

    QString describe() const
    {
        QStringList parts;
        if (hasLow)  parts << QString("下限 %1").arg(low);
        if (hasHigh) parts << QString("上限 %1").arg(high);
        return QString("从站 %1 地址 %2：%3（回差 %4）")
            .arg(slave).arg(addr)
            .arg(parts.isEmpty() ? QStringLiteral("无") : parts.join(QStringLiteral(" / ")))
            .arg(hysteresis);
    }
};

#endif // ALARMRULE_H
