#include "ScanResultsModel.h"

#include <QApplication>
#include <QStyle>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include <QtConcurrent>
#include <algorithm>
#include <numeric>
#include <functional>

#include "ResultsFormatter.h"  // extractFileName, TypeCache

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

ScanResultsModel::ScanResultsModel(QObject* parent)
    : QAbstractItemModel(parent)
{
    QStyle* st = QApplication::style();
    if (st) {
        m_dirIcon  = st->standardIcon(QStyle::SP_DirIcon);
        m_fileIcon = st->standardIcon(QStyle::SP_FileIcon);
    }
    m_rootBatchTimer = new QTimer(this);
    m_rootBatchTimer->setSingleShot(true);
    m_rootBatchTimer->setInterval(1);
    connect(m_rootBatchTimer, &QTimer::timeout, this, &ScanResultsModel::insertRootBatch);

    // 后台 prewarm 完成监听：在主线程合并类型名到 m_fileTypeCache
    m_prewarmWatcher = new QFutureWatcher<QHash<QString, QString>>(this);
    connect(m_prewarmWatcher, &QFutureWatcher<QHash<QString, QString>>::finished,
            this, &ScanResultsModel::onPrewarmFinished);
}

ScanResultsModel::~ScanResultsModel()
{
    // 等待后台 prewarm 完成：避免后台线程访问已销毁的对象
    if (m_prewarmWatcher && m_prewarmWatcher->isRunning()) {
        ++m_prewarmGeneration;  // 使结果过期
        m_prewarmWatcher->waitForFinished();
    }
}

// 按扩展名解析文件图标与类型描述，结果按扩展名缓存（每个扩展名仅查询一次系统）
// 类型描述为资源管理器风格：直接返回系统类型名（如 "文本文档"、"应用程序扩展"），
// 不带 .ext 前缀；查询失败时回退为 "文件" 或 "XXX 文件"
//
// 缓存项可能存在的状态：
//   1. {非空 icon, 非空 type}：完整解析（含图标），直接返回
//   2. {空 icon, 非空 type}：仅后台 prewarm 写入了类型名（SHGetFileInfoW），
//      图标在主线程按需懒解析（QFileIconProvider 非线程安全）
//   3. 无缓存项：完整解析（首次访问，未经过 prewarm）
const ScanResultsModel::FileTypeInfo& ScanResultsModel::resolveFileInfo(const QString& path) const
{
    const QFileInfo fi(path);
    const QString ext = fi.suffix().toLower();
    // 缓存键：有扩展名用 ".ext"，无扩展名用 "<noext>"
    const QString key = ext.isEmpty() ? QStringLiteral("<noext>") : (QStringLiteral(".") + ext);

    const auto it = m_fileTypeCache.constFind(key);
    if (it != m_fileTypeCache.constEnd()) {
        // 命中缓存：若图标已存在则直接返回；否则仅懒解析图标
        if (!it.value().icon.isNull()) return it.value();
        // 图标为空：主线程懒解析（QFileIconProvider 非线程安全）
        FileTypeInfo info = it.value();  // 拷贝（type 已存在）
        info.icon = m_iconProvider.icon(fi);
        if (info.icon.isNull()) info.icon = m_fileIcon;  // fallback
        return *m_fileTypeCache.insert(key, info);  // 更新缓存项
    }

    // 无缓存：完整解析类型名（图标后续懒解析或在此处一并解析）
    // 注意：prewarmCacheAsync 已在后台解析大部分扩展名，未命中通常是 prewarm
    // 尚未完成或扩展名在 prewarm 启动后新增（理论上不会，因为 prewarm 启动前
    // m_allData 已稳定）。此处仍做完整解析以保证正确性。
    FileTypeInfo info;
    // 图标：通过 QFileIconProvider 获取系统文件类型图标
    info.icon = m_iconProvider.icon(fi);
    if (info.icon.isNull()) info.icon = m_fileIcon;  // fallback

    // 类型描述：通过 Windows SHGetFileInfo 获取系统类型名（如"文本文档"、"应用程序扩展"、"快捷方式"）
    // SHGFI_USEFILEATTRIBUTES：不要求文件实际存在，仅按扩展名查询
    // 注意：QFileIconProvider::type() 在 Qt 6 中返回 MIME 类型（如 "text/plain"），不符合需求
    QString typeName;
#ifdef Q_OS_WIN
    {
        SHFILEINFOW sfi = {};
        const QString nativePath = QDir::toNativeSeparators(path);
        const DWORD flags = SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES;
        if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
                           FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), flags)) {
            if (sfi.szTypeName[0]) {
                typeName = QString::fromWCharArray(sfi.szTypeName);
            }
        }
    }
#endif
    if (typeName.isEmpty()) {
        // 回退：无法获取系统类型名时，使用扩展名构造描述
        info.type = ext.isEmpty() ? QStringLiteral("文件")
                                  : (ext + QStringLiteral(" 文件"));
    } else {
        info.type = typeName;
    }

    return *m_fileTypeCache.insert(key, info);
}

void ScanResultsModel::setResultsWithTree(std::vector<ScanItem>&& results, TreeIndex&& tree)
{
    if (m_rootBatchTimer) m_rootBatchTimer->stop();
    // 新扫描结果到达：旧的后台 prewarm 结果不再适用，递增代际号使其过期
    // （不强制 cancel，后台线程会自然完成；结果在 onPrewarmFinished 中被丢弃）
    ++m_prewarmGeneration;

    beginResetModel();
    m_allData = std::move(results);
    m_parents = std::move(tree.parents);
    m_children = std::move(tree.children);
    m_roots = std::move(tree.roots);
    m_visibleRowOfItem = std::move(tree.visibleRowOfItem);
    m_loaded.assign(m_allData.size(), 0);

    m_includeTerms.clear();
    m_excludeTerms.clear();
    m_hasFilter = false;
    m_cutoffMs = 0;
    m_folderBytesThreshold = 0;
    m_fileBytesThreshold = 0;
    m_flatMode = false;
    m_filteredIndices.clear();
    // 重置树形过滤状态
    m_origRoots.clear();
    m_origChildren.clear();
    m_origParents.clear();
    m_origVisibleRowOfItem.clear();
    m_treeFiltered = false;

    // 初始状态：无排序（m_sortColumn=-1）→ 树形模式
    // worker 已按 size 降序排序每一层，树形模式直接展示此自然顺序
    m_sortColumn = -1;
    m_sortOrder  = Qt::AscendingOrder;

    m_rootInsertedCount = 0;
    endResetModel();

    // 启动分批插入根级项（endResetModel 之后，rowCount 已为 0）
    if (!m_roots.empty()) {
        startBatchInsertRoots();
    }
}

