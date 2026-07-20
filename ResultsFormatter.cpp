#include "ResultsFormatter.h"

#include <QTextStream>
#include <QStringConverter>
#include <QDateTime>
#include <QDir>
#include <QIODevice>
#include <algorithm>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

// 与结果展示框一致的日期时间格式（ScanResultsModel::data 中使用）
// 用于保存结果时保持内容与界面一致
static const QString kDateTimeFormat = QStringLiteral("yyyy/MM/dd  HH:mm:ss");

// 自适应字节数格式化
QString formatSize(qint64 bytes)
{
    static const char* kUnits[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    static const int kUnitCount = sizeof(kUnits) / sizeof(kUnits[0]);

    if (bytes < 1024)
        return QString::number(bytes) + QStringLiteral(" B");

    double v = double(bytes);
    int idx = 0;
    while (v >= 1024.0 && idx < kUnitCount - 1) {
        v /= 1024.0;
        ++idx;
    }
    QString s = QString::number(v, 'f', 2);
    while (s.endsWith(QLatin1Char('0')))
        s.chop(1);
    if (s.endsWith(QLatin1Char('.')))
        s.chop(1);
    return s + QStringLiteral(" ") + QString::fromLatin1(kUnits[idx]);
}

// 自适应时间格式化
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

QString extractFileName(const QString& path)
{
    int lastSep = path.lastIndexOf(QLatin1Char('\\'));
    if (lastSep < 0) lastSep = path.lastIndexOf(QLatin1Char('/'));
    if (lastSep < 0) return path;
    return path.mid(lastSep + 1);
}

QString extractParentDir(const QString& path)
{
    int lastSep = path.lastIndexOf(QLatin1Char('\\'));
    if (lastSep < 0) lastSep = path.lastIndexOf(QLatin1Char('/'));
    if (lastSep <= 0) return QStringLiteral("-");
    return QDir::toNativeSeparators(path.left(lastSep));
}

// 从路径提取扩展名（小写，带点；无扩展名返回 "<noext>"）
// 与 ScanResultsModel::resolveFileInfo 中的键生成方式保持一致
static QString extensionKey(const QString& path)
{
    const int lastDot = path.lastIndexOf(QLatin1Char('.'));
    const int lastSep = std::max<int>(path.lastIndexOf(QLatin1Char('\\')),
                                      path.lastIndexOf(QLatin1Char('/')));
    if (lastDot >= 0 && lastDot > lastSep) {
        return QStringLiteral(".") + path.mid(lastDot + 1).toLower();
    }
    return QStringLiteral("<noext>");
}

// 解析文件类型描述（资源管理器风格）
// 若提供 typeCache，优先查缓存；否则调用 SHGetFileInfo
QString resolveFileTypeName(const QString& path, bool isDir, const TypeCache* typeCache)
{
    if (isDir) return QStringLiteral("文件夹");

    // 优先使用传入的缓存（来自模型的预热缓存）
    if (typeCache) {
        const QString key = extensionKey(path);
        auto it = typeCache->constFind(key);
        if (it != typeCache->constEnd()) return it.value();
    }

    const QString ext = [&]() -> QString {
        const int lastDot = path.lastIndexOf(QLatin1Char('.'));
        const int lastSep = std::max<int>(path.lastIndexOf(QLatin1Char('\\')),
                                          path.lastIndexOf(QLatin1Char('/')));
        return (lastDot >= 0 && lastDot > lastSep)
            ? path.mid(lastDot + 1).toLower()
            : QString();
    }();

#ifdef Q_OS_WIN
    SHFILEINFOW sfi = {};
    const QString nativePath = QDir::toNativeSeparators(path);
    const DWORD flags = SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES;
    if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
                       FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), flags)) {
        if (sfi.szTypeName[0]) {
            return QString::fromWCharArray(sfi.szTypeName);
        }
    }
#endif
    return ext.isEmpty() ? QStringLiteral("文件") : (ext + QStringLiteral(" 文件"));
}

// 默认列顺序：{0,1,2,3,4,5}
const std::vector<int>& defaultColumnOrder()
{
    static const std::vector<int> kDefault = {0, 1, 2, 3, 4, 5};
    return kDefault;
}

// 获取列名（逻辑索引 → 列标题）
QString exportColumnName(int col)
{
    switch (col) {
    case 0: return QStringLiteral("名称");
    case 1: return QStringLiteral("大小");
    case 2: return QStringLiteral("修改日期");
    case 3: return QStringLiteral("创建日期");
    case 4: return QStringLiteral("所在目录");
    case 5: return QStringLiteral("类型");
    default: return QString();
    }
}

