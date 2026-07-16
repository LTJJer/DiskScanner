#ifndef RESULTFORMATTER_H
#define RESULTFORMATTER_H

#include "ScanTypes.h"
#include <QString>
#include <vector>

// 字节数格式化为 MB 字符串
QString formatSize(qint64 bytes);

// 将扫描结果格式化为文本（GUI 保存与 CLI 共用）
QString formatResultsText(const std::vector<ScanItem>& results,
                          const ScanParams& params,
                          int scannedCount, qint64 elapsedMs);

#endif // RESULTFORMATTER_H