void ScanResultsModel::clear()
{
    if (m_rootBatchTimer) m_rootBatchTimer->stop();
    // 清空数据：进行中的 prewarm 结果不再适用，递增代际号使其过期
    ++m_prewarmGeneration;
    beginResetModel();
    std::vector<ScanItem>().swap(m_allData);
    std::vector<int>().swap(m_parents);
    std::vector<std::vector<int>>().swap(m_children);
    std::vector<int>().swap(m_roots);
    std::vector<int>().swap(m_visibleRowOfItem);
    std::vector<uint8_t>().swap(m_loaded);
    std::vector<int>().swap(m_filteredIndices);
    std::vector<int>().swap(m_origRoots);
    std::vector<std::vector<int>>().swap(m_origChildren);
    std::vector<int>().swap(m_origParents);
    std::vector<int>().swap(m_origVisibleRowOfItem);
    m_includeTerms.clear();
    m_excludeTerms.clear();
    m_hasFilter = false;
    m_cutoffMs = 0;
    m_folderBytesThreshold = 0;
    m_fileBytesThreshold = 0;
    m_flatMode = false;
    m_treeFiltered = false;
    m_rootInsertedCount = 0;
    m_fileTypeCache.clear();  // 清空文件类型缓存，释放 QIcon 资源
    endResetModel();
}

void ScanResultsModel::setFilter(const QStringList& includeTerms, const QStringList& excludeTerms)
{
    // 若过滤器未实质变化，跳过重建
    if (m_includeTerms == includeTerms && m_excludeTerms == excludeTerms) {
        return;
    }

    if (m_rootBatchTimer && m_rootBatchTimer->isActive()) m_rootBatchTimer->stop();
    m_rootInsertedCount = int(m_roots.size());

    beginResetModel();
    m_includeTerms = includeTerms;
    m_excludeTerms = excludeTerms;
    const bool hasSearch = !includeTerms.isEmpty() || !excludeTerms.isEmpty();
    const bool hasTimeOrSize = (m_cutoffMs > 0) || (m_folderBytesThreshold > 0) || (m_fileBytesThreshold > 0);
    m_hasFilter = hasSearch || hasTimeOrSize;
    // 扁平模式：只有搜索 或 有排序（时间/大小过滤不触发扁平）
    const bool shouldFlat = hasSearch || (m_sortColumn >= 0);
    const bool wasFlat = m_flatMode;
    m_flatMode = shouldFlat;

    if (shouldFlat) {
        if (!wasFlat) {
            // 从树形切到扁平：恢复原始树（确保后续切回树形时数据完整）
            restoreTree();
            m_loaded.assign(m_allData.size(), 1);
        }
        rebuildFilteredIndices();
        if (m_sortColumn >= 0) sortFilteredIndicesImpl();
    } else {
        // 树形模式：应用树形过滤（如果有时间/大小阈值）
        if (hasTimeOrSize) {
            applyTreeFilter();
        } else {
            restoreTree();
        }
        m_loaded.assign(m_allData.size(), 0);
        m_rootInsertedCount = 0;
    }
    endResetModel();

    if (!m_flatMode && !m_roots.empty()) {
        startBatchInsertRoots();
    }
}

void ScanResultsModel::clearFilter()
{
    if (!m_hasFilter && m_includeTerms.isEmpty() && m_excludeTerms.isEmpty()) {
        return;
    }

    const bool hasTimeOrSize = (m_cutoffMs > 0) || (m_folderBytesThreshold > 0) || (m_fileBytesThreshold > 0);
    // 清除搜索词后，模式取决于排序状态（时间/大小不影响扁平）
    const bool shouldFlat = (m_sortColumn >= 0);

    if (!shouldFlat) {
        // 切回树形：恢复原始树，再按时间/大小过滤决定是否应用树形过滤
        if (m_rootBatchTimer && m_rootBatchTimer->isActive()) m_rootBatchTimer->stop();
        beginResetModel();
        m_includeTerms.clear();
        m_excludeTerms.clear();
        m_hasFilter = hasTimeOrSize;
        m_flatMode = false;
        m_filteredIndices.clear();
        restoreTree();
        if (hasTimeOrSize) {
            applyTreeFilter();
        }
        m_loaded.assign(m_allData.size(), 0);
        m_rootInsertedCount = 0;
        endResetModel();
        if (!m_roots.empty()) {
            startBatchInsertRoots();
        }
    } else {
        // 保持扁平：仅清除搜索词，重建索引（仍按时间/大小过滤 + 排序）
        beginResetModel();
        m_includeTerms.clear();
        m_excludeTerms.clear();
        m_hasFilter = hasTimeOrSize;
        rebuildFilteredIndices();
        if (m_sortColumn >= 0) sortFilteredIndicesImpl();
        endResetModel();
    }
}