// 获取某项在某列的值（与结果展示框内容一致）
// 日期格式使用 yyyy/MM/dd  HH:mm:ss，与 ScanResultsModel::data 相同
QString exportColumnValue(int col, const ScanItem* s, const TypeCache* typeCache)
{
    switch (col) {
    case 0: return extractFileName(s->path);
    case 1: return formatSize(s->size);
    case 2: return s->mtimeMs > 0
        ? QDateTime::fromMSecsSinceEpoch(s->mtimeMs).toString(kDateTimeFormat)
        : QStringLiteral("-");
    case 3: return s->ctimeMs > 0
        ? QDateTime::fromMSecsSinceEpoch(s->ctimeMs).toString(kDateTimeFormat)
        : QStringLiteral("-");
    case 4: return extractParentDir(s->path);
    case 5: return resolveFileTypeName(s->path, s->isDir, typeCache);
    default: return QString();
    }
}

// ====================================================================
// 流式写入接口（后台保存使用）
// ====================================================================

void writeResultsText(QTextStream& out,
                      const std::vector<const ScanItem*>& results,
                      const ScanParams& p,
                      int scanned, qint64 elapsedMs,
                      const std::vector<int>& columnOrder,
                      const TypeCache* typeCache)
{
    int dirCount = 0, fileCount = 0;
    for (const ScanItem* s : results) {
        if (s->isDir) ++dirCount;
        else ++fileCount;
    }

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

    for (const ScanItem* s : results) {
        out << (s->isDir ? QStringLiteral("[文件夹] ") : QStringLiteral("[文件] "))
            << extractFileName(s->path) << "\n";
        for (int col : columnOrder) {
            if (col == 0) continue;  // 名称已在标题行输出
            out << QStringLiteral("    ") << exportColumnName(col)
                << QStringLiteral("：") << exportColumnValue(col, s, typeCache) << "\n";
        }
        out << "\n";
    }
    out.flush();
}

void writeResultsMarkdown(QTextStream& out,
                          const std::vector<const ScanItem*>& results,
                          const ScanParams& p,
                          int scanned, qint64 elapsedMs,
                          const std::vector<int>& columnOrder,
                          const TypeCache* typeCache)
{
    int dirCount = 0, fileCount = 0;
    for (const ScanItem* s : results) {
        if (s->isDir) ++dirCount;
        else ++fileCount;
    }

    out << QStringLiteral("# 磁盘扫描结果\n\n");
    out << QStringLiteral("- **生成时间**：") << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << "\n";
    out << QStringLiteral("- **扫描目录**：`") << QDir::toNativeSeparators(p.root) << "`\n";
    out << QStringLiteral("- **时间范围**：") << formatDuration(p.timeRangeMs) << "\n";
    out << QStringLiteral("- **文件夹大小阈值**：") << formatSize(p.folderBytesThreshold) << "\n";
    out << QStringLiteral("- **文件大小阈值**：") << formatSize(p.fileBytesThreshold) << "\n";
    out << QStringLiteral("- **共扫描条目**：") << scanned << "\n";
    out << QStringLiteral("- **耗时**：") << QString::number(elapsedMs / 1000.0, 'f', 2) << QStringLiteral(" 秒\n");
    out << QStringLiteral("- **匹配结果**：") << int(results.size())
        << QStringLiteral(" 项（目录 ") << dirCount
        << QStringLiteral("，文件 ") << fileCount << QStringLiteral("）\n\n");

    auto esc = [](const QString& s) {
        QString r = s;
        r.replace(QLatin1Char('|'), QStringLiteral("\\|"));
        r.replace(QLatin1Char('\n'), QLatin1Char(' '));
        r.replace(QLatin1Char('\r'), QLatin1Char(' '));
        return r;
    };

    out << QStringLiteral("| ");
    for (size_t i = 0; i < columnOrder.size(); ++i) {
        if (i > 0) out << QStringLiteral(" | ");
        out << esc(exportColumnName(columnOrder[i]));
    }
    out << QStringLiteral(" |\n");

    out << QStringLiteral("|");
    for (size_t i = 0; i < columnOrder.size(); ++i) {
        out << QStringLiteral("------|");
    }
    out << "\n";

    for (const ScanItem* s : results) {
        out << QStringLiteral("| ");
        for (size_t i = 0; i < columnOrder.size(); ++i) {
            if (i > 0) out << QStringLiteral(" | ");
            out << esc(exportColumnValue(columnOrder[i], s, typeCache));
        }
        out << QStringLiteral(" |\n");
    }
    out.flush();
}

