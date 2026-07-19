#include "ScanResultsModel.h"

#include <QApplication>
#include <QStyle>
#include <QDateTime>
#include <QTimer>
#include <algorithm>

#include "ResultsFormatter.h"

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
}

ScanResultsModel::~ScanResultsModel() = default;

void ScanResultsModel::setResultsWithTree(std::vector<ScanItem>&& results, TreeIndex&& tree)
{
    if (m_rootBatchTimer) m_rootBatchTimer->stop();

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
    m_flatMode = false;
    m_filteredIndices.clear();

    // worker 已按 size 降序排序每一层，重置 sort 状态与之一致
    m_sortColumn = 2;
    m_sortOrder  = Qt::DescendingOrder;

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
    beginResetModel();
    std::vector<ScanItem>().swap(m_allData);
    std::vector<int>().swap(m_parents);
    std::vector<std::vector<int>>().swap(m_children);
    std::vector<int>().swap(m_roots);
    std::vector<int>().swap(m_visibleRowOfItem);
    std::vector<uint8_t>().swap(m_loaded);
    std::vector<int>().swap(m_filteredIndices);
    m_includeTerms.clear();
    m_excludeTerms.clear();
    m_hasFilter = false;
    m_flatMode = false;
    m_rootInsertedCount = 0;
    endResetModel();
}

void ScanResultsModel::setFilter(const QStringList& includeTerms, const QStringList& excludeTerms)
{
    // 若过滤器未实质变化，跳过重建
    if (m_includeTerms == includeTerms && m_excludeTerms == excludeTerms) {
        return;
    }

    // 停止根级分批加载并同步状态：m_rootInsertedCount 设为满，避免切回树形时遗漏根
    if (m_rootBatchTimer && m_rootBatchTimer->isActive()) m_rootBatchTimer->stop();
    m_rootInsertedCount = int(m_roots.size());

    beginResetModel();
    m_includeTerms = includeTerms;
    m_excludeTerms = excludeTerms;
    m_hasFilter = !includeTerms.isEmpty() || !excludeTerms.isEmpty();
    m_flatMode = m_hasFilter;
    rebuildFilteredIndices();
    sortFilteredIndicesImpl();
    endResetModel();
}

void ScanResultsModel::clearFilter()
{
    if (!m_hasFilter) return;

    // 切回树形：m_loaded 与 m_rootInsertedCount 保留（已展开状态与已加载根继续可见）
    beginResetModel();
    m_includeTerms.clear();
    m_excludeTerms.clear();
    m_hasFilter = false;
    m_flatMode = false;
    m_filteredIndices.clear();
    endResetModel();
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
    if (row < 0 || column < 0 || column >= 5) return QModelIndex();

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
    return 5;
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
        case 0: return s->isDir ? QStringLiteral("目录") : QStringLiteral("文件");
        case 1: return s->path;
        case 2: return formatSize(s->size);
        case 3: return s->mtimeMs > 0
            ? QDateTime::fromMSecsSinceEpoch(s->mtimeMs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : QStringLiteral("-");
        case 4: return s->ctimeMs > 0
            ? QDateTime::fromMSecsSinceEpoch(s->ctimeMs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : QStringLiteral("-");
        }
        break;
    }
    case Qt::DecorationRole: {
        if (index.column() == 0) return s->isDir ? m_dirIcon : m_fileIcon;
        break;
    }
    case Qt::TextAlignmentRole: {
        if (index.column() == 2) return int(Qt::AlignRight | Qt::AlignVCenter);
        break;
    }
    case Qt::UserRole: {
        if (index.column() == 2) return s->size;
        break;
    }
    }
    return QVariant();
}

