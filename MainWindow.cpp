#include "MainWindow.h"
#include "ui_mainwindow.h"

#include "ScanWorker.h"
#include "ScanResultsModel.h"
#include "ResultsFormatter.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>
#include <QCloseEvent>
#include <QStyle>
#include <QHeaderView>
#include <QTreeView>
#include <QToolButton>
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
#include <malloc.h>      // _heapmin
#endif

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

    // 大小过滤勾选框：取消勾选时禁用对应输入控件（toggled 直接驱动 setEnabled）
    // onScan 中按勾选状态决定传入 0（不限制）还是 spinbox 值
    ui->hoursSpin->setEnabled(ui->hoursCheck->isChecked());
    ui->hoursUnit->setEnabled(ui->hoursCheck->isChecked());
    ui->folderMbSpin->setEnabled(ui->folderSizeCheck->isChecked());
    ui->folderSizeUnit->setEnabled(ui->folderSizeCheck->isChecked());
    ui->fileMbSpin->setEnabled(ui->fileSizeCheck->isChecked());
    ui->fileSizeUnit->setEnabled(ui->fileSizeCheck->isChecked());
    connect(ui->hoursCheck,      &QCheckBox::toggled, ui->hoursSpin,      &QWidget::setEnabled);
    connect(ui->hoursCheck,      &QCheckBox::toggled, ui->hoursUnit,      &QWidget::setEnabled);
    connect(ui->folderSizeCheck, &QCheckBox::toggled, ui->folderMbSpin,   &QWidget::setEnabled);
    connect(ui->folderSizeCheck, &QCheckBox::toggled, ui->folderSizeUnit, &QWidget::setEnabled);
    connect(ui->fileSizeCheck,   &QCheckBox::toggled, ui->fileMbSpin,     &QWidget::setEnabled);
    connect(ui->fileSizeCheck,   &QCheckBox::toggled, ui->fileSizeUnit,   &QWidget::setEnabled);

    // 表头列宽模式：全部可交互拖拽调整
    ui->treeView->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    ui->treeView->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    ui->treeView->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    ui->treeView->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    ui->treeView->header()->setSectionResizeMode(4, QHeaderView::Interactive);
    // 默认列宽
    ui->treeView->header()->resizeSection(0, 60);   // 类型
    ui->treeView->header()->resizeSection(1, 420);   // 路径
    ui->treeView->header()->resizeSection(2, 100);   // 大小
    ui->treeView->header()->resizeSection(3, 160);   // 修改时间
    ui->treeView->header()->resizeSection(4, 160);   // 创建时间
    ui->treeView->header()->setStretchLastSection(true);
    ui->treeView->header()->setSectionsMovable(false);

    // 自定义模型：内置过滤索引，无 QSortFilterProxyModel
    // - m_allData 持有全量 ScanItem（唯一存储）
    // - m_filteredIndices 仅存行号（4 字节/项），过滤时只重建索引
    // 相比 QTreeWidget + QTreeWidgetItem：内存降 90%+
    // 相比 QSortFilterProxyModel：内存再降 60%+，modelReset 瞬间完成
    m_model = new ScanResultsModel(this);
    ui->treeView->setModel(m_model);
    m_model->sort(2, Qt::DescendingOrder);  // 默认按大小降序（对空索引无操作）

    connect(ui->browseBtn,  &QPushButton::clicked, this, &MainWindow::onBrowse);
    connect(ui->scanBtn,    &QPushButton::clicked, this, &MainWindow::onScan);
    connect(ui->cancelBtn,  &QPushButton::clicked, this, &MainWindow::onCancel);
    connect(ui->saveBtn,    &QPushButton::clicked, this, &MainWindow::onSave);
    connect(ui->clearBtn,   &QPushButton::clicked, this, &MainWindow::onClear);
    connect(ui->treeView,   &QTreeView::doubleClicked, this, &MainWindow::onItemDoubleClicked);

    // 并发线程旁边的 "?" 按钮：点击弹出使用说明
    connect(ui->threadHelpBtn, &QToolButton::clicked, this, [this] {
        QMessageBox::information(this, tr("并发线程使用说明"),
            tr("并发线程数控制扫描时同时工作的线程数量。\n\n"
               "• 自动（默认值 0）：由程序根据磁盘类型自动选择\n"
               "    - SSD / NVMe：8~16 线程\n"
               "    - 机械硬盘 HDD：2~4 线程（避免磁头频繁寻道反而变慢）\n\n"
               "• 手动指定（1~32）：\n"
               "    - 数值越大，CPU 与磁盘并发度越高，扫描越快\n"
               "    - 但超过物理核心数后收益递减，且会增加内存占用\n"
               "    - 建议：SSD 用 8~16，HDD 用 2~4\n\n"
               "提示：扫描系统盘或大量小文件时，建议保持「自动」或适当降低线程数，\n"
               "以减少对系统响应的影响。"));
    });

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