void writeResultsCSV(QTextStream& out,
                     const std::vector<const ScanItem*>& results,
                     const ScanParams& p,
                     int scanned, qint64 elapsedMs,
                     const std::vector<int>& columnOrder,
                     const TypeCache* typeCache)
{
    Q_UNUSED(p); Q_UNUSED(scanned); Q_UNUSED(elapsedMs);

    auto esc = [](const QString& s) {
        if (s.contains(QLatin1Char(',')) ||
            s.contains(QLatin1Char('"')) ||
            s.contains(QLatin1Char('\n')) ||
            s.contains(QLatin1Char('\r'))) {
            QString r = s;
            r.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            return QStringLiteral("\"") + r + QStringLiteral("\"");
        }
        return s;
    };

    for (size_t i = 0; i < columnOrder.size(); ++i) {
        if (i > 0) out << QStringLiteral(",");
        out << esc(exportColumnName(columnOrder[i]));
    }
    out << QStringLiteral("\n");

    for (const ScanItem* s : results) {
        for (size_t i = 0; i < columnOrder.size(); ++i) {
            if (i > 0) out << QStringLiteral(",");
            out << esc(exportColumnValue(columnOrder[i], s, typeCache));
        }
        out << QStringLiteral("\n");
    }
    out.flush();
}

bool writeResultsTo(QIODevice* device, SaveFormat fmt,
                    const std::vector<const ScanItem*>& results,
                    const ScanParams& params,
                    int scannedCount, qint64 elapsedMs,
                    const std::vector<int>& columnOrder,
                    const TypeCache* typeCache)
{
    if (!device) return false;

    // CSV 格式写入 UTF-8 BOM（Excel 友好，正确识别 UTF-8 编码）
    if (fmt == SaveFormat::CSV) {
        const char bom[3] = { static_cast<char>(0xEF), static_cast<char>(0xBB), static_cast<char>(0xBF) };
        if (device->write(bom, 3) != 3) return false;
    }

    QTextStream out(device);
    out.setEncoding(QStringConverter::Utf8);
    out.setGenerateByteOrderMark(false);  // BOM 已手动写入

    switch (fmt) {
    case SaveFormat::Markdown:
        writeResultsMarkdown(out, results, params, scannedCount, elapsedMs, columnOrder, typeCache);
        break;
    case SaveFormat::CSV:
        writeResultsCSV(out, results, params, scannedCount, elapsedMs, columnOrder, typeCache);
        break;
    case SaveFormat::Text:
    default:
        writeResultsText(out, results, params, scannedCount, elapsedMs, columnOrder, typeCache);
        break;
    }
    out.flush();
    return out.status() == QTextStream::Ok;
}

// ====================================================================
// QString 返回接口（CLI 与兼容代码使用）
// ====================================================================

QString formatResultsText(const std::vector<const ScanItem*>& results,
                          const ScanParams& p,
                          int scanned, qint64 elapsedMs,
                          const std::vector<int>& columnOrder)
{
    QString text;
    QTextStream out(&text);
    out.setEncoding(QStringConverter::Utf8);
    writeResultsText(out, results, p, scanned, elapsedMs, columnOrder, nullptr);
    return text;
}

QString formatResultsMarkdown(const std::vector<const ScanItem*>& results,
                              const ScanParams& p,
                              int scanned, qint64 elapsedMs,
                              const std::vector<int>& columnOrder)
{
    QString text;
    QTextStream out(&text);
    out.setEncoding(QStringConverter::Utf8);
    writeResultsMarkdown(out, results, p, scanned, elapsedMs, columnOrder, nullptr);
    return text;
}

QString formatResultsCSV(const std::vector<const ScanItem*>& results,
                         const ScanParams& p,
                         int scanned, qint64 elapsedMs,
                         const std::vector<int>& columnOrder)
{
    QString text;
    QTextStream out(&text);
    out.setEncoding(QStringConverter::Utf8);
    writeResultsCSV(out, results, p, scanned, elapsedMs, columnOrder, nullptr);
    return text;
}

QString formatResults(SaveFormat fmt,
                      const std::vector<const ScanItem*>& results,
                      const ScanParams& params,
                      int scannedCount, qint64 elapsedMs,
                      const std::vector<int>& columnOrder)
{
    switch (fmt) {
    case SaveFormat::Markdown:
        return formatResultsMarkdown(results, params, scannedCount, elapsedMs, columnOrder);
    case SaveFormat::CSV:
        return formatResultsCSV(results, params, scannedCount, elapsedMs, columnOrder);
    case SaveFormat::Text:
    default:
        return formatResultsText(results, params, scannedCount, elapsedMs, columnOrder);
    }
}

// 旧接口兼容（CLI 使用）
QString formatResultsText(const std::vector<ScanItem>& results,
                          const ScanParams& params,
                          int scannedCount, qint64 elapsedMs)
{
    std::vector<const ScanItem*> ptrs;
    ptrs.reserve(results.size());
    for (const ScanItem& s : results) {
        ptrs.push_back(&s);
    }
    return formatResultsText(ptrs, params, scannedCount, elapsedMs, defaultColumnOrder());
}
