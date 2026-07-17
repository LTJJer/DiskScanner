#include "ResultsFormatter.h"

#include <QTextStream>
#include <QStringConverter>
#include <QDateTime>
#include <QDir>
#include <algorithm>

// 自适应字节数格式化：
//   < 1024 B        → 整数 B
//   < 1024 KB       → x.xx KB（去尾随 0/小数点）
//   < 1024 MB       → x.xx MB
//   < 1024 GB       → x.xx GB
//   < 1024 TB       → x.xx TB
//   否则            → x.xx PB
// 去尾随 0 后示例：5.00→5, 12.60→12.6, 1.20→1.2, 1.00→1
QString formatSize(qint64 bytes)
{
    // 单位表（base = 1024）：unit[i] 对应 1024^i 字节
    static const char* kUnits[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    static const int kUnitCount = sizeof(kUnits) / sizeof(kUnits[0]);

    if (bytes < 1024)
        return QString::number(bytes) + QStringLiteral(" B");

    // 选最大单位使 value >= 1（但不超出单位表范围）
    double v = double(bytes);
    int idx = 0;
    while (v >= 1024.0 && idx < kUnitCount - 1) {
        v /= 1024.0;
        ++idx;
    }
    // 保留 2 位小数，去除尾随 0 和小数点
    QString s = QString::number(v, 'f', 2);
    while (s.endsWith(QLatin1Char('0')))
        s.chop(1);
    if (s.endsWith(QLatin1Char('.')))
        s.chop(1);
    return s + QStringLiteral(" ") + QString::fromLatin1(kUnits[idx]);
}

// 自适应时间格式化：毫秒 → 毫秒/分钟/小时/天/周/月/年
// 月按 30 天、年按 365 天近似换算
QString formatDuration(qint64 ms)
{
    const qint64 MIN  = 60LL * 1000LL;
    const qint64 HOUR = 60LL * MIN;
    const qint64 DAY  = 24LL * HOUR;
    const qint64 WEEK = 7LL  * DAY;
    const qint64 MON  = 30LL * DAY;
    const qint64 YEAR = 365LL * DAY;

    if (ms < MIN)
        return QString::number(ms) + QStringLiteral(" 毫秒");

    auto trim = [](double v) {
        QString s = QString::number(v, 'f', 2);
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(QLatin1Char('.')))
            s.chop(1);
        return s;
    };

    if (ms < HOUR) return trim(double(ms) / double(MIN))  + QStringLiteral(" 分钟");
    if (ms < DAY)  return trim(double(ms) / double(HOUR)) + QStringLiteral(" 小时");
    if (ms < WEEK) return trim(double(ms) / double(DAY))  + QStringLiteral(" 天");
    if (ms < MON)  return trim(double(ms) / double(WEEK)) + QStringLiteral(" 周");
    if (ms < YEAR) return trim(double(ms) / double(MON))  + QStringLiteral(" 月");
    return trim(double(ms) / double(YEAR)) + QStringLiteral(" 年");
}

QString formatResultsText(const std::vector<ScanItem>& results,
                          const ScanParams& p,
                          int scanned, qint64 elapsedMs)
{
    QString text;
    QTextStream out(&text);
    out.setEncoding(QStringConverter::Utf8);

    const int dirCount = int(std::count_if(results.begin(), results.end(),
                                           [](const ScanItem& s) { return s.isDir; }));
    const int fileCount = int(results.size()) - dirCount;

    out << QStringLiteral("磁盘扫描结果") << "\n";
    out << QStringLiteral("========================================\n");
    out << QStringLiteral("生成时间：") << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << "\n";
    out << QStringLiteral("扫描目录：") << QDir::toNativeSeparators(p.root) << "\n";
    out << QStringLiteral("时间范围：") << formatDuration(p.timeRangeMs) << "\n";
    out << QStringLiteral("文件夹大小阈值：") << formatSize(p.folderBytesThreshold) << "\n";
    out << QStringLiteral("文件大小阈值：") << formatSize(p.fileBytesThreshold) << "\n";
    out << QStringLiteral("共扫描条目：") << scanned << "\n";
    out << QStringLiteral("耗时：") << QString::number(elapsedMs / 1000.0, 'f', 2) << QStringLiteral(" 秒\n");
    out << QStringLiteral("匹配结果：") << int(results.size())
        << QStringLiteral(" 项（目录 ") << dirCount
        << QStringLiteral("，文件 ") << fileCount << QStringLiteral("）\n");
    out << QStringLiteral("========================================\n\n");

    for (const ScanItem& s : results) {
        out << (s.isDir ? QStringLiteral("[目录] ") : QStringLiteral("[文件] "))
            << QDir::toNativeSeparators(s.path) << "\n";
        out << QStringLiteral("    大小：") << formatSize(s.size)
            << QStringLiteral("  (") << s.size << QStringLiteral(" bytes)\n");
        out << QStringLiteral("    修改时间：")
            << (s.mtimeMs > 0 ? QDateTime::fromMSecsSinceEpoch(s.mtimeMs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QStringLiteral("-")) << "\n";
        out << QStringLiteral("    创建时间：")
            << (s.ctimeMs > 0 ? QDateTime::fromMSecsSinceEpoch(s.ctimeMs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QStringLiteral("-")) << "\n\n";
    }
    out.flush();
    return text;
}