// 时间单位换算为毫秒
//   0=分钟 1=小时 2=天 3=周 4=月 5=年（月=30 天，年=365 天，近似）
static qint64 timeToMs(double value, int unitIdx)
{
    const double MIN  = 60.0 * 1000.0;
    const double HOUR = 60.0 * MIN;
    const double DAY  = 24.0 * HOUR;
    const double WEEK = 7.0  * DAY;
    const double MON  = 30.0 * DAY;
    const double YEAR = 365.0 * DAY;
    static const double kFactors[] = { MIN, HOUR, DAY, WEEK, MON, YEAR };
    if (unitIdx < 0 || unitIdx >= int(sizeof(kFactors)/sizeof(kFactors[0])))
        unitIdx = 1;  // 默认小时
    return qint64(value * kFactors[unitIdx]);
}

// 大小单位换算为字节
//   0=B 1=KB 2=MB 3=GB 4=TB 5=PB（base=1024）
static qint64 sizeToBytes(double value, int unitIdx)
{
    static const double kFactors[] = {
        1.0,
        1024.0,
        1024.0 * 1024.0,
        1024.0 * 1024.0 * 1024.0,
        1024.0 * 1024.0 * 1024.0 * 1024.0,
        1024.0 * 1024.0 * 1024.0 * 1024.0 * 1024.0
    };
    if (unitIdx < 0 || unitIdx >= int(sizeof(kFactors)/sizeof(kFactors[0])))
        unitIdx = 2;  // 默认 MB
    return qint64(value * kFactors[unitIdx]);
}