void ScanResultsModel::removePaths(const QSet<QString>& pathsToRemove)
{
    if (pathsToRemove.isEmpty() || m_allData.empty()) return;

    // 保存过滤器状态以便后续恢复
    const QStringList savedInclude = m_includeTerms;
    const QStringList savedExclude = m_excludeTerms;
    const qint64 savedCutoff = m_cutoffMs;
    const qint64 savedFolderBytes = m_folderBytesThreshold;
    const qint64 savedFileBytes = m_fileBytesThreshold;

    if (m_rootBatchTimer && m_rootBatchTimer->isActive()) m_rootBatchTimer->stop();

    beginResetModel();

    // 过滤 m_allData：移除路径在 pathsToRemove 中的项，以及它们的子项（路径前缀 + 分隔符匹配）
    std::vector<ScanItem> newData;
    newData.reserve(m_allData.size());
    for (const ScanItem& item : m_allData) {
        bool remove = pathsToRemove.contains(item.path);
        if (!remove) {
            for (const QString& rp : pathsToRemove) {
                if (item.path.size() > rp.size() &&
                    item.path.startsWith(rp, Qt::CaseInsensitive) &&
                    (item.path[rp.size()] == QLatin1Char('\\') ||
                     item.path[rp.size()] == QLatin1Char('/'))) {
                    remove = true;
                    break;
                }
            }
        }
        if (!remove) newData.push_back(item);
    }

    if (newData.size() == m_allData.size()) {
        endResetModel();
        return;
    }

    m_allData = std::move(newData);

    // 重置所有状态（含树形过滤状态）
    m_filteredIndices.clear();
    m_flatMode = false;
    m_hasFilter = false;
    m_includeTerms.clear();
    m_excludeTerms.clear();
    m_cutoffMs = 0;
    m_folderBytesThreshold = 0;
    m_fileBytesThreshold = 0;
    m_rootInsertedCount = 0;
    m_parents.clear();
    m_children.clear();
    m_roots.clear();
    m_visibleRowOfItem.clear();
    m_loaded.clear();
    m_origRoots.clear();
    m_origChildren.clear();
    m_origParents.clear();
    m_origVisibleRowOfItem.clear();
    m_treeFiltered = false;

    // 基于路径前缀包含关系重建树索引
    rebuildTreeFromPaths();

    // 恢复过滤器状态
    m_includeTerms = savedInclude;
    m_excludeTerms = savedExclude;
    m_cutoffMs = savedCutoff;
    m_folderBytesThreshold = savedFolderBytes;
    m_fileBytesThreshold = savedFileBytes;

    const bool hasSearch = !savedInclude.isEmpty() || !savedExclude.isEmpty();
    const bool hasTimeOrSize = (savedCutoff > 0) || (savedFolderBytes > 0) || (savedFileBytes > 0);
    m_hasFilter = hasSearch || hasTimeOrSize;

    // 恢复模式：有搜索 或 有排序 → 扁平；否则 → 树形
    const bool shouldFlat = hasSearch || (m_sortColumn >= 0);
    m_flatMode = shouldFlat;

    if (shouldFlat) {
        m_loaded.assign(m_allData.size(), 1);
        m_rootInsertedCount = int(m_roots.size());
        rebuildFilteredIndices();
        if (m_sortColumn >= 0) sortFilteredIndicesImpl();
    } else {
        // 树形模式：按时间/大小过滤决定是否树形过滤
        if (hasTimeOrSize) {
            applyTreeFilter();
        }
        m_loaded.assign(m_allData.size(), 0);
        m_rootInsertedCount = 0;
    }

    endResetModel();

    if (!m_flatMode && !m_roots.empty()) {
        startBatchInsertRoots();
    }
}

void ScanResultsModel::rebuildTreeFromPaths()
{
    const int n = int(m_allData.size());
    if (n == 0) return;

    // 按路径排序，保证父路径在子路径之前
    std::vector<int> order((size_t(n)));  // 额外括号避免 vexing parse
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [this](int a, int b) {
        return m_allData[size_t(a)].path < m_allData[size_t(b)].path;
    });

    m_parents.assign(size_t(n), -1);
    m_children.assign(size_t(n), {});
    m_roots.clear();
    m_roots.reserve(size_t(n));

    // 栈式父项查找：对每个项，弹出栈顶直到栈顶路径是当前路径的前缀（即父项）
    std::vector<int> stack;
    stack.reserve(size_t(n));
    for (int idx : order) {
        const QString& path = m_allData[size_t(idx)].path;
        while (!stack.empty()) {
            const int topIdx = stack.back();
            const QString& topPath = m_allData[size_t(topIdx)].path;
            // topPath 是 path 的父路径：path 以 topPath + 分隔符开头
            // 或 topPath 以分隔符结尾且 path 以 topPath 开头（如驱动器根 "D:\"）
            const bool topEndsWithSep = topPath.endsWith(QLatin1Char('\\')) ||
                                        topPath.endsWith(QLatin1Char('/'));
            if (path.size() > topPath.size() &&
                path.startsWith(topPath, Qt::CaseInsensitive) &&
                (topEndsWithSep ||
                 path[topPath.size()] == QLatin1Char('\\') ||
                 path[topPath.size()] == QLatin1Char('/'))) {
                break;  // topPath 是父路径
            }
            stack.pop_back();
        }
        if (stack.empty()) {
            m_roots.push_back(idx);
        } else {
            const int parentIdx = stack.back();
            m_parents[size_t(idx)] = parentIdx;
            m_children[size_t(parentIdx)].push_back(idx);
        }
        stack.push_back(idx);
    }

    // 按 size 降序排序根级与每层子项（与 worker 默认排序一致）
    auto cmp = [this](int a, int b) {
        return m_allData[size_t(a)].size > m_allData[size_t(b)].size;
    };
    std::sort(m_roots.begin(), m_roots.end(), cmp);
    for (auto& ch : m_children) {
        std::sort(ch.begin(), ch.end(), cmp);
    }

    // 重建 visibleRowOfItem
    m_visibleRowOfItem.assign(size_t(n), 0);
    for (size_t i = 0; i < m_roots.size(); ++i) {
        m_visibleRowOfItem[size_t(m_roots[i])] = int(i);
    }
    for (size_t i = 0; i < m_children.size(); ++i) {
        for (size_t j = 0; j < m_children[i].size(); ++j) {
            m_visibleRowOfItem[size_t(m_children[i][j])] = int(j);
        }
    }

    m_loaded.assign(size_t(n), 0);
    m_rootInsertedCount = 0;
}

const ScanItem* ScanResultsModel::itemForIndex(const QModelIndex& idx) const
{
    if (!idx.isValid()) return nullptr;
    if (m_flatMode) {
        if (idx.row() < 0 || idx.row() >= int(m_filteredIndices.size())) return nullptr;
        return &m_allData[size_t(m_filteredIndices[size_t(idx.row())])];
    }
    const int itemIdx = int(idx.internalId());
    if (itemIdx < 0 || itemIdx >= int(m_allData.size())) return nullptr;
    return &m_allData[size_t(itemIdx)];
}

QModelIndex ScanResultsModel::index(int row, int column, const QModelIndex& parent) const
{
    if (row < 0 || column < 0 || column >= 6) return QModelIndex();

    if (m_flatMode) {
        if (parent.isValid()) return QModelIndex();
        if (row >= int(m_filteredIndices.size())) return QModelIndex();
        return createIndex(row, column, quintptr(0));
    }

    // 树形模式
    if (!parent.isValid()) {
        // 根级：只允许访问已分批插入的行
        if (row >= m_rootInsertedCount) return QModelIndex();
        if (row >= int(m_roots.size())) return QModelIndex();
        const int itemIdx = m_roots[size_t(row)];
        return createIndex(row, column, quintptr(itemIdx));
    }

    const int parentIdx = int(parent.internalId());
    if (parentIdx < 0 || parentIdx >= int(m_allData.size())) return QModelIndex();
    if (!m_loaded[size_t(parentIdx)]) return QModelIndex();
    const auto& ch = m_children[size_t(parentIdx)];
    if (row >= int(ch.size())) return QModelIndex();
    const int itemIdx = ch[size_t(row)];
    return createIndex(row, column, quintptr(itemIdx));
}

