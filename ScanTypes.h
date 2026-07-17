#ifndef SCANTYPES_H
#define SCANTYPES_H

#include <QString>

// 单条扫描结果
// 时间戳用 qint64 毫秒存储，避免扫描热路径中构造 QDateTime 的开销，
// 仅在显示/保存时按需转换为 QDateTime。
struct ScanItem {
    bool isDir = false;
    QString path;       // 原生分隔符路径（Windows 为 '\\'）
    qint64 size = 0;
    qint64 mtimeMs = 0; // 修改时间（Unix 毫秒，0 = 无效）
    qint64 ctimeMs = 0; // 创建时间（Unix 毫秒，0 = 无效）
};

// 扫描参数
// 时间以毫秒、大小以字节存储，GUI/CLI 在入口处将用户输入（含单位）换算为此结构。
// 这样扫描核心只依赖绝对单位，便于多入口复用，且避免浮点阈值在热路径反复换算。
struct ScanParams {
    QString root;
    qint64 timeRangeMs = 0;          // 时间范围（毫秒）
    qint64 folderBytesThreshold = 0; // 文件夹大小阈值（字节）
    qint64 fileBytesThreshold = 0;   // 文件大小阈值（字节）
};

#endif // SCANTYPES_H
