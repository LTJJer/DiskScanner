#ifndef SCANRESULTSMODEL_H
#define SCANRESULTSMODEL_H

#include <QAbstractItemModel>
#include <QIcon>
#include <QStringList>
#include <vector>
#include "ScanTypes.h"

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
// 列定义：0 类型  1 路径  2 大小  3 修改时间  4 创建时间
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

    // 排序：树形模式下按层排序（根 + 每个节点的子项）；扁平模式下对 m_filteredIndices 排序
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    // 通过 QModelIndex 取原始数据（双击等场景）
    const ScanItem* itemForIndex(const QModelIndex& idx) const;

    // 获取全量数据（保存到文件等场景）
    const std::vector<ScanItem>& allData() const { return m_allData; }

    // 全量条目数（不受过滤影响）
    int totalCount() const { return int(m_allData.size()); }

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

    std::vector<ScanItem> m_allData;          // 全量数据（唯一存储）

    // 树形索引（与 m_allData 下标一一对应）
    std::vector<int> m_parents;               // parents[i] = 父项下标，-1 为根
    std::vector<std::vector<int>> m_children; // children[i] = i 的直接子项下标列表
    std::vector<int> m_roots;                 // 根级项下标列表
    std::vector<int> m_visibleRowOfItem;      // visibleRowOfItem[i] = i 在其父项中的行号
    std::vector<uint8_t> m_loaded;            // loaded[i] = i 的子项是否已通过 fetchMore 暴露给视图

    // 扁平模式（过滤时使用）
    std::vector<int> m_filteredIndices;       // 扁平可见行对应的 m_allData 下标
    bool m_flatMode = false;                  // 是否处于扁平模式

    // 根级分批加载
    int m_rootInsertedCount = 0;              // 树形模式下已通过 beginInsertRows 暴露的根数
    QTimer* m_rootBatchTimer = nullptr;
    static const int BATCH_SIZE = 200;

    // 过滤与排序
    QStringList m_includeTerms;
    QStringList m_excludeTerms;
    bool m_hasFilter = false;
    QIcon m_dirIcon;
    QIcon m_fileIcon;
    int m_sortColumn = 2;
    Qt::SortOrder m_sortOrder = Qt::DescendingOrder;
};

#endif // SCANRESULTSMODEL_H