QModelIndex ScanResultsModel::parent(const QModelIndex& child) const
{
    if (m_flatMode) return QModelIndex();
    if (!child.isValid()) return QModelIndex();

    const int childIdx = int(child.internalId());
    if (childIdx < 0 || childIdx >= int(m_allData.size())) return QModelIndex();

    const int parentIdx = m_parents[size_t(childIdx)];
    if (parentIdx < 0) return QModelIndex();  // child 是根级，无父

    // 父项在其父项中的行号（或根级行号）已在 buildTreeIndex/sortTreeImpl 中预算
    const int parentRow = m_visibleRowOfItem[size_t(parentIdx)];
    return createIndex(parentRow, 0, quintptr(parentIdx));
}

int ScanResultsModel::rowCount(const QModelIndex& parent) const
{
    if (m_flatMode) {
        if (parent.isValid()) return 0;
        return int(m_filteredIndices.size());
    }

    if (!parent.isValid()) {
        // 根级：返回已分批插入的数量（不是 m_roots.size()）
        return m_rootInsertedCount;
    }
    const int parentIdx = int(parent.internalId());
    if (parentIdx < 0 || parentIdx >= int(m_allData.size())) return 0;
    if (!m_loaded[size_t(parentIdx)]) return 0;  // 未 fetchMore，视图看不见子项
    return int(m_children[size_t(parentIdx)].size());
}

int ScanResultsModel::columnCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    return 6;
}

QVariant ScanResultsModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) return QVariant();

    const ScanItem* s = nullptr;
    if (m_flatMode) {
        if (index.row() < 0 || index.row() >= int(m_filteredIndices.size())) return QVariant();
        s = &m_allData[size_t(m_filteredIndices[size_t(index.row())])];
    } else {
        const int itemIdx = int(index.internalId());
        if (itemIdx < 0 || itemIdx >= int(m_allData.size())) return QVariant();
        s = &m_allData[size_t(itemIdx)];
    }

    switch (role) {
    case Qt::DisplayRole: {
        switch (index.column()) {
        case 0: {  // 名称：取路径末段（文件名 / 目录名）
            // 使用 extractFileName 避免 QFileInfo 构造开销
            // QFileInfo 会解析路径并创建内部对象，在滚动/重绘热路径中累计开销可观
            const QString name = extractFileName(s->path);
            return name.isEmpty() ? QDir::toNativeSeparators(s->path) : name;
        }
        case 1: return formatSize(s->size);  // 大小
        case 2: return s->mtimeMs > 0       // 修改日期
            ? QDateTime::fromMSecsSinceEpoch(s->mtimeMs).toString(QStringLiteral("yyyy/MM/dd  HH:mm:ss"))
            : QStringLiteral("-");
        case 3: return s->ctimeMs > 0       // 创建日期
            ? QDateTime::fromMSecsSinceEpoch(s->ctimeMs).toString(QStringLiteral("yyyy/MM/dd  HH:mm:ss"))
            : QStringLiteral("-");
        case 4: {  // 所在目录：父目录路径（原生分隔符）
            const QString parent = QFileInfo(s->path).path();
            return parent.isEmpty() ? QStringLiteral("-") : QDir::toNativeSeparators(parent);
        }
        case 5: {  // 类型：目录→"文件夹"；文件→系统类型名（如 "文本文档"）
            if (s->isDir) return QStringLiteral("文件夹");
            return resolveFileInfo(s->path).type;
        }
        }
        break;
    }
    case Qt::DecorationRole: {
        // 名称列与类型列均显示项图标
        // 目录→文件夹图标；文件→按扩展名解析的系统类型图标
        if (index.column() == 0 || index.column() == 5) {
            if (s->isDir) return m_dirIcon;
            return resolveFileInfo(s->path).icon;
        }
        break;
    }
    case Qt::TextAlignmentRole: {
        // 大小列右对齐
        if (index.column() == 1) return int(Qt::AlignRight | Qt::AlignVCenter);
        break;
    }
    case Qt::UserRole: {
        // 排序辅助：返回原始数值，避免 DisplayRole 字符串排序错乱
        switch (index.column()) {
        case 1: return s->size;
        case 2: return s->mtimeMs;
        case 3: return s->ctimeMs;
        }
        break;
    }
    }
    return QVariant();
}

QVariant ScanResultsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return QVariant();
    switch (section) {
    case 0: return QStringLiteral("名称");
    case 1: return QStringLiteral("大小");
    case 2: return QStringLiteral("修改日期");
    case 3: return QStringLiteral("创建日期");
    case 4: return QStringLiteral("所在目录");
    case 5: return QStringLiteral("类型");
    }
    return QVariant();
}

Qt::ItemFlags ScanResultsModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

bool ScanResultsModel::hasChildren(const QModelIndex& parent) const
{
    if (m_flatMode) {
        // 扁平模式：只有根级有子项（过滤后的扁平列表）；非根节点无子项
        return !parent.isValid() && !m_filteredIndices.empty();
    }
    if (!parent.isValid()) {
        return m_rootInsertedCount > 0;
    }
    const int parentIdx = int(parent.internalId());
    if (parentIdx < 0 || parentIdx >= int(m_allData.size())) return false;
    // 即使尚未 fetchMore，只要有子项就显示展开箭头
    return !m_children[size_t(parentIdx)].empty();
}

bool ScanResultsModel::canFetchMore(const QModelIndex& parent) const
{
    if (m_flatMode) return false;
    if (!parent.isValid()) return false;  // 根级由定时器分批处理，不通过 fetchMore
    const int parentIdx = int(parent.internalId());
    if (parentIdx < 0 || parentIdx >= int(m_allData.size())) return false;
    return !m_loaded[size_t(parentIdx)] && !m_children[size_t(parentIdx)].empty();
}

void ScanResultsModel::fetchMore(const QModelIndex& parent)
{
    if (m_flatMode) return;
    if (!parent.isValid()) return;
    const int parentIdx = int(parent.internalId());
    if (parentIdx < 0 || parentIdx >= int(m_allData.size())) return;
    if (m_loaded[size_t(parentIdx)]) return;
    const auto& ch = m_children[size_t(parentIdx)];
    if (ch.empty()) return;

    const int count = int(ch.size());
    beginInsertRows(parent, 0, count - 1);
    m_loaded[size_t(parentIdx)] = 1;
    endInsertRows();
}

void ScanResultsModel::startBatchInsertRoots()
{
    const int total = int(m_roots.size());
    const int firstBatch = qMin(BATCH_SIZE, total);
    if (firstBatch > 0) {
        beginInsertRows(QModelIndex(), 0, firstBatch - 1);
        m_rootInsertedCount = firstBatch;
        endInsertRows();
    }
    if (m_rootInsertedCount < total) {
        m_rootBatchTimer->start(1);
    }
}