QVariant ScanResultsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return QVariant();
    switch (section) {
    case 0: return QStringLiteral("类型");
    case 1: return QStringLiteral("路径");
    case 2: return QStringLiteral("大小");
    case 3: return QStringLiteral("修改时间");
    case 4: return QStringLiteral("创建时间");
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
    if (m_flatMode) return false;
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

    if (m_flatMode) {
        layoutAboutToBeChanged();
        sortFilteredIndicesImpl();
        layoutChanged();
    } else {
        // 排序前确保所有根级已插入（独立的 model change，不能与 layoutChanged 嵌套）
        stopBatchInsertRoots();
        layoutAboutToBeChanged();
        sortTreeImpl();
        layoutChanged();
    }
}

void ScanResultsModel::sortTreeImpl()
{
    auto cmp = [this](int a, int b) -> bool {
        const ScanItem& sa = m_allData[size_t(a)];
        const ScanItem& sb = m_allData[size_t(b)];
        int c = 0;
        switch (m_sortColumn) {
        case 0: c = (int(sa.isDir) < int(sb.isDir)) ? -1 : (int(sa.isDir) > int(sb.isDir)) ? 1 : 0; break;
        case 1: c = sa.path.compare(sb.path); break;
        case 2: c = (sa.size < sb.size) ? -1 : (sa.size > sb.size) ? 1 : 0; break;
        case 3: c = (sa.mtimeMs < sb.mtimeMs) ? -1 : (sa.mtimeMs > sb.mtimeMs) ? 1 : 0; break;
        case 4: c = (sa.ctimeMs < sb.ctimeMs) ? -1 : (sa.ctimeMs > sb.ctimeMs) ? 1 : 0; break;
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

    // worker 已对 m_allData 按 size 降序预排序（每层），
    // 但扁平模式下 m_filteredIndices 是按 m_allData 下标顺序构建的，
    // 全局 size 降序不再成立，因此不能跳过排序
    if (order == Qt::AscendingOrder) {
        std::sort(m_filteredIndices.begin(), m_filteredIndices.end(),
            [this, column](int a, int b) {
                const ScanItem& sa = m_allData[size_t(a)];
                const ScanItem& sb = m_allData[size_t(b)];
                switch (column) {
                case 0: return sa.isDir < sb.isDir;
                case 1: return sa.path < sb.path;
                case 2: return sa.size < sb.size;
                case 3: return sa.mtimeMs < sb.mtimeMs;
                case 4: return sa.ctimeMs < sb.ctimeMs;
                default: return false;
                }
            });
    } else {
        std::sort(m_filteredIndices.begin(), m_filteredIndices.end(),
            [this, column](int a, int b) {
                const ScanItem& sa = m_allData[size_t(a)];
                const ScanItem& sb = m_allData[size_t(b)];
                switch (column) {
                case 0: return sa.isDir > sb.isDir;
                case 1: return sa.path > sb.path;
                case 2: return sa.size > sb.size;
                case 3: return sa.mtimeMs > sb.mtimeMs;
                case 4: return sa.ctimeMs > sb.ctimeMs;
                default: return false;
                }
            });
    }
}

void ScanResultsModel::rebuildFilteredIndices()
{
    m_filteredIndices.clear();
    const int n = int(m_allData.size());
    if (m_includeTerms.isEmpty() && m_excludeTerms.isEmpty()) {
        // 无过滤：索引为 0..N-1
        m_filteredIndices.resize(size_t(n));
        for (int i = 0; i < n; ++i) m_filteredIndices[size_t(i)] = i;
    } else {
        m_filteredIndices.reserve(size_t(n));
        for (int i = 0; i < n; ++i) {
            const QString& path = m_allData[size_t(i)].path;
            // 排除项：包含任一即拒绝
            bool excluded = false;
            for (const QString& ex : m_excludeTerms) {
                if (path.contains(ex, Qt::CaseInsensitive)) {
                    excluded = true;
                    break;
                }
            }
            if (excluded) continue;
            // 包含项：非空时需命中至少一个
            if (!m_includeTerms.isEmpty()) {
                bool found = false;
                for (const QString& in : m_includeTerms) {
                    if (path.contains(in, Qt::CaseInsensitive)) {
                        found = true;
                        break;
                    }
                }
                if (!found) continue;
            }
            m_filteredIndices.push_back(i);
        }
    }
}
