#ifndef SCANRESULTSMODEL_H
#define SCANRESULTSMODEL_H

#include <QAbstractItemModel>
#include <QIcon>
#include <QStringList>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QFileIconProvider>
#include <QFutureWatcher>
#include <vector>
#include "ScanTypes.h"
#include "ResultsFormatter.h"  // TypeCache

class QTimer;

// 扫描结果树形模型
//
// 设计目标：
//   - 单一数据存储：m_allData 持有全量 ScanItem，无任何拷贝
//   - 树结构存储：m_parents / m_children / m_roots 仅存下标（4 字节/项）
//   - 分批加载根级项：扫描完成后立即显示前 BATCH_SIZE 个根，剩余按 1ms/批追加，
//     UI 不卡顿
//   - 子节点延迟加载：展开时通过 fetchMore 一次性填充直接子项
//   - 过滤时切换为扁平模式：保留原过滤语义（m_filteredIndices），过滤/排序逻辑
//     与原扁平模型一致；清除过滤后切回树形，并保留已展开状态
//
// 列定义：0 名称  1 大小  2 修改日期  3 创建日期  4 所在目录  5 类型
//   - 列 0 固定在最左（不可拖动），承载树形展开与项图标 + 名称
//   - 列 1~5 可拖动改顺序
//   - 日期格式：yyyy/M/d ddd  H:mm:ss
class ScanResultsModel : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit ScanResultsModel(QObject* parent = nullptr);
    ~ScanResultsModel() override;

    // 批量设置结果与树索引（move 语义，零拷贝；触发 modelReset + 启动根级分批加载）
    void setResultsWithTree(std::vector<ScanItem>&& results, TreeIndex&& tree);

    // 清空（重置所有树形状态与分批状态）
    void clear();

    // 设置/清除路径关键词过滤（include 任一 + exclude 全部）
    // 设置后切换为扁平模式；清除后切回树形
    void setFilter(const QStringList& includeTerms, const QStringList& excludeTerms);
    void clearFilter();

    // 设置结果过滤参数（时间范围、文件夹大小、文件大小阈值，0=不过滤）
    // 行为：
    //   - 扁平模式（有搜索/排序）：重建 m_filteredIndices（含时间/大小过滤）
    //   - 树形模式（无搜索/排序）：应用树形过滤，从树中移除不匹配项（保留匹配项及其祖先）
    //     不切换到扁平模式
    // cutoffMs：截止时刻（Unix 毫秒），mtimeMs/ctimeMs >= cutoffMs 才算匹配；0=不过滤
    void setResultFilter(qint64 cutoffMs, qint64 folderBytesThreshold, qint64 fileBytesThreshold);

    // 静默更新结果过滤参数（仅更新内部阈值，不触发索引重建）
    // 用于多线程搜索路径：后台 computeFilteredIndices 会用新阈值计算，
    // 完成后通过 setFilteredIndices 注入索引，避免同步重建开销
    void setResultFilterSilent(qint64 cutoffMs, qint64 folderBytesThreshold, qint64 fileBytesThreshold);

    // 多线程搜索：用预构建的索引直接设置扁平模式
    // hasFilter=true 表示有搜索/时间/大小过滤；false 表示无过滤
    // 排序状态由模型内部 m_sortColumn 决定
    // 注意：搜索总是切换到扁平模式（即使有时间/大小过滤但不切换扁平的需求，
    //       搜索本身仍需扁平，因为搜索结果跨树层级）
    void setFilteredIndices(std::vector<int>&& indices, bool hasFilter);

    // 线程安全：在后台线程计算过滤索引（不修改模型状态）
    // 参数为搜索词、时间/大小阈值，返回匹配项在 m_allData 中的下标集合
    // 主线程拿到结果后调用 setFilteredIndices 注入
    // 注意：调用期间 m_allData 必须不被修改（扫描完成后即稳定）
    std::vector<int> computeFilteredIndices(const QStringList& includeTerms,
                                            const QStringList& excludeTerms,
                                            qint64 cutoffMs,
                                            qint64 folderBytesThreshold,
                                            qint64 fileBytesThreshold) const;

    // 获取过滤后的数据（用于保存：只保存可见项）
    // 返回指向 m_allData 中可见项的指针列表（不拷贝 ScanItem）
    std::vector<const ScanItem*> filteredData() const;

    // 预热文件类型类型名缓存（多线程版）：后台线程对每个唯一扩展名调用
    // SHGetFileInfoW 解析系统类型名（如"文本文档"）；图标在主线程按需懒解析
    // 调用后立即返回，结果通过 QFutureWatcher 在主线程合并到 m_fileTypeCache
    // 使用 m_prewarmGeneration 在 clear/setResults 时丢弃过期结果
    void prewarmCacheAsync();

    // 排序：树形模式下按层排序（根 + 每个节点的子项）；扁平模式下对 m_filteredIndices 排序
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    // 通过 QModelIndex 取原始数据（双击等场景）
    const ScanItem* itemForIndex(const QModelIndex& idx) const;

    // 获取全量数据（保存到文件等场景）
    const std::vector<ScanItem>& allData() const { return m_allData; }

    // 获取类型缓存的快照（扩展名 → 类型描述），用于后台保存线程
    // 避免后台线程重复调用 SHGetFileInfo
    TypeCache typeCacheSnapshot() const;

    // 从模型中移除指定路径的项（含子项），并基于路径前缀重建树索引
    // 用于"删除到回收站"后从视图中移除已删除项
    void removePaths(const QSet<QString>& pathsToRemove);

    // 全量条目数（不受过滤影响）
    int totalCount() const { return int(m_allData.size()); }

    // 当前排序列：-1 表示无排序（树形模式），0~5 为列索引
    int sortColumn() const { return m_sortColumn; }
    Qt::SortOrder sortOrder() const { return m_sortOrder; }
    // 当前是否为扁平模式（排序或过滤激活时为 true）
    bool isFlatMode() const { return m_flatMode; }

    // QAbstractItemModel 重写
    QModelIndex index(int row, int column,
                      const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                         int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool hasChildren(const QModelIndex& parent = QModelIndex()) const override;
    bool canFetchMore(const QModelIndex& parent = QModelIndex()) const override;
    void fetchMore(const QModelIndex& parent) override;

private:
    // 树形模式：启动分批插入根级项
    void startBatchInsertRoots();
    // 树形模式：插入下一批根级项（由 m_rootBatchTimer 触发）
    void insertRootBatch();
    // 树形模式：停止定时器并一次性插入所有剩余根级项
    // 用于过滤/排序/清空等需要完整数据的场景
    void stopBatchInsertRoots();

    // 扁平模式：重建过滤索引
    void rebuildFilteredIndices();
    // 扁平模式：对 m_filteredIndices 做间接排序
    void sortFilteredIndicesImpl();
    // 树形模式：按 m_sortColumn/m_sortOrder 排序每一层并重建 visibleRowOfItem
    void sortTreeImpl();

    // 树形过滤：基于时间/大小阈值重建 m_roots/m_children，只保留匹配项及其祖先
    // 首次调用时保存原始树到 m_orig*；阈值变化时基于 m_orig* 重建
    void applyTreeFilter();
    // 从 m_orig* 恢复原始树（清除树形过滤）
    void restoreTree();
    // 判断单个项是否匹配时间/大小过滤（用于树形过滤与扁平过滤共用）
    bool matchesTimeAndSize(const ScanItem& s) const;

    // 基于 m_allData 的路径前缀包含关系重建树索引
    // 用于 removePaths 后恢复树结构（路径排序 + 栈式父项查找）
    void rebuildTreeFromPaths();

    // 按扩展名解析文件图标与类型描述（含缓存）
    // 返回 {icon, typeStr}，typeStr 为资源管理器风格（如 "文本文档"，无 .ext 前缀）
    // 若缓存项存在但图标为空（来自后台 prewarm 仅解析了类型名），则懒解析图标
    struct FileTypeInfo { QIcon icon; QString type; };
    const FileTypeInfo& resolveFileInfo(const QString& path) const;

    // 后台 prewarm 完成回调：将类型名合并到 m_fileTypeCache（图标保持为空，懒解析）
    void onPrewarmFinished();

    std::vector<ScanItem> m_allData;          // 全量数据（唯一存储）

    // 树形索引（与 m_allData 下标一一对应）
    std::vector<int> m_parents;               // parents[i] = 父项下标，-1 为根
    std::vector<std::vector<int>> m_children; // children[i] = i 的直接子项下标列表
    std::vector<int> m_roots;                 // 根级项下标列表
    std::vector<int> m_visibleRowOfItem;      // visibleRowOfItem[i] = i 在其父项中的行号
    std::vector<uint8_t> m_loaded;            // loaded[i] = i 的子项是否已通过 fetchMore 暴露给视图

    // 原始树索引备份（树形过滤激活时保存，清除时恢复）
    // 时间/大小过滤不切换扁平模式，而是在树形模式下过滤：
    // 过滤时 m_roots/m_children 被重建为只含匹配项，原始数据移到 m_orig*
    std::vector<int> m_origRoots;
    std::vector<std::vector<int>> m_origChildren;
    std::vector<int> m_origParents;
    std::vector<int> m_origVisibleRowOfItem;
    bool m_treeFiltered = false;              // 是否处于树形过滤状态

    // 扁平模式（过滤时使用）
    std::vector<int> m_filteredIndices;       // 扁平可见行对应的 m_allData 下标
    bool m_flatMode = false;                  // 是否处于扁平模式

    // 根级分批加载
    int m_rootInsertedCount = 0;              // 树形模式下已通过 beginInsertRows 暴露的根数
    QTimer* m_rootBatchTimer = nullptr;
    static constexpr int BATCH_SIZE = 200;

    // 过滤与排序
    QStringList m_includeTerms;
    QStringList m_excludeTerms;
    bool m_hasFilter = false;
    // 结果过滤（时间 + 大小，0 = 不过滤）
    qint64 m_cutoffMs = 0;                // 截止时刻（Unix 毫秒），0 = 不限时间
    qint64 m_folderBytesThreshold = 0;    // 文件夹大小阈值，0 = 不限
    qint64 m_fileBytesThreshold = 0;      // 文件大小阈值，0 = 不限
    QIcon m_dirIcon;
    QIcon m_fileIcon;
    QFileIconProvider m_iconProvider;
    mutable QHash<QString, FileTypeInfo> m_fileTypeCache;  // 按扩展名缓存 {icon, type}
    int m_sortColumn = -1;  // -1 = 无排序（树形模式），0~5 = 按列排序（扁平模式）
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;

    // 后台 prewarm：在后台线程解析所有唯一扩展名的类型名（SHGetFileInfoW）
    // 完成后通过 onPrewarmFinished 在主线程合并到 m_fileTypeCache
    // 图标在主线程按需懒解析（QFileIconProvider 非线程安全）
    QFutureWatcher<QHash<QString, QString>>* m_prewarmWatcher = nullptr;
    int m_prewarmGeneration = 0;      // 代际号：clear/setResults 时递增，丢弃过期结果
    int m_prewarmGenAtLaunch = 0;     // 当前后台任务启动时的代际号，完成时比对
};

#endif // SCANRESULTSMODEL_H
