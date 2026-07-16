#include "ScanWorker.h"

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fileapi.h>
#endif

// 路径拼接：若 parent 已以 '\\' 结尾（如驱动器根 "D:\"）则不再追加分隔符，
// 避免出现 "D:\\Folder" 双反斜杠。name 长度由调用方传入，省一次 wcslen。
static inline std::wstring joinPath(const std::wstring& parent,
                                    const wchar_t* name, size_t nameLen)
{
    std::wstring result;
    result.reserve(parent.size() + 1 + nameLen);
    result = parent;
    if (!result.empty() && result.back() != L'\\') {
        result.push_back(L'\\');
    }
    result.append(name, nameLen);
    return result;
}

// 构造 ScanItem（Phase 1 文件匹配与 Phase 2 文件夹匹配共用）
static inline ScanItem makeScanItem(bool isDir, std::wstring nativePath,
                                    qint64 size, qint64 mtimeMs, qint64 ctimeMs)
{
    ScanItem it;
    it.isDir = isDir;
    // 存储原生分隔符路径，消除 replace('\\','/') 的全串扫描开销
    // 显示/保存时路径已是原生格式，QDir::toNativeSeparators 为 no-op
    it.path = QString::fromWCharArray(nativePath.c_str(), int(nativePath.size()));
    it.size = size;
    it.mtimeMs = mtimeMs;
    it.ctimeMs = ctimeMs;
    return it;
}

ScanWorker::ScanWorker(const ScanParams& params, QObject* parent)
    : QObject(parent), m_params(params)
{
    m_folderBytesThreshold = qint64(m_params.folderMb * 1024.0 * 1024.0);
    m_fileBytesThreshold   = qint64(m_params.fileMb   * 1024.0 * 1024.0);
    // 截止时刻 = 当前时间 - 指定小时数（Unix 毫秒）
    const QDateTime cutoff = QDateTime::currentDateTime()
        .addMSecs(-qint64(m_params.hours * 3600.0 * 1000.0));
    m_cutoffMs = cutoff.toMSecsSinceEpoch();
    // 线程数：逻辑核心数，上限 16（I/O 密集型，更多线程收益递减）
    m_threadCount = std::max(1, std::min(16, QThread::idealThreadCount()));
}

void ScanWorker::doScan()
{
    m_scanned = 0;
    m_scannedDirs = 0;
    m_scannedFiles = 0;
    m_elapsed.start();
    m_lastProgressMs = 0;

    // 清理上一次扫描的状态
    m_results.clear();
    m_allNodes.clear();
    m_queue.clear();
    m_pendingDirs = 0;
    m_rootEntry = nullptr;
    m_workerCtxs.clear();

    QDir root(m_params.root);
    if (!root.exists()) {
        emit failed(tr("指定的目录不存在：\n%1").arg(m_params.root));
        emit finished();
        return;
    }
    root.makeAbsolute();
    m_params.root = root.absolutePath();

#ifdef Q_OS_WIN
    // 禁用系统错误弹框（如"磁盘未准备好"），避免阻塞扫描线程
    const UINT oldErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    SetErrorMode(oldErrorMode | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
#endif

    // 预留节点存储容量，减少 vector 扩容
    m_allNodes.reserve(4096);

    // 创建工作线程上下文（每线程一个，消除锁竞争）
    m_workerCtxs.reserve(m_threadCount);
    for (int i = 0; i < m_threadCount; ++i) {
        m_workerCtxs.emplace_back(std::make_unique<WorkerContext>());
    }

    // 创建根节点（存入 worker 0 的本地池）并入队
    // QDir::toNativeSeparators 后驱动器根为 "D:\"（单反斜杠），joinPath 会正确处理
    const std::wstring rootNative = QDir::toNativeSeparators(m_params.root).toStdWString();
    m_rootEntry = createDirEntryLocal(*m_workerCtxs[0], rootNative, nullptr, 0, 0);

    m_pendingDirs.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.push_back(m_rootEntry);
    }
    m_cv.notify_one();

    // Phase 1：启动工作线程并行枚举
    {
        std::vector<std::thread> workers;
        workers.reserve(m_threadCount);
        for (int i = 0; i < m_threadCount; ++i) {
            workers.emplace_back(&ScanWorker::workerLoop, this, i);
        }
        // 等待所有工作线程完成
        for (auto& t : workers) {
            t.join();
        }
    }

    m_elapsedMs = m_elapsed.elapsed();

#ifdef Q_OS_WIN
    SetErrorMode(oldErrorMode);
#endif

    // 合并线程本地数据到全局存储（单线程，无锁）
    int totalScanned = 0;
    int totalDirs = 0;
    int totalFiles = 0;
    {
        // 预估总结果数，减少 m_results 扩容
        size_t totalLocalResults = 0;
        for (const auto& ctx : m_workerCtxs) {
            totalLocalResults += ctx->localResults.size();
            totalScanned += ctx->scanned;
            totalDirs += ctx->scannedDirs;
            totalFiles += ctx->scannedFiles;
        }
        m_results.reserve(totalLocalResults + size_t(totalDirs) + 16);

        for (auto& ctx : m_workerCtxs) {
            // 合并节点所有权
            for (auto& node : ctx->localNodes) {
                m_allNodes.push_back(std::move(node));
            }
            ctx->localNodes.clear();

            // 合并文件匹配结果
            for (auto& item : ctx->localResults) {
                m_results.push_back(std::move(item));
            }
            ctx->localResults.clear();
        }
        m_workerCtxs.clear();
    }

    // 更新全局统计
    m_scanned = totalScanned;
    m_scannedDirs = totalDirs;
    m_scannedFiles = totalFiles;

    // Phase 2：顺序后序遍历，聚合文件夹大小并筛选
    // 即使取消也执行，以输出已扫描部分的部分文件夹结果
    if (m_rootEntry) {
        aggregateSizes(m_rootEntry);
    }

    emit finished();
}

