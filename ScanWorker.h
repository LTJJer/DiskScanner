#ifndef SCANWORKER_H
#define SCANWORKER_H

#include <QObject>
#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QThread>
#include <atomic>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <memory>
#include <unordered_map>

#include "ScanTypes.h"

// 后台扫描工作对象，运行在独立线程中
//
// 性能优化设计：
//   Phase 1（并行枚举）：N 个工作线程从共享队列取目录并行处理
//     - 每个工作线程维护本地结果缓冲区（localResults），避免结果互斥锁
//     - 每个工作线程维护本地节点池（localNodes），避免节点互斥锁
//     - 每个工作线程维护本地计数器（scanned/scannedDirs/scannedFiles），
//       避免每条目原子 RMW 导致的缓存行争用
//     - 批量入队子目录（每个目录处理完毕后一次性入队所有子目录），减少队列锁竞争
//     - 批量获取任务（每次锁获取取多个目录），进一步减少锁争用
//     - 全程使用 std::wstring，避免 QString↔wstring 频繁转换
//     - 时间戳用 qint64 毫秒存储，避免 QDateTime 构造开销
//   Phase 2（顺序聚合）：单线程后序遍历，累加文件夹大小并筛选
//     - 无锁，直接追加到 m_results
class ScanWorker : public QObject {
    Q_OBJECT
public:
    explicit ScanWorker(const ScanParams& params, QObject* parent = nullptr);

    const std::vector<ScanItem>& results() const { return m_results; }
    // 转移结果所有权（避免 onFinished 中拷贝导致双份百万级数据驻留）
    // move 后主动 swap 清空 m_results，确保无残留容量引用
    std::vector<ScanItem> takeResults()
    {
        std::vector<ScanItem> out = std::move(m_results);
        std::vector<ScanItem>().swap(m_results);  // 彻底清空，释放可能残留的容量
        return out;
    }
    // 转移树索引所有权（与 takeResults 配对使用）
    TreeIndex takeTreeIndex()
    {
        TreeIndex out = std::move(m_treeIndex);
        m_treeIndex = TreeIndex{};
        return out;
    }
    int scannedCount() const { return m_scanned; }
    int scannedDirCount() const { return m_scannedDirs; }
    int scannedFileCount() const { return m_scannedFiles; }
    qint64 elapsedMs() const { return m_elapsedMs; }
    const ScanParams& params() const { return m_params; }
    // 实际使用的线程数（自动模式下由驱动器类型决定）
    int threadCount() const { return m_threadCount; }
    // 扫描根路径所在驱动器的类型（用于 UI 显示与自适应决策回溯）
    DriveKind driveKind() const { return m_driveKind; }

public slots:
    void doScan();
    void cancel();

signals:
    void progress(int scanned, int dirCount, int fileCount, int matchedCount,
                  qint64 elapsedMs, const QString& currentPath,
                  qint64 scannedBytes, qint64 totalBytes);
    void finished();
    void failed(const QString& msg);

private:
    ScanParams m_params;
    qint64 m_folderBytesThreshold = 0;
    qint64 m_fileBytesThreshold = 0;
    qint64 m_cutoffMs = 0;  // 截止时刻（Unix 毫秒）

    // 最终结果（Phase 1 文件 + Phase 2 文件夹）
    // Phase 1 期间无锁（各线程写本地缓冲区，结束后合并）
    // Phase 2 期间单线程追加，无需锁
    // 使用 std::vector 而非 QVector：连续存储，reserve 后 push_back 无额外分配，
    // 且 move 语义更高效
    std::vector<ScanItem> m_results;

    // 全局统计（Phase 1 结束后由各线程本地计数器合并，热路径中不触碰）
    int m_scanned = 0;
    int m_scannedDirs = 0;
    int m_scannedFiles = 0;

    // 对齐到缓存行，避免 false sharing
    // m_cancel 在热路径中被多线程读取，需独立缓存行
    alignas(64) std::atomic<bool> m_cancel{false};
    alignas(64) std::atomic<qint64> m_lastProgressMs{0};

    QElapsedTimer m_elapsed;
    qint64 m_elapsedMs = 0;