void ScanResultsModel::insertRootBatch()
{
    const int total = int(m_roots.size());
    const int start = m_rootInsertedCount;
    if (start >= total) return;
    const int end = qMin(start + BATCH_SIZE, total) - 1;
    beginInsertRows(QModelIndex(), start, end);
    m_rootInsertedCount = end + 1;
    endInsertRows();
    if (m_rootInsertedCount < total) {
        m_rootBatchTimer->start(1);
    }
}

void ScanResultsModel::stopBatchInsertRoots()
{
    if (m_rootBatchTimer && m_rootBatchTimer->isActive()) {
        m_rootBatchTimer->stop();
    }
    const int total = int(m_roots.size());
    if (m_rootInsertedCount < total) {
        beginInsertRows(QModelIndex(), m_rootInsertedCount, total - 1);
        m_rootInsertedCount = total;
        endInsertRows();
    }
}

void ScanResultsModel::sort(int column, Qt::SortOrder order)
{
    if (m_allData.empty()) return;

    m_sortColumn = column;
    m_sortOrder  = order;

    // 决定是否应为扁平模式：有排序（column >= 0）或 有搜索
    // 时间/大小过滤不触发扁平
    const bool hasSearch = !m_includeTerms.isEmpty() || !m_excludeTerms.isEmpty();
    const bool shouldFlat = (column >= 0) || hasSearch;

    if (shouldFlat != m_flatMode) {
        // 模式切换：需要完整重置
        if (m_rootBatchTimer && m_rootBatchTimer->isActive()) m_rootBatchTimer->stop();

        beginResetModel();
        m_flatMode = shouldFlat;

        if (m_flatMode) {
            // 切到扁平：恢复原始树（确保后续切回树形时数据完整）
            restoreTree();
            m_rootInsertedCount = int(m_roots.size());
            m_loaded.assign(m_allData.size(), 1);
            rebuildFilteredIndices();
            if (column >= 0) sortFilteredIndicesImpl();
        } else {
            // 切到树形：清空过滤索引，恢复原始树，按时间/大小过滤决定是否树形过滤
            m_filteredIndices.clear();
            restoreTree();
            const bool hasTimeOrSize = (m_cutoffMs > 0) || (m_folderBytesThreshold > 0) || (m_fileBytesThreshold > 0);
            if (hasTimeOrSize) {
                applyTreeFilter();
            }
            m_loaded.assign(m_allData.size(), 0);
            m_rootInsertedCount = 0;
        }
        endResetModel();

        if (!m_flatMode && !m_roots.empty()) {
            startBatchInsertRoots();
        }
    } else {
        // 同模式，仅重排
        if (m_flatMode) {
            if (column >= 0) {
                layoutAboutToBeChanged();
                sortFilteredIndicesImpl();
                layoutChanged();
            } else {
                // 无排序 + 扁平（因搜索）：重建为自然顺序
                layoutAboutToBeChanged();
                rebuildFilteredIndices();
                layoutChanged();
            }
        } else {
            // 树形模式排序
            if (m_sortColumn < 0) return;
            stopBatchInsertRoots();
            layoutAboutToBeChanged();
            sortTreeImpl();
            layoutChanged();
        }
    }
}

void ScanResultsModel::sortTreeImpl()
{
    // 性能优化：按名称排序时预计算所有文件名，避免 O(N log N) 次构造 QFileInfo
    // QFileInfo 构造会解析路径并创建内部对象，在排序热路径中是主要瓶颈
    std::vector<QString> fileNames;
    if (m_sortColumn == 0) {
        fileNames.resize(m_allData.size());
        for (size_t i = 0; i < m_allData.size(); ++i) {
            fileNames[i] = extractFileName(m_allData[i].path);
        }
    }

    auto cmp = [this, &fileNames](int a, int b) -> bool {
        const ScanItem& sa = m_allData[size_t(a)];
        const ScanItem& sb = m_allData[size_t(b)];
        int c = 0;
        switch (m_sortColumn) {
        case 0: c = fileNames[size_t(a)].compare(fileNames[size_t(b)]); break;                     // 名称（预计算）
        case 1: c = (sa.size < sb.size) ? -1 : (sa.size > sb.size) ? 1 : 0; break;                // 大小
        case 2: c = (sa.mtimeMs < sb.mtimeMs) ? -1 : (sa.mtimeMs > sb.mtimeMs) ? 1 : 0; break;    // 修改日期
        case 3: c = (sa.ctimeMs < sb.ctimeMs) ? -1 : (sa.ctimeMs > sb.ctimeMs) ? 1 : 0; break;    // 创建日期
        case 4: c = sa.path.compare(sb.path); break;                                              // 所在目录（用全路径排序）
        case 5: c = (int(sa.isDir) < int(sb.isDir)) ? -1 : (int(sa.isDir) > int(sb.isDir)) ? 1 : 0; break;  // 类型
        default: c = 0; break;
        }
        if (c == 0) return false;
        const bool less = (c < 0);
        return (m_sortOrder == Qt::AscendingOrder) ? less : !less;
    };

    // 排序根级
    if (m_roots.size() > 1) {
        std::sort(m_roots.begin(), m_roots.end(), cmp);
    }
    // 排序每个节点的子项
    for (size_t i = 0; i < m_children.size(); ++i) {
        if (m_children[i].size() > 1) {
            std::sort(m_children[i].begin(), m_children[i].end(), cmp);
        }
    }

    // 重建 visibleRowOfItem（排序后行号已变化）
    std::fill(m_visibleRowOfItem.begin(), m_visibleRowOfItem.end(), 0);
    for (size_t i = 0; i < m_roots.size(); ++i) {
        m_visibleRowOfItem[size_t(m_roots[i])] = int(i);
    }
    for (size_t i = 0; i < m_children.size(); ++i) {
        const auto& ch = m_children[i];
        for (size_t j = 0; j < ch.size(); ++j) {
            m_visibleRowOfItem[size_t(ch[j])] = int(j);
        }
    }
}