// 驱动器类型的中文标签（用于状态栏展示自适应决策）
static QString driveKindLabel(DriveKind dk)
{
    switch (dk) {
        case DriveKind::SSD:       return QStringLiteral("SSD");
        case DriveKind::HDD:       return QStringLiteral("HDD");
        case DriveKind::Removable: return QStringLiteral("可移动盘");
        case DriveKind::Network:   return QStringLiteral("网络盘");
        case DriveKind::Unknown:
        default:                   return QStringLiteral("未知介质");
    }
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
    const qint64 timeMs       = ui->hoursCheck->isChecked()
        ? timeToMs(ui->hoursSpin->value(), ui->hoursUnit->currentIndex()) : 0;
    // 勾选时按 spinbox 值过滤；取消勾选则传 0（语义：不限制大小，列出所有文件/非空文件夹）
    const qint64 folderBytes  = ui->folderSizeCheck->isChecked()
        ? sizeToBytes(ui->folderMbSpin->value(), ui->folderSizeUnit->currentIndex()) : 0;
    const qint64 fileBytes    = ui->fileSizeCheck->isChecked()
        ? sizeToBytes(ui->fileMbSpin->value(),   ui->fileSizeUnit->currentIndex())   : 0;
    const int threadCount     = int(ui->threadSpin->value());
    startScan(root, timeMs, folderBytes, fileBytes, threadCount);
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
    if (m_model->allData().empty()) {
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
    m_model->clear();
    m_lastTotalAll = m_lastTotalDirs = m_lastTotalFiles = 0;
    ui->saveBtn->setEnabled(false);
    ui->clearBtn->setEnabled(false);
    ui->progressBar->setValue(0);
    ui->statusLabel->setText(tr("已清空结果。"));
    ui->currentLabel->clear();

#ifdef Q_OS_WIN
    // 清空 model 后立即归还工作集，避免 freed 页滞留
    _heapmin();
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
#endif
}

void MainWindow::onProgress(int scanned, int dirCount, int fileCount,
                            int matchedCount, qint64 elapsedMs, const QString& currentPath)
{
    ui->statusLabel->setText(tr("正在扫描… 已扫描 %1 项（目录 %2，文件 %3），匹配 %4 项，已用 %5 秒")
        .arg(scanned).arg(dirCount).arg(fileCount).arg(matchedCount)
        .arg(elapsedMs / 1000.0, 0, 'f', 1));
    ui->currentLabel->setText(QDir::toNativeSeparators(currentPath));
}

void MainWindow::onItemDoubleClicked(const QModelIndex& index)
{
    if (!index.isValid()) return;
    const ScanItem* item = m_model->itemForIndex(index);
    if (!item || item->path.isEmpty()) return;

#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(item->path);
    const std::wstring param = (QStringLiteral("/select,\"") + nativePath + "\"").toStdWString();
    ShellExecuteW(nullptr, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
#else
    QProcess::startDetached("xdg-open", {QFileInfo(item->path).absolutePath()});
#endif
}

void MainWindow::onFinished()
{
    if (m_worker) {
        // 取走结果所有权，避免拷贝导致百万级数据双份驻留
        // 注意：worker 线程已在 emit finished() 前完成 buildTreeIndex（含每层 size 降序）
        // + shrink_to_fit，这里 takeResults/takeTreeIndex 拿到的是已就绪、已紧缩的数据，
        // 主线程无需再做任何重活
        std::vector<ScanItem> allResults = m_worker->takeResults();
        TreeIndex treeIndex = m_worker->takeTreeIndex();
        m_lastParams   = m_worker->params();
        m_lastScanned  = m_worker->scannedCount();
        m_lastElapsed  = m_worker->elapsedMs();
        // 记录实际线程数与驱动器类型，供状态栏展示自适应决策
        m_lastThreadCount = m_worker->threadCount();
        m_lastDriveKind   = m_worker->driveKind();

        // 统计：一次性遍历，记录到成员变量，避免每次过滤都重新遍历
        m_lastTotalAll  = int(allResults.size());
        m_lastTotalDirs = int(std::count_if(allResults.begin(), allResults.end(),
                                            [](const ScanItem& s) { return s.isDir; }));
        m_lastTotalFiles = m_lastTotalAll - m_lastTotalDirs;

        // 全量数据 + 树索引 move 给模型（唯一存储，无拷贝）
        // setResultsWithTree 内部仅 O(1) move + modelReset，然后启动根级分批加载，
        // UI 瞬间显示前 BATCH_SIZE 个根级项，剩余每 1ms/批追加
        m_model->setResultsWithTree(std::move(allResults), std::move(treeIndex));
    }
    if (m_cancelled) m_failed = false;
    applyFilter();  // 应用当前搜索过滤器（首次通常为空，clearFilter 会跳过冗余重建）

    ui->currentLabel->clear();
    ui->progressBar->setRange(0, 100);
    ui->progressBar->setValue(100);

    ui->scanBtn->setEnabled(true);
    ui->cancelBtn->setEnabled(false);
    ui->saveBtn->setEnabled(m_model->rowCount(QModelIndex()) > 0);
    ui->clearBtn->setEnabled(m_model->totalCount() > 0);

    cleanupThread();  // 停止线程并释放 worker/thread 对象（worker 的 m_results 已被 take 走）

#ifdef Q_OS_WIN
    // 主线程兜底再调一次：cleanupThread delete worker 时会释放 worker 对象本身、
    // QThread 对象、以及 m_params/m_workerCtxs（虽然已 swap，但容器外壳也占少量字节）等。
    // Windows 整个进程共享 GetProcessHeap()，worker 线程 _heapmin 已归还大块，
    // 这里清掉主线程这一侧可能产生的小块残留。
    _heapmin();
    // 修复"扫描完成后内存卡在 529MB 不动"的问题：
    // 根因：扫描期间 DirEntry 节点（临时）与 ScanItem 的 QString 路径（持久，随结果
    // 移交给 model）在同一段堆空间中交错分配。DirEntry 被 swap 释放后，_heapmin 调用
    // HeapCompact 只能归还"整段全空"的段，但被持久 QString 数据穿插的段无法归还，
    // 加之 Windows 默认启用 LFH，HeapCompact 在 LFH 上的归还效果进一步减弱。
    // 因此 freed 的 DirEntry 内存虽标记为可用，仍滞留在进程工作集中。
    // SetProcessWorkingSetSize(-1,-1) 在 VM 层强制修剪工作集，绕过 LFH/HeapCompact
    // 限制，立即将这些已 free 的页移出 RAM，任务管理器显示值随之下降。
    // 不影响"迅速列出列表"：worker 已完成预排序与 shrink_to_fit，model 数据就位后
    // 才调用此函数，仅清理工作集冗余，不动 model 数据。
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
#endif
}

void MainWindow::onSearchDebounced()
{
    m_currentFilter = parseSearchFilter(ui->searchEdit->text());
    if (m_model->rowCount(QModelIndex()) == 0) return;  // 无结果时无需过滤
    applyFilter();
}

// 应用搜索过滤器到 model 并刷新状态栏
// 全量数据始终在 m_model::m_allData 中，setFilter 只重建 m_filteredIndices（零拷贝）
void MainWindow::applyFilter()
{
    if (m_currentFilter.isEmpty()) {
        m_model->clearFilter();
    } else {
        m_model->setFilter(m_currentFilter.includeTerms, m_currentFilter.excludeTerms);
    }
    ui->treeView->scrollToTop();

    const int totalAll = m_lastTotalAll;
    const int allDirs  = m_lastTotalDirs;
    const int allFiles = m_lastTotalFiles;
    const int totalFiltered = m_model->rowCount(QModelIndex());
    const double sec = m_lastElapsed / 1000.0;

    const bool filteredFlag = !m_currentFilter.isEmpty() && totalAll != totalFiltered;
    const QString filterSuffix = filteredFlag
        ? tr("（过滤后 %1 项）").arg(totalFiltered)
        : QString();

    // 运行期信息后缀：展示自适应线程数与驱动器类型（用户显式指定线程时也展示，
    // 便于对比调优；扫描未开始时 m_lastThreadCount==0 跳过）
    const QString runtimeSuffix = (m_lastThreadCount > 0)
        ? tr("（线程：%1，%2）").arg(m_lastThreadCount).arg(driveKindLabel(m_lastDriveKind))
        : QString();

    QString summary;
    if (m_failed) {
        summary = tr("扫描失败。%1").arg(runtimeSuffix);
    } else if (m_cancelled) {
        summary = tr("已取消。匹配结果：%1 项（目录 %2，文件 %3）；扫描 %4 项，耗时 %5 秒 %6%7")
            .arg(totalAll).arg(allDirs).arg(allFiles)
            .arg(m_lastScanned).arg(sec, 0, 'f', 2).arg(filterSuffix).arg(runtimeSuffix);
    } else {
        summary = tr("扫描完成。匹配结果：%1 项（目录 %2，文件 %3）；扫描 %4 项，耗时 %5 秒 %6%7")
            .arg(totalAll).arg(allDirs).arg(allFiles)
            .arg(m_lastScanned).arg(sec, 0, 'f', 2).arg(filterSuffix).arg(runtimeSuffix);
    }
    ui->statusLabel->setText(summary);

    ui->saveBtn->setEnabled(totalFiltered > 0);
    ui->clearBtn->setEnabled(totalAll > 0);
}

void MainWindow::startScan(const QString& root, qint64 timeRangeMs,
                           qint64 folderBytesThreshold, qint64 fileBytesThreshold,
                           int threadCount)
{
    m_cancelled = false;
    m_failed = false;
    m_lastTotalAll = m_lastTotalDirs = m_lastTotalFiles = 0;
    m_model->clear();
    ui->saveBtn->setEnabled(false);
    ui->clearBtn->setEnabled(false);

    m_lastParams = {root, timeRangeMs, folderBytesThreshold, fileBytesThreshold, threadCount};

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
    const QString text = formatResultsText(m_model->allData(), m_lastParams, m_lastScanned, m_lastElapsed);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    // 写入内容
    f.write(text.toUtf8());
    f.flush();
    return true;
}


