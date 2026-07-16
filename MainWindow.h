#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QWidget>
#include <QThread>
#include <QTimer>
#include <QStringList>
#include <vector>

#include "ScanTypes.h"

class ScanWorker;

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
    void onProgress(int scanned, int dirCount, int fileCount,
                    qint64 elapsedMs, const QString& currentPath);
    void onFinished();
    void onItemDoubleClicked();
    void onSearchDebounced();  // 搜索框输入防抖触发

private:
    Ui::MainWindow* ui = nullptr;
    QThread* m_thread = nullptr;
    ScanWorker* m_worker = nullptr;
    bool m_cancelled = false;
    bool m_failed = false;

    ScanParams m_lastParams;
    // 全部扫描结果（未经搜索过滤）
    std::vector<ScanItem> m_allResults;
    // 当前显示结果（经搜索过滤）
    std::vector<ScanItem> m_results;
    SearchFilter m_currentFilter;
    QTimer* m_searchDebounce = nullptr;

    int m_lastScanned = 0;
    qint64 m_lastElapsed = 0;

    void startScan(const QString& root, double hours, double folderMb, double fileMb);
    void populateTree();
    void applyFilter();          // 根据当前过滤器从 m_allResults 过滤到 m_results 并刷新树
    void resetUiIdle();
    bool writeResults(const QString& path) const;
    void cleanupThread();        // 停止线程并释放 worker/thread 对象
};

#endif // MAINWINDOW_H
