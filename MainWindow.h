#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QWidget>
#include <QThread>
#include <QTimer>
#include <QStringList>
#include <QModelIndex>
#include <QFutureWatcher>
#include <QHash>
#include <vector>

#include "ScanTypes.h"
#include "ResultsFormatter.h"

class ScanWorker;
class ScanResultsModel;

namespace Ui { class MainWindow; }

// 搜索过滤器：解析 "AAA BBB !CCC !DDD" 形式的语法
//   - includeTerms：无前缀词，路径需包含其中任一（若非空）
//   - excludeTerms：! 前缀词，路径不可包含其中任何一个
struct SearchFilter {
    QStringList includeTerms;
    QStringList excludeTerms;

    bool isEmpty() const { return includeTerms.isEmpty() && excludeTerms.isEmpty(); }
    bool matches(const QString& path) const;
};

// 解析搜索文本为 SearchFilter
SearchFilter parseSearchFilter(const QString& text);

class MainWindow : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private slots:
    void onBrowse();
    void onScan();
    void onCancel();
    void onSave();
    void onClear();
    void onProgress(int scanned, int dirCount, int fileCount, int matchedCount,
                    qint64 elapsedMs, const QString& currentPath,
                    qint64 scannedBytes, qint64 totalBytes);
    void onFinished();
    void onItemDoubleClicked(const QModelIndex& index);
    void onSearchDebounced();  // 搜索框输入防抖触发
    void onDeleteSelected();   // Del 键：将选中项移至回收站
    void onHeaderClicked(int column);  // 表头点击：3 态排序循环
    void onResultFilterChanged();      // 时间/大小过滤 UI 变化：重新应用结果过滤
    void onSearchFinished();           // 后台搜索完成：注入索引到模型


private:
    Ui::MainWindow* ui = nullptr;
    QThread* m_thread = nullptr;
    ScanWorker* m_worker = nullptr;
    ScanResultsModel* m_model = nullptr;       // 持有全量数据 + 过滤索引（唯一存储）
    bool m_cancelled = false;
    bool m_failed = false;

    ScanParams m_lastParams;
    SearchFilter m_currentFilter;
    QTimer* m_searchDebounce = nullptr;

    // 多线程搜索：后台计算过滤索引，避免主线程卡顿
    QFutureWatcher<std::vector<int>>* m_searchWatcher = nullptr;
    int m_searchGeneration = 0;  // 搜索代次：用于丢弃过期结果（用户已输入新内容）

    // 多线程保存：后台格式化 + 写文件，避免主线程卡顿
    // 保存期间禁用 扫描/清空/删除 等可能修改 m_allData 的操作，保证指针有效
    QFutureWatcher<bool>* m_saveWatcher = nullptr;
    bool m_saveInProgress = false;
    QString m_currentSavePath;  // 当前保存任务的目标路径（用于完成回调）

    // 统计：扫描完成时记录，避免每次过滤都重新遍历模型
    int m_lastTotalAll = 0;
    int m_lastTotalDirs = 0;
    int m_lastTotalFiles = 0;
    int m_lastScanned = 0;
    qint64 m_lastElapsed = 0;
    // 扫描器实际使用的线程数与驱动器类型（用于状态栏展示自适应决策）
    int m_lastThreadCount = 0;
    DriveKind m_lastDriveKind = DriveKind::Unknown;

    // 上次保存结果目录（持久化到 QSettings）
    QString m_lastSaveDir;

    // 持久化列宽与列顺序（loadSettings 填充，构造函数的 singleShot 应用）
    // m_savedColumnWidths[logicalIndex] = 列宽
    // m_savedColumnOrder[visualPos] = 应位于该视觉位置的逻辑索引
    std::vector<int> m_savedColumnWidths;
    std::vector<int> m_savedColumnOrder;

    void startScan(const QString& root, qint64 timeRangeMs,
                   qint64 folderBytesThreshold, qint64 fileBytesThreshold,
                   int threadCount);
    void launchBackgroundFilter();  // 后台线程计算过滤索引（搜索 + 时间 + 大小）
    void refreshStatusBar();        // 刷新状态栏与按钮启用状态（无过滤操作）
    void resetUiIdle();
    void cleanupThread();        // 停止线程并释放 worker/thread 对象

    // 后台保存结果：在工作线程中格式化 + 写文件
    // 数据（指针、列顺序、类型缓存等）在主线程收集后按值传入，确保线程安全
    void launchBackgroundSave(const QString& path, SaveFormat fmt);
    void onSaveFinished();       // 后台保存完成的回调（主线程）
    void setSaveInProgress(bool inProgress);  // 切换保存期间的 UI 状态

    // 从 UI 读取当前时间/大小过滤参数（毫秒/字节，0=不过滤）
    void readResultFilterParams(qint64& cutoffMs, qint64& folderBytes, qint64& fileBytes) const;

    // 配置持久化（QSettings，INI 格式）
    // 保存内容：扫描目录、时间/大小过滤值、并发线程数、上次保存目录、列顺序与宽度
    void loadSettings();
    void saveSettings() const;
};

#endif // MAINWINDOW_H
