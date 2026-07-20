#ifndef RESULTFORMATTER_H
#define RESULTFORMATTER_H

#include "ScanTypes.h"
#include <QString>
#include <QHash>
#include <vector>

class QIODevice;
class QTextStream;

// 保存格式枚举
enum class SaveFormat {
    Text,       // 默认文本格式（可读性强，含分组与明细）
    Markdown,   // Markdown 表格
    CSV         // CSV 表格（UTF-8 BOM，Excel 友好）
};

// 导出列定义（逻辑索引）
//   0 名称  1 大小  2 修改日期  3 创建日期  4 所在目录  5 类型
// 列 0~5 与结果展示框一一对应，保存时按界面列顺序导出
constexpr int kExportColumnCount = 6;

// 类型缓存：扩展名（带点，如 ".txt"，无扩展名用 "<noext>"）→ 类型描述
// 由 ScanResultsModel 预热，保存时传入避免后台线程重复调用 SHGetFileInfo
using TypeCache = QHash<QString, QString>;

// 字节数格式化为自适应单位字符串：123 B / 5 KB / 12.6 MB / 1.2 GB / 1.1 TB / 1 PB
QString formatSize(qint64 bytes);

// 毫秒数格式化为自适应时间单位字符串
QString formatDuration(qint64 ms);

// 解析文件类型描述（资源管理器风格）
// 若提供 typeCache（按扩展名缓存），优先查缓存；否则调用 SHGetFileInfo
QString resolveFileTypeName(const QString& path, bool isDir,
                            const TypeCache* typeCache = nullptr);

// 从路径中提取文件名
QString extractFileName(const QString& path);

// 从路径中提取所在目录
QString extractParentDir(const QString& path);

// 获取列名（逻辑索引 → 列标题）
QString exportColumnName(int col);

// 获取某项在某列的值（逻辑索引 → 字符串值）
// typeCache 可选，用于复用模型预热的类型缓存
QString exportColumnValue(int col, const ScanItem* s,
                          const TypeCache* typeCache = nullptr);

// 默认列顺序：{0,1,2,3,4,5}
const std::vector<int>& defaultColumnOrder();

// ====== 流式写入接口（后台保存使用，避免大 QString 内存开销）======
// 直接写入 QIODevice，适合大结果集
void writeResultsText(QTextStream& out,
                      const std::vector<const ScanItem*>& results,
                      const ScanParams& params,
                      int scannedCount, qint64 elapsedMs,
                      const std::vector<int>& columnOrder,
                      const TypeCache* typeCache = nullptr);
void writeResultsMarkdown(QTextStream& out,
                          const std::vector<const ScanItem*>& results,
                          const ScanParams& params,
                          int scannedCount, qint64 elapsedMs,
                          const std::vector<int>& columnOrder,
                          const TypeCache* typeCache = nullptr);
void writeResultsCSV(QTextStream& out,
                     const std::vector<const ScanItem*>& results,
                     const ScanParams& params,
                     int scannedCount, qint64 elapsedMs,
                     const std::vector<int>& columnOrder,
                     const TypeCache* typeCache = nullptr);
// 根据格式分派到对应写入函数；返回 true 表示成功
bool writeResultsTo(QIODevice* device, SaveFormat fmt,
                    const std::vector<const ScanItem*>& results,
                    const ScanParams& params,
                    int scannedCount, qint64 elapsedMs,
                    const std::vector<int>& columnOrder,
                    const TypeCache* typeCache = nullptr);

// ====== QString 返回接口（CLI 与兼容代码使用）======
QString formatResultsText(const std::vector<const ScanItem*>& results,
                          const ScanParams& params,
                          int scannedCount, qint64 elapsedMs,
                          const std::vector<int>& columnOrder = defaultColumnOrder());

QString formatResultsMarkdown(const std::vector<const ScanItem*>& results,
                              const ScanParams& params,
                              int scannedCount, qint64 elapsedMs,
                              const std::vector<int>& columnOrder = defaultColumnOrder());

QString formatResultsCSV(const std::vector<const ScanItem*>& results,
                         const ScanParams& params,
                         int scannedCount, qint64 elapsedMs,
                         const std::vector<int>& columnOrder = defaultColumnOrder());

QString formatResults(SaveFormat fmt,
                      const std::vector<const ScanItem*>& results,
                      const ScanParams& params,
                      int scannedCount, qint64 elapsedMs,
                      const std::vector<int>& columnOrder = defaultColumnOrder());

// 旧接口兼容（CLI 使用）：接受 ScanItem 引用数组，内部转换为指针数组
// 使用默认列顺序
QString formatResultsText(const std::vector<ScanItem>& results,
                          const ScanParams& params,
                          int scannedCount, qint64 elapsedMs);

#endif // RESULTFORMATTER_H
