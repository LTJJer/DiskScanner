#include "ResultsFormatter.h"

#include <QTextStream>
#include <QStringConverter>
#include <QDir>
#include <algorithm>

QString formatSize(qint64 bytes)
{
    const double mb = bytes / (1024.0 * 1024.0);
    return QString::number(mb, 'f', 2) + QStringLiteral(" MB");
}

QString formatResultsText(const QVector<ScanItem>& results,
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
    out << QStringLiteral("时间范围：") << QString::number(p.hours, 'f', 2) << QStringLiteral(" 小时\n");
    out << QStringLiteral("文件夹大小阈值：") << QString::number(p.folderMb, 'f', 2) << QStringLiteral(" MB\n");
    out << QStringLiteral("文件大小阈值：") << QString::number(p.fileMb, 'f', 2) << QStringLiteral(" MB\n");
    out << QStringLiteral("共扫描条目：") << scanned << "\n";
    out << QStringLiteral("耗时：") << QString::number(elapsedMs / 1000.0, 'f', 2) << QStringLiteral(" 秒\n");
    out << QStringLiteral("匹配结果：") << results.size()
        << QStringLiteral(" 项（目录 ") << dirCount
        << QStringLiteral("，文件 ") << fileCount << QStringLiteral("）\n");
    out << QStringLiteral("========================================\n\n");

    for (const ScanItem& s : std::as_const(results)) {
        out << (s.isDir ? QStringLiteral("[目录] ") : QStringLiteral("[文件] "))
            << QDir::toNativeSeparators(s.path) << "\n";
        out << QStringLiteral("    大小：") << formatSize(s.size)
            << QStringLiteral("  (") << s.size << QStringLiteral(" bytes)\n");
        out << QStringLiteral("    修改时间：")
            << (s.mtime.isValid() ? s.mtime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QStringLiteral("-")) << "\n";
        out << QStringLiteral("    创建时间：")
            << (s.ctime.isValid() ? s.ctime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QStringLiteral("-")) << "\n\n";
    }
    out.flush();
    return text;
}