void ScanResultsModel::sortFilteredIndicesImpl()
{
    if (m_filteredIndices.empty()) return;

    const int column = m_sortColumn;
    const Qt::SortOrder order = m_sortOrder;

    // 性能优化：按名称排序时预计算所有文件名，避免 O(N log N) 次构造 QFileInfo
    // QFileInfo 构造会解析路径并创建内部对象，在排序热路径中是主要瓶颈
    // 对于 100 万项：QFileInfo 方式约 5-10 秒，预计算方式约 100-200 毫秒
    std::vector<QString> fileNames;
    if (column == 0) {
        fileNames.resize(m_allData.size());
        for (size_t i = 0; i < m_allData.size(); ++i) {
            fileNames[i] = extractFileName(m_allData[i].path);
        }
    }

    // worker 已对 m_allData 按 size 降序预排序（每层），
    // 但扁平模式下 m_filteredIndices 是按 m_allData 下标顺序构建的，
    // 全局 size 降序不再成立，因此不能跳过排序
    const bool ascending = (order == Qt::AscendingOrder);
    std::sort(m_filteredIndices.begin(), m_filteredIndices.end(),
        [this, &fileNames, column, ascending](int a, int b) -> bool {
            const ScanItem& sa = m_allData[size_t(a)];
            const ScanItem& sb = m_allData[size_t(b)];
            int c = 0;
            switch (column) {
            case 0: c = fileNames[size_t(a)].compare(fileNames[size_t(b)]); break;                // 名称（预计算）
            case 1: c = (sa.size < sb.size) ? -1 : (sa.size > sb.size) ? 1 : 0; break;            // 大小
            case 2: c = (sa.mtimeMs < sb.mtimeMs) ? -1 : (sa.mtimeMs > sb.mtimeMs) ? 1 : 0; break; // 修改日期
            case 3: c = (sa.ctimeMs < sb.ctimeMs) ? -1 : (sa.ctimeMs > sb.ctimeMs) ? 1 : 0; break; // 创建日期
            case 4: c = sa.path.compare(sb.path); break;                                          // 所在目录
            case 5: c = (int(sa.isDir) < int(sb.isDir)) ? -1 : (int(sa.isDir) > int(sb.isDir)) ? 1 : 0; break;  // 类型
            default: c = 0; break;
            }
            if (c == 0) return false;
            const bool less = (c < 0);
            return ascending ? less : !less;
        });
}

void ScanResultsModel::setResultFilter(qint64 cutoffMs, qint64 folderBytesThreshold, qint64 fileBytesThreshold)
{
    // 若阈值未变化，跳过重建
    if (m_cutoffMs == cutoffMs &&
        m_folderBytesThreshold == folderBytesThreshold &&
        m_fileBytesThreshold == fileBytesThreshold) {
        return;
    }

    m_cutoffMs = cutoffMs;
    m_folderBytesThreshold = folderBytesThreshold;
    m_fileBytesThreshold = fileBytesThreshold;

    const bool hasSearch = !m_includeTerms.isEmpty() || !m_excludeTerms.isEmpty();
    const bool hasTimeOrSize = (cutoffMs > 0) || (folderBytesThreshold > 0) || (fileBytesThreshold > 0);
    m_hasFilter = hasSearch || hasTimeOrSize;

    if (m_flatMode) {
        // 扁平模式（有搜索或排序）：重建 m_filteredIndices（含时间/大小过滤）
        beginResetModel();
        rebuildFilteredIndices();
        if (m_sortColumn >= 0) sortFilteredIndicesImpl();
        endResetModel();
    } else {
        // 树形模式：应用或恢复树形过滤（不切换到扁平）
        if (m_rootBatchTimer && m_rootBatchTimer->isActive()) m_rootBatchTimer->stop();
        beginResetModel();
        if (hasTimeOrSize) {
            applyTreeFilter();
        } else {
            restoreTree();
        }
        m_loaded.assign(m_allData.size(), 0);
        m_rootInsertedCount = 0;
        endResetModel();
        if (!m_roots.empty()) startBatchInsertRoots();
    }
}

// 判断项是否匹配时间/大小过滤
bool ScanResultsModel::matchesTimeAndSize(const ScanItem& s) const
{
    // 时间过滤：mtime 或 ctime 在 cutoff 之后才算匹配
    if (m_cutoffMs > 0) {
        const bool mtimeOk = (s.mtimeMs > 0 && s.mtimeMs >= m_cutoffMs);
        const bool ctimeOk = (s.ctimeMs > 0 && s.ctimeMs >= m_cutoffMs);
        if (!mtimeOk && !ctimeOk) return false;
    }
    // 大小过滤：文件夹与文件分别按各自阈值
    if (s.isDir) {
        if (m_folderBytesThreshold > 0 && s.size <= m_folderBytesThreshold) return false;
    } else {
        if (m_fileBytesThreshold > 0 && s.size <= m_fileBytesThreshold) return false;
    }
    return true;
}

// 树形过滤：重建 m_roots/m_children 只保留匹配项及其祖先
void ScanResultsModel::applyTreeFilter()
{
    const int n = int(m_allData.size());
    if (n == 0) return;

    // 首次激活树形过滤：保存原始树到 m_orig*（move 语义，避免拷贝）
    if (!m_treeFiltered) {
        m_origRoots = std::move(m_roots);
        m_origChildren = std::move(m_children);
        m_origParents = std::move(m_parents);
        m_origVisibleRowOfItem = std::move(m_visibleRowOfItem);
        m_treeFiltered = true;
    }

    // 标记每个项：0=不保留, 1=匹配项, 2=祖先（需保留以维持树结构）
    std::vector<uint8_t> keep(size_t(n), 0);
    for (int i = 0; i < n; ++i) {
        if (matchesTimeAndSize(m_allData[size_t(i)])) {
            keep[size_t(i)] = 1;
        }
    }
    // 从匹配项沿父链向上标记祖先
    for (int i = 0; i < n; ++i) {
        if (keep[size_t(i)] == 1) {
            int p = m_origParents[size_t(i)];
            while (p >= 0 && keep[size_t(p)] != 2) {
                keep[size_t(p)] = 2;
                p = m_origParents[size_t(p)];
            }
        }
    }

    // 基于原始树重建过滤后的树：保留 keep > 0 的项
    m_parents.assign(size_t(n), -1);
    m_children.assign(size_t(n), {});
    m_roots.clear();
    m_roots.reserve(m_origRoots.size());

    // 递归构建过滤后的子树（迭代式，避免深递归栈溢出）
    // origChildren[idx] 是原始顺序，保持排序
    std::function<void(int)> buildSubtree;
    buildSubtree = [&](int idx) {
        for (int child : m_origChildren[size_t(idx)]) {
            if (keep[size_t(child)]) {
                m_children[size_t(idx)].push_back(child);
                m_parents[size_t(child)] = idx;
                buildSubtree(child);
            }
        }
    };

    for (int root : m_origRoots) {
        if (keep[size_t(root)]) {
            m_roots.push_back(root);
            // 根的 parent 保持 -1
            buildSubtree(root);
        }
    }

    // 重建 visibleRowOfItem
    m_visibleRowOfItem.assign(size_t(n), 0);
    for (size_t i = 0; i < m_roots.size(); ++i) {
        m_visibleRowOfItem[size_t(m_roots[i])] = int(i);
    }
    for (size_t i = 0; i < m_children.size(); ++i) {
        const auto& ch = m_children[i];
        for (size_t j = 0; j < ch.size(); ++j) {
            m_visibleRowOfItem[size_t(ch[j])] = int(j);
        }
    }
}

