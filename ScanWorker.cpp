#include "ScanWorker.h"

#include <algorithm>
#include <cwctype>      // std::iswalpha
#include <cstring>      // std::strlen

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fileapi.h>
#include <malloc.h>      // _heapmin
#include <winioctl.h>    // IOCTL_STORAGE_QUERY_PROPERTY, STORAGE_PROPERTY_QUERY
#include <ntddstor.h>    // STORAGE_BUS_TYPE, BusTypeNvme 等
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

#ifdef Q_OS_WIN
// ── 驱动器类型检测 ──────────────────────────────────────────────────
// 实现思路：
//   1. 从原生路径提取盘符根（"D:\Folder" -> "D:\"），UNC 路径直接判为 Network
//   2. GetDriveTypeW 区分 Removable / Remote / CDROM / Fixed
//   3. 对 DRIVE_FIXED，打开物理设备 "\\.\D:"，下发 IOCTL_STORAGE_QUERY_PROPERTY
//      查询 StorageDeviceProperty，依据 STORAGE_DEVICE_DESCRIPTOR::BusType 判定：
//        - Nvme / Usb / Mmc / Sd / Ufs / SCM -> 闪存，按 SSD 策略
//        - Sata / Ata / Atapi / Scsi / Sas / RAID / Fibre -> 可能 SSD 也可能 HDD，
//          在 VendorId/ProductId/SerialNumber 等 ASCII 串中搜 "SSD" 关键字，
//          命中判 SSD，否则保守按 HDD（避免磁头颠簸）
//        - 其他总线 -> HDD
//   4. 任何步骤失败 -> Unknown（保守 2 线程）

// 从完整原生路径提取驱动器根，如 "D:\Folder\Sub" -> "D:\"
// 不以 "<letter>:\" 开头（如 UNC \\server\share）时返回空串
static std::wstring extractDriveRoot(const std::wstring& nativePath)
{
    if (nativePath.size() >= 3 &&
        std::iswalpha(static_cast<wint_t>(nativePath[0])) &&
        nativePath[1] == L':' &&
        nativePath[2] == L'\\') {
        return nativePath.substr(0, 3);  // "D:\"
    }
    return std::wstring();
}

// 查询驱动器已用空间（总空间 - 空闲空间），用于整盘扫描进度计算
// driveRoot 格式如 "D:\"，返回 0 表示查询失败
static qint64 queryDriveUsedBytes(const std::wstring& driveRoot)
{
    ULARGE_INTEGER freeBytesAvailable, totalBytes, totalFreeBytes;
    if (GetDiskFreeSpaceExW(driveRoot.c_str(), &freeBytesAvailable, &totalBytes, &totalFreeBytes)) {
        const qint64 total = qint64(totalBytes.QuadPart);
        const qint64 free = qint64(totalFreeBytes.QuadPart);
        const qint64 used = total - free;
        return used > 0 ? used : 0;
    }
    return 0;
}

// 在 STORAGE_DEVICE_DESCRIPTOR 跟随的变长 ASCII 字符串区域中搜索 needle（不区分大小写）
// desc 指向整个 buffer 起始，bufferSize 为 buffer 字节数
// STORAGE_DEVICE_DESCRIPTOR 中各 *Offset 字段为相对 buffer 起始的字节偏移，0 表示不存在
static bool descriptorStringContains(const STORAGE_DEVICE_DESCRIPTOR* desc,
                                     const void* buffer, size_t bufferSize,
                                     const char* needleAscii)
{
    const char* base = static_cast<const char*>(buffer);
    const DWORD offsets[] = {
        desc->VendorIdOffset,
        desc->ProductIdOffset,
        desc->ProductRevisionOffset,
        desc->SerialNumberOffset
    };
    const size_t needleLen = std::strlen(needleAscii);
    if (needleLen == 0) return false;

    for (DWORD off : offsets) {
        if (off == 0 || size_t(off) >= bufferSize) continue;
        const char* s = base + off;
        const size_t maxLen = bufferSize - off;
        size_t slen = 0;
        while (slen < maxLen && s[slen] != '\0') ++slen;
        if (slen < needleLen) continue;
        for (size_t i = 0; i + needleLen <= slen; ++i) {
            bool match = true;
            for (size_t j = 0; j < needleLen; ++j) {
                char a = s[i + j];
                char b = needleAscii[j];
                if (a >= 'a' && a <= 'z') a -= char('a' - 'A');
                if (b >= 'a' && b <= 'z') b -= char('a' - 'A');
                if (a != b) { match = false; break; }
            }
            if (match) return true;
        }
    }
    return false;
}