    int m_threadCount = 1;
    DriveKind m_driveKind = DriveKind::Unknown;  // 由 detectDriveKindForPath 在构造时填充
    qint64 m_driveUsedBytes = 0;  // 整盘扫描时磁盘已用空间（0=非整盘扫描或查询失败，进度条显示忙碌指示）

    // 目录树节点：Phase 1 并行构建，Phase 2 顺序聚合
    struct DirEntry {
        std::wstring path;          // 原生 UTF-16 完整路径
        DirEntry* parent = nullptr;
        qint64 mtimeMs = 0;         // 目录自身修改时间（Unix 毫秒，0=无效）
        qint64 ctimeMs = 0;         // 目录自身创建时间
        qint64 selfFilesSize = 0;   // 该目录直属文件大小之和
        qint64 totalSize = 0;       // 子树总大小（Phase 2 计算）
        std::vector<DirEntry*> children;  // 同时用作批量入队源
    };

    // 每个工作线程的本地上下文（消除锁竞争的核心）
    struct WorkerContext {
        std::vector<std::unique_ptr<DirEntry>> localNodes;   // 本地节点池
        std::vector<ScanItem> localResults;                  // 本地文件匹配结果
        std::vector<DirEntry*> localFileParents;             // 与 localResults 并行：每个文件所属的父 DirEntry
        // 线程本地计数器：消除每条目的原子 RMW 缓存行争用
        int scanned = 0;
        int scannedDirs = 0;
        int scannedFiles = 0;
        qint64 scannedBytes = 0;  // 累计已扫描的文件大小（字节，用于整盘扫描进度）
        WorkerContext() {
            localNodes.reserve(256);
            localResults.reserve(256);
            localFileParents.reserve(256);
        }
    };
    std::vector<std::unique_ptr<WorkerContext>> m_workerCtxs;

    // 节点所有权存储（Phase 1 结束后从各 WorkerContext 合并）
    std::vector<std::unique_ptr<DirEntry>> m_allNodes;

    // 并发工作队列
    std::mutex m_queueMutex;
    std::condition_variable m_cv;
    std::deque<DirEntry*> m_queue;
    std::atomic<int> m_pendingDirs{0};  // 已入队但尚未完成枚举的目录数

    DirEntry* m_rootEntry = nullptr;

    // 树索引构建所需的中途数据（仅 Phase 1~2 期间有效，buildTreeIndex 后清空）
    // m_fileParents: 与 m_results 的文件段（前 fileCount 项）并行的父 DirEntry*
    // m_folderDirEntryToIdx: 文件夹 DirEntry* -> m_results 下标（在 aggregateSizes 中填充）
    std::vector<DirEntry*> m_fileParents;
    std::unordered_map<DirEntry*, int> m_folderDirEntryToIdx;

    // 最终树索引（在 emit finished 前由 buildTreeIndex 填充，takeTreeIndex 取走）
    TreeIndex m_treeIndex;

    bool timeMatches(qint64 mtimeMs, qint64 ctimeMs) const;
    // 检查是否应发射进度（限频），返回 true 时调用方才构造路径并调用 emitProgress
    bool shouldEmitProgress(qint64 nowMs) const;
    void emitProgress(const std::wstring& currentPathNative, qint64 nowMs);
    void workerLoop(int idx);
    void processDir(DirEntry* node, WorkerContext& ctx);
    qint64 aggregateSizes(DirEntry* node);
    DirEntry* createDirEntryLocal(WorkerContext& ctx, const std::wstring& path,
                                  DirEntry* parent, qint64 mtimeMs, qint64 ctimeMs);
    // 构建 m_treeIndex：基于 m_fileParents 与 m_folderDirEntryToIdx 计算父子关系，
    // 并按 size 降序排序每一层（替代原先对 m_results 的全局排序）
    void buildTreeIndex();
    // 检测指定路径所在驱动器的类型（Windows API：GetDriveTypeW + IOCTL_STORAGE_QUERY_PROPERTY）
    static DriveKind detectDriveKindForPath(const std::wstring& nativePath);
};

#endif // SCANWORKER_H
