#include "MainWindow.h"
#include "ui_mainwindow.h"

#include "ScanWorker.h"
#include "ResultsFormatter.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>
#include <QCloseEvent>
#include <QStyle>
#include <QHeaderView>
#include <QTreeWidgetItem>
#include <QFile>
#include <QDateTime>
#include <QProcess>
#include <QRegularExpression>
#include <algorithm>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

// 让“大小”列按数值而非字符串排序
class ResultTreeWidgetItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem& other) const override
    {
        const QTreeWidget* tw = treeWidget();
        if (!tw) return false;
        const int col = tw->sortColumn();
        if (col == 2) {
            const qint64 a = data(2, Qt::UserRole).toLongLong();
            const qint64 b = other.data(2, Qt::UserRole).toLongLong();
            return a < b;
        }
        return text(col) < other.text(col);
    }
};

// 搜索过滤器：路径匹配 includeTerms（任一）与 excludeTerms（无一）
// 大小写不敏感（NTFS 默认大小写不敏感；中文无大小写问题）
bool SearchFilter::matches(const QString& path) const
{
    if (isEmpty()) return true;
    // 排除项：路径包含任一排除词即不匹配
    for (const QString& ex : excludeTerms) {
        if (path.contains(ex, Qt::CaseInsensitive)) return false;
    }
    // 包含项：非空时路径需包含至少一个包含词
    if (!includeTerms.isEmpty()) {
        bool found = false;
        for (const QString& in : includeTerms) {
            if (path.contains(in, Qt::CaseInsensitive)) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

// 解析搜索文本：以空白分隔，! 前缀为排除词，否则为包含词
SearchFilter parseSearchFilter(const QString& text)
{
    SearchFilter f;
    const QStringList tokens = text.split(QRegularExpression(QStringLiteral("\\s+")),
                                           Qt::SkipEmptyParts);
    for (const QString& tok : tokens) {
        if (tok.startsWith(QLatin1Char('!'))) {
            const QString t = tok.mid(1);
            if (!t.isEmpty()) f.excludeTerms.append(t);
        } else {
            f.includeTerms.append(tok);
        }
    }
    return f;
}

MainWindow::MainWindow(QWidget* parent) : QWidget(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // 当前扫描路径标签：启用自动换行，且不参与布局的最小宽度计算，避免长路径撑宽窗口
    ui->currentLabel->setWordWrap(true);
    ui->currentLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    // 表头列宽模式：全部可交互拖拽调整
    ui->treeWidget->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    ui->treeWidget->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    ui->treeWidget->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    ui->treeWidget->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    ui->treeWidget->header()->setSectionResizeMode(4, QHeaderView::Interactive);
    // 默认列宽
    ui->treeWidget->header()->resizeSection(0, 60);   // 类型
    ui->treeWidget->header()->resizeSection(1, 420);   // 路径
    ui->treeWidget->header()->resizeSection(2, 100);   // 大小
    ui->treeWidget->header()->resizeSection(3, 160);   // 修改时间
    ui->treeWidget->header()->resizeSection(4, 160);   // 创建时间
    ui->treeWidget->header()->setStretchLastSection(true);
    ui->treeWidget->header()->setSectionsMovable(false);

    connect(ui->browseBtn,  &QPushButton::clicked, this, &MainWindow::onBrowse);
    connect(ui->scanBtn,    &QPushButton::clicked, this, &MainWindow::onScan);
    connect(ui->cancelBtn,  &QPushButton::clicked, this, &MainWindow::onCancel);
    connect(ui->saveBtn,    &QPushButton::clicked, this, &MainWindow::onSave);
    connect(ui->clearBtn,   &QPushButton::clicked, this, &MainWindow::onClear);
    connect(ui->treeWidget, &QTreeWidget::itemDoubleClicked, this, &MainWindow::onItemDoubleClicked);

    // 搜索框：防抖触发，避免每键都重建树
    m_searchDebounce = new QTimer(this);
    m_searchDebounce->setSingleShot(true);
    m_searchDebounce->setInterval(150);
    connect(m_searchDebounce, &QTimer::timeout, this, &MainWindow::onSearchDebounced);
    connect(ui->searchEdit, &QLineEdit::textChanged, this, [this] {
        m_searchDebounce->start();
    });

    resetUiIdle();
}

MainWindow::~MainWindow()
{
    cleanupThread();
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    cleanupThread();
    QWidget::closeEvent(e);
}

// 停止工作线程并释放 worker/thread 对象。
// 使用显式 delete 而非 deleteLater，避免工作线程事件循环已停止时
// DeferredDelete 事件不被处理导致内存泄漏。
void MainWindow::cleanupThread()
{
    if (m_thread) {
        if (m_worker) m_worker->cancel();
        m_thread->quit();
        m_thread->wait(5000);
        delete m_worker;
        m_worker = nullptr;
        delete m_thread;
        m_thread = nullptr;
    }
}

void MainWindow::onBrowse()
{
    const QString cur = ui->dirEdit->text().trimmed();
    const QString start = cur.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
        : cur;
    const QString dir = QFileDialog::getExistingDirectory(this, tr("选择要扫描的目录"), start);
    if (!dir.isEmpty()) ui->dirEdit->setText(QDir::toNativeSeparators(dir));
}

void MainWindow::onScan()
{
    const QString root = ui->dirEdit->text().trimmed();
    if (root.isEmpty()) {
        QMessageBox::warning(this, tr("提示"), tr("请先选择要扫描的目录。"));
        return;
    }
    const QFileInfo rootInfo(root);
    if (!rootInfo.exists()) {
        QMessageBox::warning(this, tr("提示"), tr("目录不存在：\n%1").arg(root));
        return;
    }
    if (!rootInfo.isDir()) {
        QMessageBox::warning(this, tr("提示"), tr("指定的路径不是目录：\n%1").arg(root));
        return;
    }
    startScan(root, ui->hoursSpin->value(), ui->folderMbSpin->value(), ui->fileMbSpin->value());
}

void MainWindow::onCancel()
{
    m_cancelled = true;
    if (m_worker) m_worker->cancel();
    ui->cancelBtn->setEnabled(false);
    ui->statusLabel->setText(tr("正在取消…"));
}

void MainWindow::onSave()
{
    if (m_results.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可保存的结果。"));
        return;
    }
    const QString defaultName = QStringLiteral("DiskScan_%1.txt")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this, tr("保存扫描结果"), defaultName,
        tr("文本文件 (*.txt);;所有文件 (*.*)"));
    if (path.isEmpty()) return;
    if (!writeResults(path)) {
        QMessageBox::warning(this, tr("保存失败"),
            tr("无法写入文件：\n%1").arg(path));
    } else {
        ui->statusLabel->setText(tr("结果已保存到：%1").arg(path));
    }
}

void MainWindow::onClear()
{
    m_allResults.clear();
    m_results.clear();
    ui->treeWidget->clear();
    ui->saveBtn->setEnabled(false);
    ui->clearBtn->setEnabled(false);
    ui->progressBar->setValue(0);
    ui->statusLabel->setText(tr("已清空结果。"));
    ui->currentLabel->clear();
}

void MainWindow::onProgress(int scanned, int dirCount, int fileCount,
                            qint64 elapsedMs, const QString& currentPath)
{
    ui->statusLabel->setText(tr("正在扫描… 已扫描 %1 项（目录 %2，文件 %3），已用 %4 秒")
        .arg(scanned).arg(dirCount).arg(fileCount)
        .arg(elapsedMs / 1000.0, 0, 'f', 1));
    ui->currentLabel->setText(QDir::toNativeSeparators(currentPath));
}

void MainWindow::onItemDoubleClicked()
{
    QTreeWidgetItem* item = ui->treeWidget->currentItem();
    if (!item) return;
    const QString path = item->text(1);  // 路径列
    if (path.isEmpty()) return;

#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(path);
    const std::wstring param = (QStringLiteral("/select,\"") + nativePath + "\"").toStdWString();
    ShellExecuteW(nullptr, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
#else
    QProcess::startDetached("xdg-open", {QFileInfo(path).absolutePath()});
#endif
}

void MainWindow::onFinished()
{
    if (m_worker) {
        m_allResults   = m_worker->results();
        m_lastParams   = m_worker->params();
        m_lastScanned  = m_worker->scannedCount();
        m_lastElapsed  = m_worker->elapsedMs();
    }
    if (m_cancelled) m_failed = false;
    applyFilter();  // 根据当前搜索框过滤并刷新树与状态

    ui->currentLabel->clear();
    ui->progressBar->setRange(0, 100);
    ui->progressBar->setValue(100);

    ui->scanBtn->setEnabled(true);
    ui->cancelBtn->setEnabled(false);
    ui->saveBtn->setEnabled(!m_results.empty());
    ui->clearBtn->setEnabled(!m_results.empty());

    cleanupThread();  // 停止线程并释放 worker/thread 对象
}

void MainWindow::onSearchDebounced()
{
    m_currentFilter = parseSearchFilter(ui->searchEdit->text());
    if (m_allResults.empty()) return;  // 无结果时无需过滤
    applyFilter();
}

// 根据当前搜索过滤器从 m_allResults 过滤到 m_results，刷新树与状态栏
void MainWindow::applyFilter()
{
    m_results.clear();
    if (m_currentFilter.isEmpty()) {
        m_results = m_allResults;
    } else {
        m_results.reserve(m_allResults.size());
        for (const ScanItem& s : m_allResults) {
            if (m_currentFilter.matches(s.path)) {
                m_results.push_back(s);
            }
        }
    }
    populateTree();

    const int totalAll = int(m_allResults.size());
    const int allDirs = int(std::count_if(m_allResults.begin(), m_allResults.end(),
                                          [](const ScanItem& s) { return s.isDir; }));
    const int allFiles = totalAll - allDirs;
    const double sec = m_lastElapsed / 1000.0;

    const bool filtered = !m_currentFilter.isEmpty() && totalAll != int(m_results.size());
    const QString filterSuffix = filtered
        ? tr("（过滤后 %1 项）").arg(int(m_results.size()))
        : QString();

    QString summary;
    if (m_failed) {
        summary = tr("扫描失败。");
    } else if (m_cancelled) {
        summary = tr("已取消。匹配结果：%1 项（目录 %2，文件 %3）；扫描 %4 项，耗时 %5 秒 %6")
            .arg(totalAll).arg(allDirs).arg(allFiles)
            .arg(m_lastScanned).arg(sec, 0, 'f', 2).arg(filterSuffix);
    } else {
        summary = tr("扫描完成。匹配结果：%1 项（目录 %2，文件 %3）；扫描 %4 项，耗时 %5 秒 %6")
            .arg(totalAll).arg(allDirs).arg(allFiles)
            .arg(m_lastScanned).arg(sec, 0, 'f', 2).arg(filterSuffix);
    }
    ui->statusLabel->setText(summary);

    ui->saveBtn->setEnabled(!m_results.empty());
    ui->clearBtn->setEnabled(!m_allResults.empty());
}

void MainWindow::startScan(const QString& root, double hours, double folderMb, double fileMb)
{
    m_cancelled = false;
    m_failed = false;
    m_allResults.clear();
    m_results.clear();
    ui->treeWidget->clear();
    ui->saveBtn->setEnabled(false);
    ui->clearBtn->setEnabled(false);

    m_lastParams = {root, hours, folderMb, fileMb};

    ui->scanBtn->setEnabled(false);
    ui->cancelBtn->setEnabled(true);
    ui->progressBar->setRange(0, 0);  // 忙碌指示
    ui->statusLabel->setText(tr("正在扫描…"));
    ui->currentLabel->setText(QDir::toNativeSeparators(root));

    m_thread = new QThread(this);
    m_worker = new ScanWorker(m_lastParams);
    m_worker->moveToThread(m_thread);

    connect(m_thread, &QThread::started, m_worker, &ScanWorker::doScan);
    connect(m_worker, &ScanWorker::progress, this, &MainWindow::onProgress);
    connect(m_worker, &ScanWorker::failed, this, [this](const QString& msg) {
        m_failed = true;
        QMessageBox::warning(this, tr("扫描失败"), msg);
    });
    connect(m_worker, &ScanWorker::finished, this, &MainWindow::onFinished, Qt::QueuedConnection);
    // 不使用 deleteLater 连接：工作线程事件循环停止后 deleteLater 不会被处理，导致泄漏。
    // 线程清理在 onFinished / closeEvent / ~MainWindow 中通过 cleanupThread() 显式完成。
    m_thread->start();
}

void MainWindow::populateTree()
{
    ui->treeWidget->setSortingEnabled(false);
    ui->treeWidget->setUpdatesEnabled(false);
    ui->treeWidget->clear();
    const QIcon dirIcon  = style()->standardIcon(QStyle::SP_DirIcon);
    const QIcon fileIcon = style()->standardIcon(QStyle::SP_FileIcon);
    QList<QTreeWidgetItem*> items;
    items.reserve(int(m_results.size()));
    for (const ScanItem& s : m_results) {
        const QString typeStr = s.isDir ? tr("目录") : tr("文件");
        const QString sizeStr = formatSize(s.size);
        const QString mtStr = s.mtimeMs > 0
            ? QDateTime::fromMSecsSinceEpoch(s.mtimeMs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QStringLiteral("-");
        const QString ctStr = s.ctimeMs > 0
            ? QDateTime::fromMSecsSinceEpoch(s.ctimeMs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QStringLiteral("-");
        auto* it = new ResultTreeWidgetItem({typeStr, s.path, sizeStr, mtStr, ctStr});
        it->setIcon(0, s.isDir ? dirIcon : fileIcon);
        it->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        it->setData(2, Qt::UserRole, s.size);  // 供数值排序使用
        items.append(it);
    }
    ui->treeWidget->addTopLevelItems(items);
    ui->treeWidget->setUpdatesEnabled(true);
    ui->treeWidget->setSortingEnabled(true);
    ui->treeWidget->sortByColumn(2, Qt::DescendingOrder);  // 默认按大小降序
}

void MainWindow::resetUiIdle()
{
    ui->scanBtn->setEnabled(true);
    ui->cancelBtn->setEnabled(false);
    ui->saveBtn->setEnabled(false);
    ui->clearBtn->setEnabled(false);
    ui->progressBar->setRange(0, 100);
    ui->progressBar->setValue(0);
    ui->statusLabel->setText(tr("就绪。请选择目录并设置参数后点击“开始扫描”。"));
    ui->currentLabel->clear();
}

bool MainWindow::writeResults(const QString& path) const
{
    const QString text = formatResultsText(m_results, m_lastParams, m_lastScanned, m_lastElapsed);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    // 写入内容
    f.write(text.toUtf8());
    f.flush();
    return true;
}