DriveKind ScanWorker::detectDriveKindForPath(const std::wstring& nativePath)
{
    const std::wstring driveRoot = extractDriveRoot(nativePath);
    if (driveRoot.empty()) {
        // UNC \\server\share 或无盘符路径，按网络盘处理
        return DriveKind::Network;
    }

    const UINT dt = GetDriveTypeW(driveRoot.c_str());
    switch (dt) {
        case DRIVE_REMOVABLE: return DriveKind::Removable;
        case DRIVE_REMOTE:    return DriveKind::Network;
        case DRIVE_CDROM:
        case DRIVE_RAMDISK:
            return DriveKind::Unknown;
        case DRIVE_FIXED: {
            // 对固定盘进一步用 IOCTL_STORAGE_QUERY_PROPERTY 查询总线类型
            // 打开 "\\.\D:"（物理设备路径，仅需 FILE_READ_ATTRIBUTES 权限，无需管理员）
            const std::wstring devPath = L"\\\\.\\" + driveRoot.substr(0, 2);
            HANDLE hDev = CreateFileW(devPath.c_str(),
                                       FILE_READ_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, 0, nullptr);
            if (hDev == INVALID_HANDLE_VALUE) {
                return DriveKind::Unknown;
            }
            STORAGE_PROPERTY_QUERY query = {};
            query.PropertyId = StorageDeviceProperty;
            query.QueryType = PropertyStandardQuery;

            // 缓冲区需容纳 STORAGE_DEVICE_DESCRIPTOR + 变长 ASCII 字符串
            unsigned char buffer[4096] = {};
            DWORD bytesReturned = 0;
            BOOL ok = DeviceIoControl(hDev,
                                       IOCTL_STORAGE_QUERY_PROPERTY,
                                       &query, sizeof(query),
                                       buffer, sizeof(buffer),
                                       &bytesReturned, nullptr);
            CloseHandle(hDev);
            if (!ok || bytesReturned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
                return DriveKind::Unknown;
            }

            const STORAGE_DEVICE_DESCRIPTOR* desc =
                reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer);

            switch (desc->BusType) {
                case BusTypeNvme:
                case BusTypeUsb:
                case BusTypeMmc:
                case BusTypeSd:
                case BusTypeUfs:
                case BusTypeSCM:
                    // 闪存类介质，按 SSD 策略（高并发）
                    return DriveKind::SSD;
                case BusTypeSata:
                case BusTypeAta:
                case BusTypeAtapi:
                case BusTypeScsi:
                case BusTypeSas:
                case BusTypeRAID:
                case BusTypeFibre:
                    // SATA/SCSI/SAS 既可能是 SSD 也可能是 HDD
                    // 在设备描述 ASCII 串中搜 "SSD" 关键字
                    if (descriptorStringContains(desc, buffer, sizeof(buffer), "SSD")) {
                        return DriveKind::SSD;
                    }
                    // 未命中 SSD 关键字，保守按 HDD 处理
                    return DriveKind::HDD;
                default:
                    // 其他总线类型，保守按 HDD
                    return DriveKind::HDD;
            }
        }
        default:
            return DriveKind::Unknown;
    }
}
#else
// 非 Windows 平台暂不实现驱动器类型检测
DriveKind ScanWorker::detectDriveKindForPath(const std::wstring&)
{
    return DriveKind::Unknown;
}
#endif