void ScanWorker::cancel()
{
    m_cancel = true;
    m_cv.notify_all();
}

// 修改时间或创建时间在指定小时之内即视为匹配
bool ScanWorker::timeMatches(qint64 mtimeMs, qint64 ctimeMs) const
{
    if (mtimeMs > 0 && mtimeMs >= m_cutoffMs) return true;
    if (ctimeMs > 0 && ctimeMs >= m_cutoffMs) return true;
    return false;
}

// 限频检查：是否应发射进度信号
// 调用方先调用此方法，返回 true 时才构造路径字符串并调用 emitProgress
bool ScanWorker::shouldEmitProgress(qint64 nowMs) const
{
    qint64 last = m_lastProgressMs.load(std::memory_order_relaxed);
    return (nowMs - last) >= 100;
}

// 发射进度信号（调用方已确认 shouldEmitProgress 返回 true）
void ScanWorker::emitProgress(const std::wstring& currentPathNative, qint64 nowMs)
{
    qint64 last = m_lastProgressMs.load(std::memory_order_relaxed);
    if (nowMs - last >= 100) {
        if (m_lastProgressMs.compare_exchange_strong(last, nowMs,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
            // 读取各线程本地计数器之和作为实时进度
            int scanned = 0, dirs = 0, files = 0;
            for (const auto& ctx : m_workerCtxs) {
                scanned += ctx->scanned;
                dirs += ctx->scannedDirs;
                files += ctx->scannedFiles;
            }
            emit progress(scanned, dirs, files, nowMs,
                         QString::fromWCharArray(currentPathNative.c_str(),
                                                  int(currentPathNative.size())));
        }
    }
}

ScanWorker::DirEntry* ScanWorker::createDirEntryLocal(WorkerContext& ctx,
                                                       const std::wstring& path,
                                                       DirEntry* parent,
                                                       qint64 mtimeMs, qint64 ctimeMs)
{
    auto entry = std::make_unique<DirEntry>();
    entry->path = path;
    entry->parent = parent;
    entry->mtimeMs = mtimeMs;
    entry->ctimeMs = ctimeMs;
    DirEntry* raw = entry.get();
    ctx.localNodes.push_back(std::move(entry));  // 无锁，仅当前线程访问
    return raw;
}

// 工作线程主循环：从队列批量取出目录，枚举，批量入队子目录
// 优化：批量获取后用 range-for 顺序处理，避免 vector 头部 erase 的 O(n) 开销
void ScanWorker::workerLoop(int idx)
{
    WorkerContext& ctx = *m_workerCtxs[idx];

    // 本地任务缓冲：一次锁获取取多个目录，减少锁争用
    std::vector<DirEntry*> localTasks;
    localTasks.reserve(8);

    while (true) {
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cv.wait(lock, [this] {
                return !m_queue.empty() || m_pendingDirs == 0 || m_cancel;
            });
            if (m_cancel || m_pendingDirs == 0) return;

            // 批量获取任务（最多 8 个），减少锁获取次数
            localTasks.clear();
            const size_t maxGrab = std::min(size_t(8), m_queue.size());
            for (size_t i = 0; i < maxGrab; ++i) {
                localTasks.push_back(m_queue.front());
                m_queue.pop_front();
            }
        }

        // 顺序处理本批任务，避免 vector 头部 erase
        for (DirEntry* task : localTasks) {
            if (m_cancel.load(std::memory_order_relaxed)) return;

            // 枚举目录，子目录存入 task->children，文件匹配存入 ctx.localResults
            processDir(task, ctx);

            // 批量入队子目录（一次性加锁，减少队列锁竞争）
            if (!task->children.empty() && !m_cancel.load(std::memory_order_relaxed)) {
                const int childCount = int(task->children.size());
                m_pendingDirs.fetch_add(childCount, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(m_queueMutex);
                    for (DirEntry* c : task->children) {
                        m_queue.push_back(c);
                    }
                }
                m_cv.notify_all();
            }

            // 该目录枚举完成
            if (m_pendingDirs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                // 所有目录处理完毕，唤醒其他等待线程
                m_cv.notify_all();
            }
        }
    }
}

