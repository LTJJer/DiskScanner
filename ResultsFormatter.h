#ifndef RESULTFORMATTER_H
#define RESULTFORMATTER_H

#include "ScanTypes.h"
#include <QString>
#include <vector>

// 字节数格式化为自适应单位字符串：123 B / 5 KB / 12.6 MB / 1.2 GB / 1.1 TB / 1 PB
// 规则：B 显示整数；KB 及以上保留 2 位小数，并去除尾随 0 与小数点。
QString formatSize(qint64 bytes);

// 毫秒数格式化为自适应时间单位字符串：500 毫秒 / 30 分钟 / 24 小时 / 7 天 等
// 单位阶梯：毫秒、分钟、小时、天、周、月、年（月=30 天，年=365 天，近似）
QString formatDuration(qint64 ms);

// 将扫描结果格式化为文本（GUI 保存与 CLI 共用）
QString formatResultsText(const std::vector<ScanItem>& results,
                          const ScanParams& params,
                          int scannedCount, qint64 elapsedMs);

#endif // RESULTFORMATTER_H