// 从 m_orig* 恢复原始树
void ScanResultsModel::restoreTree()
{
    if (!m_treeFiltered) return;
    m_roots = std::move(m_origRoots);
    m_children = std::move(m_origChildren);
    m_parents = std::move(m_origParents);
    m_visibleRowOfItem = std::move(m_origVisibleRowOfItem);
    m_treeFiltered = false;
}

// 静默更新阈值（不重建索引）：用于多线程搜索路径
// 调用方应随后通过 setFilteredIndices 注入预计算好的索引
void ScanResultsModel::setResultFilterSilent(qint64 cutoffMs, qint64 folderBytesThreshold, qint64 fileBytesThreshold)
{
    m_cutoffMs = cutoffMs;
    m_folderBytesThreshold = folderBytesThreshold;
    m_fileBytesThreshold = fileBytesThreshold;
    // 不修改 m_hasFilter、不重建索引
    // hasFilter 状态由后续 setFilteredIndices 调用设置
}

void ScanResultsModel::setFilteredIndices(std::vector<int>&& indices, bool hasFilter)
{
    if (m_rootBatchTimer && m_rootBatchTimer->isActive()) m_rootBatchTimer->stop();

    // 搜索结果注入：hasFilter 表示有搜索（可能含时间/大小过滤）
    // 搜索总是切换到扁平模式
    const bool hasSearch = hasFilter;
    const bool shouldFlat = hasSearch || (m_sortColumn >= 0);

    if (shouldFlat) {
        beginResetModel();
        if (!m_flatMode) {
            // 从树形切到扁平：恢复原始树
            restoreTree();
            m_loaded.assign(m_allData.size(), 1);
        }
        m_flatMode = true;
        m_hasFilter = hasFilter;
        m_rootInsertedCount = int(m_roots.size());
        m_filteredIndices = std::move(indices);
        if (m_sortColumn >= 0) sortFilteredIndicesImpl();
        endResetModel();
    } else {
        // 无搜索且无排序 → 切回树形
        beginResetModel();
        m_flatMode = false;
        m_hasFilter = false;
        m_filteredIndices.clear();
        restoreTree();
        // 时间/大小过滤可能仍存在，应用树形过滤
        const bool hasTimeOrSize = (m_cutoffMs > 0) || (m_folderBytesThreshold > 0) || (m_fileBytesThreshold > 0);
        if (hasTimeOrSize) {
            applyTreeFilter();
        }
        m_loaded.assign(m_allData.size(), 0);
        m_rootInsertedCount = 0;
        endResetModel();
        if (!m_roots.empty()) startBatchInsertRoots();
    }
}

std::vector<const ScanItem*> ScanResultsModel::filteredData() const
{
    std::vector<const ScanItem*> out;
    if (m_flatMode) {
        // 扁平模式：仅 m_filteredIndices 中的项可见
        out.reserve(m_filteredIndices.size());
        for (int idx : m_filteredIndices) {
            out.push_back(&m_allData[size_t(idx)]);
        }
    } else if (m_treeFiltered) {
        // 树形过滤模式：仅 m_roots/m_children 中的项可见
        // 遍历过滤后的树，收集所有项
        const int n = int(m_allData.size());
        std::vector<uint8_t> visited(size_t(n), 0);
        out.reserve(m_allData.size());  // 预估上限
        std::function<void(int)> collect;
        collect = [&](int idx) {
            if (visited[size_t(idx)]) return;
            visited[size_t(idx)] = 1;
            out.push_back(&m_allData[size_t(idx)]);
            for (int child : m_children[size_t(idx)]) {
                collect(child);
            }
        };
        for (int root : m_roots) {
            collect(root);
        }
    } else {
        // 树形无过滤：所有项可见
        out.reserve(m_allData.size());
        for (const ScanItem& s : m_allData) {
            out.push_back(&s);
        }
    }
    return out;
}

// 多线程预热：主线程收集唯一扩展名，后台线程并行调用 SHGetFileInfoW 解析类型名
// 主线程只做轻量遍历（O(N) 收集扩展名），重活（每个扩展名调用一次 SHGetFileInfoW）
// 移到后台线程；SHGetFileInfoW 是 Win32 API，可安全在非主线程调用
// 图标（QFileIconProvider 非线程安全）保留在主线程懒解析
void ScanResultsModel::prewarmCacheAsync()
{
    if (m_allData.empty()) return;
    // 取消对旧结果的关注（不中断后台计算，但结果会被代际号丢弃）
    ++m_prewarmGeneration;
    const int gen = m_prewarmGeneration;

    // 主线程：收集所有唯一扩展名（含 "<noext>" 表示无扩展名）
    // 缓存键与 resolveFileInfo 一致：".ext" 或 "<noext>"
    QSet<QString> uniqueKeys;  // 缓存键集合
    QSet<QString> uniqueExts;  // 扩展名（不带点，用于回退描述）
    for (const ScanItem& s : m_allData) {
        if (s.isDir) continue;
        const int lastDot = s.path.lastIndexOf(QLatin1Char('.'));
        const int lastSep = std::max<int>(s.path.lastIndexOf(QLatin1Char('\\')),
                                          s.path.lastIndexOf(QLatin1Char('/')));
        if (lastDot >= 0 && lastDot > lastSep) {
            const QString ext = s.path.mid(lastDot + 1).toLower();
            uniqueExts.insert(ext);
            uniqueKeys.insert(QStringLiteral(".") + ext);
        } else {
            uniqueExts.insert(QString());
            uniqueKeys.insert(QStringLiteral("<noext>"));
        }
    }
    if (uniqueKeys.isEmpty()) return;

    // 拷贝扩展名列表到后台线程（避免后台访问主线程对象）
    QList<QString> extsList = uniqueExts.values();
    // 启动后台任务：对每个扩展名调用 SHGetFileInfoW 获取类型名
    QFuture<QHash<QString, QString>> future = QtConcurrent::run(
        [exts = std::move(extsList)]() -> QHash<QString, QString> {
            QHash<QString, QString> result;
            result.reserve(int(exts.size()));
#ifdef Q_OS_WIN
            for (const QString& ext : exts) {
                const QString fakePath = ext.isEmpty()
                    ? QStringLiteral("C:\\__prewarm_dummy__")
                    : (QStringLiteral("C:\\__prewarm_dummy__.") + ext);
                SHFILEINFOW sfi = {};
                const QString nativePath = QDir::toNativeSeparators(fakePath);
                const DWORD flags = SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES;
                QString typeName;
                if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
                                   FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), flags)) {
                    if (sfi.szTypeName[0]) {
                        typeName = QString::fromWCharArray(sfi.szTypeName);
                    }
                }
                if (typeName.isEmpty()) {
                    // 回退：与 resolveFileInfo 一致的回退描述
                    typeName = ext.isEmpty() ? QStringLiteral("文件")
                                             : (ext + QStringLiteral(" 文件"));
                }
                const QString key = ext.isEmpty()
                    ? QStringLiteral("<noext>")
                    : (QStringLiteral(".") + ext);
                result.insert(key, typeName);
            }
