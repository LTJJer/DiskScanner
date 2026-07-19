#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QWidget>
#include <QThread>
#include <QTimer>
#include <QStringList>
#include <QModelIndex>
#include <vector>

#include "ScanTypes.h"

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

private slots:
    void onBrowse();
    void onScan();
    void onCancel();
    void onSave();
    void onClear();
    void onProgress(int scanned, int dirCount, int fileCount, int matchedCount,
                    qint64 elapsedMs, const QString& currentPath);
    void onFinished();
    void onItemDoubleClicked(const QModelIndex& index);
    void onSearchDebounced();  // 搜索框输入防抖触发


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

    // 统计：扫描完成时记录，避免每次过滤都重新遍历模型
    int m_lastTotalAll = 0;
    int m_lastTotalDirs = 0;
    int m_lastTotalFiles = 0;
    int m_lastScanned = 0;
    qint64 m_lastElapsed = 0;
    // 扫描器实际使用的线程数与驱动器类型（用于状态栏展示自适应决策）
    int m_lastThreadCount = 0;
    DriveKind m_lastDriveKind = DriveKind::Unknown;

    void startScan(const QString& root, qint64 timeRangeMs,
                   qint64 folderBytesThreshold, qint64 fileBytesThreshold,
                   int threadCount);
    void applyFilter();          // 应用搜索过滤器到 proxy model 并刷新状态
    void resetUiIdle();
    bool writeResults(const QString& path) const;
    void cleanupThread();        // 停止线程并释放 worker/thread 对象
};

#endif // MAINWINDOW_H
