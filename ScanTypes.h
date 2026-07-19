#ifndef SCANTYPES_H
#define SCANTYPES_H

#include <QString>
#include <vector>

// 驱动器类型（用于自动决定并发线程数）
//   SSD       —— 固态盘/NVMe/USB 闪存：可高并发（min(16, idealThreadCount)）
//   HDD       —— 机械盘：低并发（2~4 线程），避免磁头颠簸
//   Removable —— 可移动盘（U盘/SD 卡）：2 线程
//   Network   —— 网络盘：2 线程（受限于带宽与延迟，并发收益小）
//   Unknown   —— 未知/检测失败：保守 2 线程
enum class DriveKind {
    SSD,
    HDD,
    Removable,
    Network,
    Unknown
};

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
    int threadCount = 0;             // 并发线程数：0=自动（按 CPU 核心数），>0=用户指定
};

// 扫描结果的树形索引（在 worker 线程构建，移交给模型）
// 索引下标与 m_results / m_allData 的下标一一对应
//   parents[i]          : 第 i 项的父项下标，-1 表示根级
//   children[i]         : 第 i 项的直接子项下标列表（已按 size 降序排序）
//   roots               : 根级项下标列表（已按 size 降序排序）
//   visibleRowOfItem[i] : 第 i 项在其父项 children 中的行号（或根级在 roots 中的行号）
//   visibleCount        : 总项数（= m_results.size()，便于模型预分配）
struct TreeIndex {
    std::vector<int> parents;
    std::vector<std::vector<int>> children;
    std::vector<int> roots;
    std::vector<int> visibleRowOfItem;
    int visibleCount = 0;
};

#endif // SCANTYPES_H