ScanWorker::ScanWorker(const ScanParams& params, QObject* parent)
    : QObject(parent), m_params(params)
{
    m_folderBytesThreshold = m_params.folderBytesThreshold;
    m_fileBytesThreshold   = m_params.fileBytesThreshold;
    // 截止时刻：timeRangeMs==0 表示不限时间，cutoff 置 0 使所有有效时间戳都满足 >= cutoff
    // （timeMatches 中 mtimeMs > 0 && mtimeMs >= 0 对所有有效时间戳恒真）
    if (m_params.timeRangeMs > 0) {
        const QDateTime cutoff = QDateTime::currentDateTime()
            .addMSecs(-m_params.timeRangeMs);
        m_cutoffMs = cutoff.toMSecsSinceEpoch();
    } else {
        m_cutoffMs = 0;
    }
    // 线程数决策：
    //   用户指定 >0：按用户值（1~32），不进行驱动器类型检测
    //   用户指定 0（自动）：先检测扫描根所在驱动器类型，再据此决定：
    //     - SSD       : min(16, idealThreadCount)，沿用原逻辑
    //     - HDD       : clamp(2, 4, ideal/2)，避免磁头颠簸
    //     - Removable : 2（U盘/SD 卡等闪存介质，IO 缓慢）
    //     - Network   : 2（受限于带宽与延迟，并发收益小）
    //     - Unknown   : 2（保守）
    if (m_params.threadCount > 0) {
        m_threadCount = std::max(1, std::min(32, m_params.threadCount));
        // 用户显式指定时不进行驱动器类型检测，m_driveKind 保持 Unknown
    } else {
        const std::wstring rootNative = QDir::toNativeSeparators(m_params.root).toStdWString();
        m_driveKind = detectDriveKindForPath(rootNative);
        const int ideal = QThread::idealThreadCount();
        switch (m_driveKind) {
            case DriveKind::SSD:
                m_threadCount = std::max(1, std::min(16, ideal));
                break;
            case DriveKind::HDD:
                // 机械盘：2~4 线程；多核机器取 ideal/2，但不超过 4，不少于 2
                m_threadCount = std::max(2, std::min(4, ideal / 2));
                if (m_threadCount < 2) m_threadCount = 2;
                break;
            case DriveKind::Removable:
            case DriveKind::Network:
            case DriveKind::Unknown:
            default:
                m_threadCount = 2;
                break;
        }
    }
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
    m_fileParents.clear();
    m_folderDirEntryToIdx.clear();
    m_treeIndex = TreeIndex{};

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

    // 整盘扫描检测：若扫描根为驱动器根（如 "D:\"），查询磁盘已用空间用于进度计算
    // 非驱动器根扫描时 m_driveUsedBytes 保持 0，进度条显示忙碌指示
#ifdef Q_OS_WIN
    {
        const std::wstring driveRoot = extractDriveRoot(rootNative);
        if (!driveRoot.empty() && driveRoot == rootNative) {
            m_driveUsedBytes = queryDriveUsedBytes(driveRoot);
        }
    }
#endif

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
        m_fileParents.reserve(totalLocalResults);

        for (auto& ctx : m_workerCtxs) {
            // 合并节点所有权
            for (auto& node : ctx->localNodes) {
                m_allNodes.push_back(std::move(node));
            }
            ctx->localNodes.clear();

            // 合并文件匹配结果与对应的父 DirEntry*
            // 两者必须保持下标一一对应，供 buildTreeIndex 计算文件所属文件夹
            for (auto& item : ctx->localResults) {
                m_results.push_back(std::move(item));
            }
            for (DirEntry* p : ctx->localFileParents) {
                m_fileParents.push_back(p);
            }
            ctx->localResults.clear();
            ctx->localFileParents.clear();
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

    // 构建树索引（仍需 DirEntry 树：通过 m_fileParents 与 m_folderDirEntryToIdx
    // 沿父链向上查找最近的结果中祖先文件夹）
    // 内部按 size 降序排序每一层，替代原先对 m_results 的全局排序
    buildTreeIndex();

    // 聚合完成且树索引已构建，DirEntry 节点不再被需要，立即释放以降低峰值内存
    // 使用 swap 而非 clear+shrink_to_fit：强制释放底层内存归还给 OS
    // （shrink_to_fit 是非强制请求，部分实现不会立即归还）
    std::vector<std::unique_ptr<DirEntry>>().swap(m_allNodes);
    m_rootEntry = nullptr;
    std::vector<DirEntry*>().swap(m_fileParents);
    m_folderDirEntryToIdx.clear();
    m_folderDirEntryToIdx = std::unordered_map<DirEntry*, int>{};

    // 在工作线程中完成"重活"，避免主线程 onFinished 阻塞导致 UI 未响应：
    //   shrink_to_fit 紧缩容量（reserve 可能 over-allocate，紧缩后 move 给 model 无需 realloc）
    // 这样主线程 onFinished 只需 O(1) move + modelReset，UI 瞬间刷新
    m_results.shrink_to_fit();

#ifdef Q_OS_WIN
    // 关键：swap 只是让 CRT 把节点内存标记为 free，但不会立即归还给 OS。
    // 任务管理器看到的"内存"是进程工作集（Working Set），其中包含已 free 但未归还的页。
    // _heapmin 内部调用 HeapCompact，强制 CRT 把 free list 中整页归还给 OS，
    // 立即降低进程工作集。这是"536MB 不动"问题的直接修复。
    // 1.2M 个 DirEntry 节点释放后的 free list 约 200-300MB，全部归还后内存可降 60%+
    _heapmin();
#endif

    emit finished();
}

// 构建树索引：基于 m_fileParents（文件段）与 m_folderDirEntryToIdx（文件夹段）
// 计算每项的父项下标；对于不在结果中的直接父文件夹，沿 DirEntry->parent 链向上
// 查找最近的结果中祖先文件夹；找不到则视为根级。
// 完成后按 size 降序排序每一层（替代原先对 m_results 的全局排序）。
void ScanWorker::buildTreeIndex()
{
    const int n = int(m_results.size());
    m_treeIndex = TreeIndex{};
    if (n == 0) return;

    const int fileCount = int(m_fileParents.size());  // m_results 前 fileCount 项为文件

    m_treeIndex.parents.assign(n, -1);
    m_treeIndex.children.assign(n, {});
    m_treeIndex.visibleRowOfItem.assign(n, 0);

    // 沿父链查找最近的结果中祖先文件夹，返回其 m_results 下标，找不到返回 -1
    auto findAncestorInResults = [this](DirEntry* dir) -> int {
        while (dir && dir != m_rootEntry) {
            auto it = m_folderDirEntryToIdx.find(dir);
            if (it != m_folderDirEntryToIdx.end()) return it->second;
            dir = dir->parent;
        }
        return -1;
    };

    // 1) 文件段：父项 = 所属 DirEntry 的最近结果祖先
    for (int i = 0; i < fileCount; ++i) {
        const int parentIdx = findAncestorInResults(m_fileParents[size_t(i)]);
        m_treeIndex.parents[size_t(i)] = parentIdx;
        if (parentIdx >= 0) {
            m_treeIndex.children[size_t(parentIdx)].push_back(i);
        } else {
            m_treeIndex.roots.push_back(i);
        }
    }

    // 2) 文件夹段：父项 = 自身 DirEntry->parent 的最近结果祖先
    //    注意：文件夹被跳过过滤时，其子项的查找会绕过它继续向上（见 findAncestorInResults）
    for (const auto& kv : m_folderDirEntryToIdx) {
        DirEntry* dirEntry = kv.first;
        const int idx = kv.second;
        const int parentIdx = findAncestorInResults(dirEntry->parent);
        m_treeIndex.parents[size_t(idx)] = parentIdx;
        if (parentIdx >= 0) {
            m_treeIndex.children[size_t(parentIdx)].push_back(idx);
        } else {
            m_treeIndex.roots.push_back(idx);
        }
    }

    // 3) 按大小降序排序每一层（根级 + 每个节点的子项）
    auto cmpBySizeDesc = [this](int a, int b) {
        return m_results[size_t(a)].size > m_results[size_t(b)].size;
    };
    if (m_treeIndex.roots.size() > 1) {
        std::sort(m_treeIndex.roots.begin(), m_treeIndex.roots.end(), cmpBySizeDesc);
    }
    for (int i = 0; i < n; ++i) {
        if (m_treeIndex.children[size_t(i)].size() > 1) {
            std::sort(m_treeIndex.children[size_t(i)].begin(),
                      m_treeIndex.children[size_t(i)].end(), cmpBySizeDesc);
        }
    }

    // 4) 构建 visibleRowOfItem：每项在其父项 children（或 roots）中的行号
    //    用于模型 parent() 在 O(1) 内取回父项的行号，避免在百万级 roots 中线性查找
    for (size_t i = 0; i < m_treeIndex.roots.size(); ++i) {
        m_treeIndex.visibleRowOfItem[size_t(m_treeIndex.roots[i])] = int(i);
    }
    for (int i = 0; i < n; ++i) {
        const auto& ch = m_treeIndex.children[size_t(i)];
        for (size_t j = 0; j < ch.size(); ++j) {
            m_treeIndex.visibleRowOfItem[size_t(ch[j])] = int(j);
        }
    }

    m_treeIndex.visibleCount = n;
}

void ScanWorker::cancel()
{
    m_cancel = true;
    m_cv.notify_all();
}

// 修改时间或创建时间在指定时刻之后即视为匹配
// timeRangeMs == 0 表示不限时间，所有项均匹配（含无效时间戳）
bool ScanWorker::timeMatches(qint64 mtimeMs, qint64 ctimeMs) const
{
    if (m_params.timeRangeMs <= 0) return true;
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
            int scanned = 0, dirs = 0, files = 0, matched = 0;
            qint64 bytes = 0;
            for (const auto& ctx : m_workerCtxs) {
                scanned += ctx->scanned;
                dirs += ctx->scannedDirs;
                files += ctx->scannedFiles;
                matched += int(ctx->localResults.size());
                bytes += ctx->scannedBytes;
            }
            emit progress(scanned, dirs, files, matched, nowMs,
                         QString::fromWCharArray(currentPathNative.c_str(),
                                                  int(currentPathNative.size())),
                         bytes, m_driveUsedBytes);
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
    // 大小阈值：0 = 不限大小（包含空文件夹）；>0 = total > threshold
    if (node != m_rootEntry &&
        (m_folderBytesThreshold == 0 || total > m_folderBytesThreshold) &&
        timeMatches(node->mtimeMs, node->ctimeMs)) {
        // Phase 2 单线程，直接追加到 m_results，无需锁
        // 同步记录 DirEntry* -> m_results 下标，供 buildTreeIndex 查找文件夹父项
        m_folderDirEntryToIdx.emplace(node, int(m_results.size()));
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
            ctx.scannedBytes += sz;  // 累计已扫描字节（用于整盘扫描进度）

            // 大小阈值：0 = 不限大小（包含 0 字节文件）；>0 = sz > threshold
            if (m_fileBytesThreshold == 0 || sz > m_fileBytesThreshold) {
                const qint64 mtMs = fileTimeToMs(fd.ftLastWriteTime);
                const qint64 ctMs = fileTimeToMs(fd.ftCreationTime);
                if (timeMatches(mtMs, ctMs)) {
                    const size_t nameLen = std::wcslen(name);
                    const std::wstring childPath = joinPath(node->path, name, nameLen);
                    // 存入线程本地结果缓冲区，无锁；move 避免拷贝
                    // 同时记录父 DirEntry，供后续 buildTreeIndex 计算文件所属文件夹
                    ctx.localResults.push_back(
                        makeScanItem(false, std::move(childPath), sz, mtMs, ctMs));
                    ctx.localFileParents.push_back(node);
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
            ctx.scannedBytes += sz;  // 累计已扫描字节（用于整盘扫描进度）
            if (m_fileBytesThreshold == 0 || sz > m_fileBytesThreshold) {
                const qint64 mtMs = info.lastModified().isValid() ? info.lastModified().toMSecsSinceEpoch() : 0;
                const qint64 ctMs = info.birthTime().isValid() ? info.birthTime().toMSecsSinceEpoch() : 0;
                if (timeMatches(mtMs, ctMs)) {
                    const std::wstring childPath = QDir::toNativeSeparators(info.filePath()).toStdWString();
                    ctx.localResults.push_back(
                        makeScanItem(false, childPath, sz, mtMs, ctMs));
                    ctx.localFileParents.push_back(node);
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