// Phase 2：后序遍历计算子树总大小，筛选符合条件的文件夹
qint64 ScanWorker::aggregateSizes(DirEntry* node)
{
    qint64 total = node->selfFilesSize;
    for (DirEntry* child : node->children) {
        total += aggregateSizes(child);
    }
    node->totalSize = total;

    // 根目录不加入结果（与原实现一致）
    if (node != m_rootEntry &&
        total > m_folderBytesThreshold &&
        timeMatches(node->mtimeMs, node->ctimeMs)) {
        // Phase 2 单线程，直接追加到 m_results，无需锁
        m_results.push_back(makeScanItem(true, node->path, total,
                                         node->mtimeMs, node->ctimeMs));
    }

    return total;
}

#ifdef Q_OS_WIN
// ── Windows 原生目录枚举 ──────────────────────────────────────────
// 使用 FindFirstFileExW 而非 Qt 的 QDir::entryInfoList，原因：
//   QDir::entryInfoList 在处理重解析点（junction/symlink）时可能跟随
//   目标路径，当目标为网络位置或不可用路径时会阻塞约 30 秒。
//   原生 API 直接返回 WIN32_FIND_DATAW，不解析重解析点目标，
//   我们通过检查 FILE_ATTRIBUTE_REPARSE_POINT 属性跳过所有重解析点。
//
// 性能优化要点：
//   1. 多线程并发枚举：N 个工作线程从共享队列取目录并行处理
//   2. 线程本地结果/节点缓冲区，消除互斥锁
//   3. 线程本地计数器，消除每条目原子 RMW 的缓存行争用
//   4. 批量入队子目录 + 批量获取任务，减少队列锁竞争
//   5. 全程使用 std::wstring，避免 QString↔wstring 频繁转换
//   6. 时间戳用 qint64 毫秒存储，避免 QDateTime 构造开销
//   7. 跳过所有重解析点后目录图为树（无环），无需 visited 集合
//   8. FindFirstFileExW 使用 FindExInfoBasic + FIND_FIRST_EX_LARGE_FETCH
//   9. 文件夹大小在 Phase 2 顺序聚合，避免并行锁争用
//  10. 进度发射限频：先检查时间窗口再构造字符串，避免无效分配
//  11. 缓存 wcslen 结果，避免重复扫描文件名长度
//  12. joinPath 统一路径拼接，正确处理驱动器根目录尾部分隔符
//  13. workerLoop 用 range-for 处理批量任务，避免 vector 头部 erase O(n)

