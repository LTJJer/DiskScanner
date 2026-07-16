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
struct ScanParams {
    QString root;
    double hours = 0;
    double folderMb = 0;
    double fileMb = 0;
};

#endif // SCANTYPES_H