#endif
            return result;
        });
    // 记录此次任务的代际号，完成时比对
    m_prewarmGenAtLaunch = gen;
    m_prewarmWatcher->setFuture(future);
}

// 后台 prewarm 完成：在主线程合并类型名到 m_fileTypeCache
// 若代际号已过期（被 clear/setResults 递增），丢弃结果
// 仅当缓存项不存在或图标为空且类型名为空时才写入，避免覆盖主线程已解析的完整项
void ScanResultsModel::onPrewarmFinished()
{
    if (m_prewarmGenAtLaunch != m_prewarmGeneration) return;  // 过期
    if (m_allData.empty()) return;  // 数据已被清空

    QHash<QString, QString> result = m_prewarmWatcher->result();
    for (auto it = result.constBegin(); it != result.constEnd(); ++it) {
        const QString& key = it.key();
        const QString& typeName = it.value();
        const auto cacheIt = m_fileTypeCache.constFind(key);
        if (cacheIt != m_fileTypeCache.constEnd()) {
            // 缓存已存在：仅当类型名为空时补上（避免覆盖主线程已解析的完整项）
            if (cacheIt.value().type.isEmpty()) {
                FileTypeInfo info = cacheIt.value();
                info.type = typeName;
                m_fileTypeCache.insert(key, info);
            }
            // 否则跳过：主线程已解析过（可能含图标），保留主线程版本
        } else {
            // 缓存不存在：插入仅含类型名的项（图标为空，待懒解析）
            FileTypeInfo info;
            info.type = typeName;
            m_fileTypeCache.insert(key, info);
        }
    }
    // 不主动触发 dataChanged：
    // - 类型名：主线程 resolveFileInfo 在 prewarm 完成前已通过 SHGetFileInfoW 解析，
    //   结果与后台一致，无需刷新
    // - 图标：data() 调用时按需懒解析（QFileIconProvider），自然填充
}

// 获取类型缓存的快照：扩展名（带点，如 ".txt"，无扩展名用 "<noext>"）→ 类型描述
// 用于后台保存线程，避免在后台线程重复调用 SHGetFileInfo
TypeCache ScanResultsModel::typeCacheSnapshot() const
{
    TypeCache snapshot;
    snapshot.reserve(m_fileTypeCache.size());
    for (auto it = m_fileTypeCache.constBegin(); it != m_fileTypeCache.constEnd(); ++it) {
        snapshot.insert(it.key(), it.value().type);
    }
    return snapshot;
}

void ScanResultsModel::rebuildFilteredIndices()
{
    m_filteredIndices = computeFilteredIndices(m_includeTerms, m_excludeTerms,
                                                m_cutoffMs,
                                                m_folderBytesThreshold,
                                                m_fileBytesThreshold);
}

// 线程安全：仅读 m_allData，不修改任何成员状态
// 可在后台线程执行；主线程拿到结果后通过 setFilteredIndices 注入
std::vector<int> ScanResultsModel::computeFilteredIndices(const QStringList& includeTerms,
                                                          const QStringList& excludeTerms,
                                                          qint64 cutoffMs,
                                                          qint64 folderBytesThreshold,
                                                          qint64 fileBytesThreshold) const
{
    std::vector<int> result;
    const int n = int(m_allData.size());
    const bool hasSearch = !includeTerms.isEmpty() || !excludeTerms.isEmpty();
    const bool hasTimeFilter = (cutoffMs > 0);
    const bool hasFolderSize = (folderBytesThreshold > 0);
    const bool hasFileSize = (fileBytesThreshold > 0);
    const bool anyFilter = hasSearch || hasTimeFilter || hasFolderSize || hasFileSize;

    if (!anyFilter) {
        // 无过滤：索引为 0..N-1
        result.resize(size_t(n));
        for (int i = 0; i < n; ++i) result[size_t(i)] = i;
        return result;
    }

    result.reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
        const ScanItem& s = m_allData[size_t(i)];

        // 时间过滤：mtime 或 ctime 在 cutoff 之后才算匹配
        if (hasTimeFilter) {
            const bool mtimeOk = (s.mtimeMs > 0 && s.mtimeMs >= cutoffMs);
            const bool ctimeOk = (s.ctimeMs > 0 && s.ctimeMs >= cutoffMs);
            if (!mtimeOk && !ctimeOk) continue;
        }

        // 大小过滤：文件夹与文件分别按各自阈值
        if (s.isDir) {
            if (hasFolderSize && s.size <= folderBytesThreshold) continue;
        } else {
            if (hasFileSize && s.size <= fileBytesThreshold) continue;
        }

        // 搜索过滤：只匹配文件名（不含路径）
        if (hasSearch) {
            // 提取文件名（最后一个分隔符之后的部分），避免构造 QFileInfo
            const QString& p = s.path;
            int lastSep = p.lastIndexOf(QLatin1Char('\\'));
            if (lastSep < 0) lastSep = p.lastIndexOf(QLatin1Char('/'));
            const QString name = lastSep < 0
                ? p
                : p.mid(lastSep + 1);

            // 排除项：文件名包含任一即拒绝
            bool excluded = false;
            for (const QString& ex : excludeTerms) {
                if (name.contains(ex, Qt::CaseInsensitive)) {
                    excluded = true;
                    break;
                }
            }
            if (excluded) continue;

            // 包含项：非空时文件名需命中至少一个
            if (!includeTerms.isEmpty()) {
                bool found = false;
                for (const QString& in : includeTerms) {
                    if (name.contains(in, Qt::CaseInsensitive)) {
                        found = true;
                        break;
                    }
                }
                if (!found) continue;
            }
        }

        result.push_back(i);
    }
    return result;
}