// FILETIME → Unix 毫秒（0 表示无效）
static inline qint64 fileTimeToMs(const FILETIME& ft)
{
    const ULONGLONG hundredNs = (ULONGLONG(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    if (hundredNs == 0 || hundredNs < 116444736000000000ULL)
        return 0;
    return qint64(hundredNs - 116444736000000000ULL) / 10000LL;
}

void ScanWorker::processDir(DirEntry* node, WorkerContext& ctx)
{
    // 目录开始时的进度发射
    {
        const qint64 nowMs = m_elapsed.elapsed();
        if (shouldEmitProgress(nowMs)) {
            emitProgress(node->path, nowMs);
        }
    }

    // 构造搜索通配符，joinPath 处理驱动器根 "D:\" 不重复加反斜杠
    const std::wstring pattern = joinPath(node->path, L"*", 1);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileExW(
        pattern.c_str(),
        FindExInfoBasic,   // 不获取 8.3 短名，更快
        &fd,
        FindExSearchNameMatch,
        nullptr,
        FIND_FIRST_EX_LARGE_FETCH);  // 大缓冲区，批量读取

    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do {
        if (m_cancel.load(std::memory_order_relaxed)) break;

        // 跳过 "." 和 ".."
        const wchar_t* name = fd.cFileName;
        if (name[0] == L'.' && (name[1] == L'\0' ||
            (name[1] == L'.' && name[2] == L'\0')))
            continue;

        // 跳过所有重解析点（junction、symlink 等），避免跟随网络目标导致阻塞
        // 同时确保目录图为树结构（无环），无需 visited 集合
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;

        ++ctx.scanned;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ++ctx.scannedDirs;
            // 缓存文件名长度，避免重复 wcslen
            const size_t nameLen = std::wcslen(name);
            // 拼接子目录完整路径（joinPath 正确处理驱动器根）
            const std::wstring childPath = joinPath(node->path, name, nameLen);

            const qint64 mtMs = fileTimeToMs(fd.ftLastWriteTime);
            const qint64 ctMs = fileTimeToMs(fd.ftCreationTime);

            // 创建节点存入线程本地池，子目录指针存入 node->children 供批量入队
            DirEntry* child = createDirEntryLocal(ctx, childPath, node, mtMs, ctMs);
            node->children.push_back(child);
        } else {
            ++ctx.scannedFiles;
            const qint64 sz = (qint64(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            node->selfFilesSize += sz;

            // 仅在大小阈值通过时才检查时间并构造路径
            if (sz > m_fileBytesThreshold) {
                const qint64 mtMs = fileTimeToMs(fd.ftLastWriteTime);
                const qint64 ctMs = fileTimeToMs(fd.ftCreationTime);
                if (timeMatches(mtMs, ctMs)) {
                    const size_t nameLen = std::wcslen(name);
                    const std::wstring childPath = joinPath(node->path, name, nameLen);
                    // 存入线程本地结果缓冲区，无锁；move 避免拷贝
                    ctx.localResults.push_back(
                        makeScanItem(false, std::move(childPath), sz, mtMs, ctMs));
                }
            }
        }

        // 每隔 64 项发射一次进度（使用线程本地计数器，无原子开销）
        // 先检查时间窗口，避免无效的字符串构造
        if ((ctx.scanned & 0x3F) == 0) {
            const qint64 nowMs = m_elapsed.elapsed();
            if (shouldEmitProgress(nowMs)) {
                const size_t nameLen = std::wcslen(name);
                const std::wstring curPath = joinPath(node->path, name, nameLen);
                emitProgress(curPath, nowMs);
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

#else
// ── 非 Windows 平台：使用 Qt 的 QDir（仅作 fallback，本项目仅面向 Windows） ──

void ScanWorker::processDir(DirEntry* node, WorkerContext& ctx)
{
    {
        const qint64 nowMs = m_elapsed.elapsed();
        if (shouldEmitProgress(nowMs)) {
            emitProgress(node->path, nowMs);
        }
    }

    const QString dirQ = QString::fromWCharArray(node->path.c_str(),
                                                  int(node->path.size()));
    qint64 selfSize = 0;
    QDir dir(dirQ);
    const QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System | QDir::NoSymLinks,
        QDir::NoSort);

    for (const QFileInfo& info : entries) {
        if (m_cancel.load(std::memory_order_relaxed)) break;
        ++ctx.scanned;

        if (info.isDir()) {
            ++ctx.scannedDirs;
            const std::wstring childPath = QDir::toNativeSeparators(info.filePath()).toStdWString();
            const qint64 mtMs = info.lastModified().isValid() ? info.lastModified().toMSecsSinceEpoch() : 0;
            const qint64 ctMs = info.birthTime().isValid() ? info.birthTime().toMSecsSinceEpoch() : 0;
            DirEntry* child = createDirEntryLocal(ctx, childPath, node, mtMs, ctMs);
            node->children.push_back(child);
        } else if (info.isFile()) {
            ++ctx.scannedFiles;
            const qint64 sz = info.size();
            selfSize += sz;
            if (sz > m_fileBytesThreshold) {
                const qint64 mtMs = info.lastModified().isValid() ? info.lastModified().toMSecsSinceEpoch() : 0;
                const qint64 ctMs = info.birthTime().isValid() ? info.birthTime().toMSecsSinceEpoch() : 0;
                if (timeMatches(mtMs, ctMs)) {
                    const std::wstring childPath = QDir::toNativeSeparators(info.filePath()).toStdWString();
                    ctx.localResults.push_back(
                        makeScanItem(false, childPath, sz, mtMs, ctMs));
                }
            }
        }

        if ((ctx.scanned & 0x3F) == 0) {
            const qint64 nowMs = m_elapsed.elapsed();
            if (shouldEmitProgress(nowMs)) {
                emitProgress(QDir::toNativeSeparators(info.filePath()).toStdWString(), nowMs);
            }
        }
    }
    node->selfFilesSize = selfSize;
}
#endif
