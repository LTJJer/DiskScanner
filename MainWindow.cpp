#include "MainWindow.h"
#include "ui_mainwindow.h"

#include "ScanWorker.h"
#include "ScanResultsModel.h"
#include "ResultsFormatter.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QStyle>
#include <QHeaderView>
#include <QTreeView>
#include <QToolButton>
#include <QFile>
#include <QDateTime>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QKeyEvent>
#include <QSet>
#include <QSettings>
#include <QDir>
#include <QtConcurrent>
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
    // 列顺序：0 名称  1 大小  2 修改日期  3 创建日期  4 所在目录  5 类型
    ui->treeView->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    ui->treeView->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    ui->treeView->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    ui->treeView->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    ui->treeView->header()->setSectionResizeMode(4, QHeaderView::Interactive);
    ui->treeView->header()->setSectionResizeMode(5, QHeaderView::Interactive);
    // 默认列宽按 5:2:3:3:4:2 比例分配（名称:大小:修改日期:创建日期:所在目录:类型）
    // 构造时尚未布局，viewport 宽度可能为 0，使用延迟调用在首展后按比例分配
    ui->treeView->header()->setStretchLastSection(false);
    // 列可拖动改顺序；名称列固定在最左，拖动后会被拉回原位
    ui->treeView->header()->setSectionsMovable(true);
    // 最后一列不自动拉伸，避免与 Interactive 冲突导致列宽跳动
    connect(ui->treeView->header(), &QHeaderView::sectionMoved, this, [this](int, int from, int to) {
        QHeaderView* h = ui->treeView->header();
        // 名称列（section 0）必须保持 visualIndex = 0
        const int visualOfName = h->visualIndex(0);
        if (visualOfName != 0) {
            // 阻断递归：临时屏蔽信号
            h->blockSignals(true);
            h->moveSection(visualOfName, 0);
            h->blockSignals(false);
        }
        Q_UNUSED(from); Q_UNUSED(to);
    });
    // 延迟到首次显示后按比例设置列宽（此时 viewport 已有实际宽度）
    // loadSettings 在此 singleShot 注册之后调用，会填充 m_savedColumnWidths/Order。
    // 由于 Qt 同间隔 singleShot 按 FIFO 顺序触发，本 lambda 执行时已能读到配置。
    QTimer::singleShot(0, this, [this]() {
        QHeaderView* h = ui->treeView->header();
        if (!m_savedColumnWidths.empty()) {
            // 应用持久化的列宽与列顺序
            for (size_t i = 0; i < m_savedColumnWidths.size() && i < 6; ++i) {
                h->resizeSection(int(i), m_savedColumnWidths[i]);
            }
            // 应用列顺序：将保存的 visualOrder 还原
            // savedColumnOrder[v] = 应位于视觉位置 v 的逻辑索引
            if (m_savedColumnOrder.size() >= 6) {
                // 名称列（logical 0）固定在最左，跳过视觉位置 0
                for (int v = 1; v < 6; ++v) {
                    const int targetLogical = m_savedColumnOrder[size_t(v)];
                    const int curVisual = h->visualIndex(targetLogical);
                    if (curVisual >= 0 && curVisual != v) {
                        h->blockSignals(true);
                        h->moveSection(curVisual, v);
                        h->blockSignals(false);
                    }
                }
            }
        } else {
            // 无配置：默认比例 5:2:3:3:4:2 = 19 份
            const int total = ui->treeView->viewport()->width();
            if (total <= 0) return;
            static constexpr int ratios[] = {5, 2, 3, 3, 4, 2};
            static constexpr int sum = 19;
            int allocated = 0;
            for (int i = 0; i < 6; ++i) {
                if (i == 5) {
                    // 最后一列取剩余，避免取整误差导致总和 ≠ viewport 宽度
                    h->resizeSection(i, total - allocated);
                } else {
                    const int w = total * ratios[i] / sum;
                    h->resizeSection(i, w);
                    allocated += w;
                }
            }
        }
    });

    // 自定义模型：内置过滤索引，无 QSortFilterProxyModel
    // - m_allData 持有全量 ScanItem（唯一存储）
    // - m_filteredIndices 仅存行号（4 字节/项），过滤时只重建索引
    // 相比 QTreeWidget + QTreeWidgetItem：内存降 90%+
    // 相比 QSortFilterProxyModel：内存再降 60%+，modelReset 瞬间完成
    m_model = new ScanResultsModel(this);
    ui->treeView->setModel(m_model);
    // 默认无排序 → 树形模式（model 初始 m_sortColumn=-1）
    // 禁用视图内置的 2 态排序，改用 sectionClicked 实现 3 态循环
    ui->treeView->setSortingEnabled(false);
    ui->treeView->header()->setSectionsClickable(true);
    // 排序指示器（升降序箭头）默认显示；setSortIndicator 会自动启用
    ui->treeView->header()->setSortIndicatorShown(true);
    connect(ui->treeView->header(), &QHeaderView::sectionClicked,
            this, &MainWindow::onHeaderClicked);

    connect(ui->browseBtn,  &QPushButton::clicked, this, &MainWindow::onBrowse);
    connect(ui->scanBtn,    &QPushButton::clicked, this, &MainWindow::onScan);
    connect(ui->cancelBtn,  &QPushButton::clicked, this, &MainWindow::onCancel);
    connect(ui->saveBtn,    &QPushButton::clicked, this, &MainWindow::onSave);
    connect(ui->clearBtn,   &QPushButton::clicked, this, &MainWindow::onClear);
    connect(ui->treeView,   &QTreeView::doubleClicked, this, &MainWindow::onItemDoubleClicked);

    // 树视图安装事件过滤器：捕获 Del 键删除选中项到回收站
    ui->treeView->installEventFilter(this);

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

    // 多线程搜索：QFutureWatcher 监听后台计算结果
    m_searchWatcher = new QFutureWatcher<std::vector<int>>(this);
    connect(m_searchWatcher, &QFutureWatcher<std::vector<int>>::finished,
            this, &MainWindow::onSearchFinished);

    // 多线程保存：QFutureWatcher 监听后台写文件结果
    m_saveWatcher = new QFutureWatcher<bool>(this);
    connect(m_saveWatcher, &QFutureWatcher<bool>::finished,
            this, &MainWindow::onSaveFinished);

    // 时间/大小过滤 UI 变化时，重新应用结果过滤（不重新扫描）
    // 这些控件在扫描完成后才生效；扫描进行中 onResultFilterChanged 会跳过
    connect(ui->hoursCheck,      &QCheckBox::toggled, this, &MainWindow::onResultFilterChanged);
    connect(ui->hoursSpin,       qOverload<double>(&QDoubleSpinBox::valueChanged), this, &MainWindow::onResultFilterChanged);
    connect(ui->hoursUnit,       qOverload<int>(&QComboBox::currentIndexChanged), this, &MainWindow::onResultFilterChanged);
    connect(ui->folderSizeCheck, &QCheckBox::toggled, this, &MainWindow::onResultFilterChanged);
    connect(ui->folderMbSpin,    qOverload<double>(&QDoubleSpinBox::valueChanged), this, &MainWindow::onResultFilterChanged);
    connect(ui->folderSizeUnit,  qOverload<int>(&QComboBox::currentIndexChanged), this, &MainWindow::onResultFilterChanged);
    connect(ui->fileSizeCheck,   &QCheckBox::toggled, this, &MainWindow::onResultFilterChanged);
    connect(ui->fileMbSpin,      qOverload<double>(&QDoubleSpinBox::valueChanged), this, &MainWindow::onResultFilterChanged);
    connect(ui->fileSizeUnit,    qOverload<int>(&QComboBox::currentIndexChanged), this, &MainWindow::onResultFilterChanged);

    resetUiIdle();

    // 加载持久化配置（扫描目录、过滤值、线程数、保存目录、列顺序与宽度）
    loadSettings();
}

MainWindow::~MainWindow()
{
    cleanupThread();
    // 等待后台保存完成：保存线程持有指向 m_allData 的指针，
    // 必须在析构前完成，避免悬空引用
    if (m_saveWatcher && m_saveWatcher->isRunning()) {
        m_saveWatcher->waitForFinished();
    }
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    // 等待后台保存完成：保存线程持有指向 m_allData 的指针，
    // 必须在关闭前完成，避免悬空引用
    if (m_saveWatcher && m_saveWatcher->isRunning()) {
        ui->statusLabel->setText(tr("正在等待保存完成…"));
        m_saveWatcher->waitForFinished();
    }
    saveSettings();
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
    // 取消后台搜索（若正在进行）：结果将被丢弃
    if (m_searchWatcher && m_searchWatcher->isRunning()) {
        ++m_searchGeneration;  // 使进行中的结果过期
        m_searchWatcher->cancel();
        m_searchWatcher->waitForFinished();
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
    // 保存进行中：禁止启动新扫描，避免修改 m_allData 导致保存线程悬空引用
    if (m_saveInProgress) return;
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
    // 时间/大小过滤已移到结果层：扫描时全部传 0（列出所有项），onFinished 后再应用
    const qint64 timeMs       = 0;
    const qint64 folderBytes  = 0;
    const qint64 fileBytes    = 0;
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
    // 保存进行中：避免并发保存导致文件损坏或指针竞争
    if (m_saveInProgress) return;

    if (m_model->allData().empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可保存的结果。"));
        return;
    }
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString defaultName = QStringLiteral("DiskScan_%1.txt").arg(stamp);
    // 默认目录：上次保存目录（持久化），否则用 home 目录
    const QString defaultDir = m_lastSaveDir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
        : m_lastSaveDir;
    const QString defaultPath = QDir(defaultDir).absoluteFilePath(defaultName);
    // 多格式保存：通过 selectedFilter 判断用户选择的格式
    const QString filter = tr("文本文件 (*.txt);;Markdown 表格 (*.md);;CSV 表格 (*.csv);;所有文件 (*.*)");
    QString selectedFilter = tr("文本文件 (*.txt)");
    const QString path = QFileDialog::getSaveFileName(
        this, tr("保存扫描结果"), defaultPath, filter, &selectedFilter);
    if (path.isEmpty()) return;

    // 根据用户选择的过滤器决定保存格式
    SaveFormat fmt = SaveFormat::Text;
    if (selectedFilter.startsWith(QStringLiteral("Markdown"))) {
        fmt = SaveFormat::Markdown;
    } else if (selectedFilter.startsWith(QStringLiteral("CSV"))) {
        fmt = SaveFormat::CSV;
    }

    launchBackgroundSave(path, fmt);
}

// 切换保存期间的 UI 状态：禁用可能修改 m_allData 的操作
// 保存线程持有指向 m_allData 的指针，期间不能 扫描/清空/删除
void MainWindow::setSaveInProgress(bool inProgress)
{
    m_saveInProgress = inProgress;
    ui->saveBtn->setEnabled(!inProgress);
    ui->scanBtn->setEnabled(!inProgress && !m_thread);
    ui->cancelBtn->setEnabled(!inProgress && m_thread);
    ui->clearBtn->setEnabled(!inProgress && m_lastTotalAll > 0);
    // 删除操作通过 Del 键触发，事件过滤器中通过 m_saveInProgress 跳过
}

// 启动后台保存：在工作线程中格式化 + 写文件
// 主线程负责收集所有需要的数据（按值或只读指针传入），后台线程不访问 UI 与模型状态
// 保存期间禁用 扫描/清空/删除，保证 m_allData 不被修改 → 指针有效
void MainWindow::launchBackgroundSave(const QString& path, SaveFormat fmt)
{
    // 收集可见项（指针指向 m_allData，保存期间 m_allData 不变 → 指针有效）
    std::vector<const ScanItem*> visible = m_model->filteredData();
    if (visible.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可保存的结果。"));
        return;
    }

    // 读取界面列顺序（仅 0~5，与结果展示框一致）
    std::vector<int> columnOrder;
    columnOrder.reserve(6);
    QHeaderView* header = ui->treeView->header();
    for (int visual = 0; visual < header->count(); ++visual) {
        const int logical = header->logicalIndex(visual);
        if (logical >= 0 && logical <= 5) {
            columnOrder.push_back(logical);
        }
    }
    // 校验：必须包含全部 6 列
    if (columnOrder.size() != 6) {
        columnOrder = {0, 1, 2, 3, 4, 5};
    }

    // 获取类型缓存快照（避免后台线程调用 SHGetFileInfo）
    TypeCache typeCache = m_model->typeCacheSnapshot();

    // 拷贝扫描参数与统计（避免后台线程访问主线程成员）
    const ScanParams params = m_lastParams;
    const int scannedCount = m_lastScanned;
    const qint64 elapsedMs = m_lastElapsed;

    // 切换 UI 到保存中状态
    setSaveInProgress(true);
    m_currentSavePath = path;
    ui->statusLabel->setText(tr("正在保存…"));

    // 启动后台任务：捕获所需数据，在工作线程中执行格式化 + 写文件
    // visible 是 vector<const ScanItem*>，按值捕获（指针指向 m_allData，期间不变）
    QFuture<bool> future = QtConcurrent::run(
        [path, fmt, visible = std::move(visible), columnOrder = std::move(columnOrder),
         typeCache = std::move(typeCache), params, scannedCount, elapsedMs]() -> bool {
            QFile f(path);
            // QIODevice::Text：在 Windows 上写入时将 \n 转换为 \r\n（CRLF）
            // UTF-8 编码由 writeResultsTo 内部的 QTextStream.setEncoding(Utf8) 保证
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;
            const bool ok = writeResultsTo(&f, fmt, visible, params,
                                            scannedCount, elapsedMs,
                                            columnOrder, &typeCache);
            f.close();
            return ok;
        });
    m_saveWatcher->setFuture(future);
}

// 后台保存完成的回调（主线程）
void MainWindow::onSaveFinished()
{
    const bool ok = m_saveWatcher->result();
    const QString path = m_currentSavePath;
    m_currentSavePath.clear();

    setSaveInProgress(false);

    if (!ok) {
        QMessageBox::warning(this, tr("保存失败"),
            tr("无法写入文件：\n%1").arg(path));
        ui->statusLabel->setText(tr("保存失败"));
    } else {
        // 记录本次保存目录，下次保存时作为默认目录
        m_lastSaveDir = QDir::toNativeSeparators(QFileInfo(path).absolutePath());
        ui->statusLabel->setText(tr("结果已保存到：%1").arg(path));
    }
    refreshStatusBar();
}

void MainWindow::onClear()
{
    // 保存进行中：禁止清空，避免修改 m_allData 导致保存线程悬空引用
    if (m_saveInProgress) return;
    // 取消后台搜索（若正在进行）
    if (m_searchWatcher && m_searchWatcher->isRunning()) {
        ++m_searchGeneration;
        m_searchWatcher->cancel();
        m_searchWatcher->waitForFinished();
    }
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
                            int matchedCount, qint64 elapsedMs, const QString& currentPath,
                            qint64 scannedBytes, qint64 totalBytes)
{
    ui->statusLabel->setText(tr("正在扫描… 已扫描 %1 项（目录 %2，文件 %3），匹配 %4 项，已用 %5 秒")
        .arg(scanned).arg(dirCount).arg(fileCount).arg(matchedCount)
        .arg(elapsedMs / 1000.0, 0, 'f', 1));
    ui->currentLabel->setText(QDir::toNativeSeparators(currentPath));

    // 整盘扫描：根据已扫描字节与磁盘已用字节显示确定进度
    // 非整盘扫描（totalBytes == 0）：保持忙碌指示（setRange(0,0)），不修改进度条
    if (totalBytes > 0) {
        // scannedBytes 仅含文件大小，不含目录元数据，最终不会精确到 100%
        // 上限钳制到 99%，留出 100% 给 onFinished 设置
        const int percent = int(scannedBytes * 100 / totalBytes);
        ui->progressBar->setRange(0, 100);
        ui->progressBar->setValue(std::clamp(percent, 0, 99));
    }
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

// 事件过滤器：捕获树视图的 Del 键，触发删除到回收站
bool MainWindow::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == ui->treeView && e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_Delete) {
            onDeleteSelected();
            return true;  // 事件已处理，阻止 QTreeView 默认行为
        }
    }
    return QWidget::eventFilter(obj, e);
}

#ifdef Q_OS_WIN
// 将多个路径移至回收站（不永久删除）
// 使用 SHFileOperationW + FOF_ALLOWUNDO 实现回收站语义
static bool deleteToRecycleBin(const QStringList& paths)
{
    if (paths.isEmpty()) return false;

    // SHFileOperationW 需要双 null 结尾的路径列表
    std::wstring packed;
    packed.reserve(size_t(paths.size() * 16));
    for (const QString& p : paths) {
        packed += QDir::toNativeSeparators(p).toStdWString();
        packed += L'\0';
    }
    packed += L'\0';  // 额外 null 结尾

    SHFILEOPSTRUCTW op = {};
    op.hwnd = nullptr;
    op.wFunc = FO_DELETE;
    op.pFrom = packed.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    op.fAnyOperationsAborted = FALSE;

    return (SHFileOperationW(&op) == 0) && !op.fAnyOperationsAborted;
}
#endif

void MainWindow::onDeleteSelected()
{
    // 扫描进行中禁用删除
    if (m_thread) return;
    // 保存进行中禁用删除：保存线程持有指向 m_allData 的指针
    if (m_saveInProgress) return;

    const QModelIndexList selected = ui->treeView->selectionModel()->selectedRows(0);
    if (selected.isEmpty()) return;

    // 收集待删除路径（去重）
    QSet<QString> pathsSet;
    for (const QModelIndex& idx : selected) {
        const ScanItem* item = m_model->itemForIndex(idx);
        if (item && !item->path.isEmpty() && !pathsSet.contains(item->path)) {
            pathsSet.insert(item->path);
        }
    }
    if (pathsSet.isEmpty()) return;

    // 用户要求：移入回收站无需确认对话框，直接执行
    const QStringList pathsList = pathsSet.values();

#ifdef Q_OS_WIN
    deleteToRecycleBin(pathsList);

    // 检查哪些路径实际已被删除（处理部分失败的情况）
    QSet<QString> actuallyDeleted;
    actuallyDeleted.reserve(pathsSet.size());
    for (const QString& p : pathsList) {
        if (!QFileInfo::exists(p)) {
            actuallyDeleted.insert(p);
        }
    }

    if (actuallyDeleted.isEmpty()) {
        QMessageBox::warning(this, tr("删除失败"),
            tr("无法将选中项移至回收站，可能已被占用或权限不足。"));
        return;
    }

    // 取消后台搜索并等待完成：removePaths 会修改 m_allData，
    // 避免后台线程悬空引用崩溃
    if (m_searchWatcher && m_searchWatcher->isRunning()) {
        ++m_searchGeneration;
        m_searchWatcher->cancel();
        m_searchWatcher->waitForFinished();
    }

    // 从模型中移除已删除的项
    m_model->removePaths(actuallyDeleted);

    // 更新统计与按钮状态
    m_lastTotalAll = m_model->totalCount();
    if (m_lastTotalAll == 0) {
        m_lastTotalDirs = m_lastTotalFiles = 0;
        ui->saveBtn->setEnabled(false);
        ui->clearBtn->setEnabled(false);
    } else {
        // 重新统计目录/文件数
        m_lastTotalDirs = int(std::count_if(m_model->allData().begin(), m_model->allData().end(),
                                            [](const ScanItem& s) { return s.isDir; }));
        m_lastTotalFiles = m_lastTotalAll - m_lastTotalDirs;
    }

    const int deletedCount = int(actuallyDeleted.size());
    const int failedCount = int(pathsSet.size()) - deletedCount;
    if (failedCount > 0) {
        ui->statusLabel->setText(
            tr("已将 %1 项移至回收站，%2 项删除失败。").arg(deletedCount).arg(failedCount));
    } else {
        ui->statusLabel->setText(tr("已将 %1 项移至回收站。").arg(deletedCount));
    }

    // 归还工作集
    _heapmin();
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
#else
    QMessageBox::warning(this, tr("不支持"),
        tr("删除到回收站功能仅在 Windows 上可用。"));
#endif
}

// 表头点击：3 态排序循环
//   同列点击：升序 → 降序 → 无排序 → 升序 → ...
//   切换列：从升序开始
void MainWindow::onHeaderClicked(int column)
{
    QHeaderView* h = ui->treeView->header();
    const int curColumn = m_model->sortColumn();
    const Qt::SortOrder curOrder = m_model->sortOrder();

    int nextColumn;
    Qt::SortOrder nextOrder;

    if (column != curColumn) {
        // 切换列：从升序开始
        nextColumn = column;
        nextOrder = Qt::AscendingOrder;
    } else {
        // 同列循环：升序 → 降序 → 无排序 → 升序
        if (curOrder == Qt::AscendingOrder) {
            nextColumn = column;
            nextOrder = Qt::DescendingOrder;
        } else if (curOrder == Qt::DescendingOrder) {
            nextColumn = -1;  // 无排序
            nextOrder = Qt::AscendingOrder;  // 占位，无排序时不使用
        } else {
            nextColumn = column;
            nextOrder = Qt::AscendingOrder;
        }
    }

    // 应用排序（model.sort 内部处理扁平/树形切换）
    m_model->sort(nextColumn, nextOrder);

    // 更新表头排序指示器（升降序箭头）
    // 必须显式调用 setSortIndicatorShown(true)，否则在某些 QStyle 下不显示箭头
    h->setSortIndicatorShown(true);
    if (nextColumn < 0) {
        // 无排序：隐藏指示器
        h->setSortIndicator(-1, Qt::AscendingOrder);
        h->setSortIndicatorShown(false);
    } else {
        h->setSortIndicator(nextColumn, nextOrder);
    }
}

// Enter 键：无论焦点在窗口何处，触发开始扫描（扫描进行中或保存中不触发）
void MainWindow::keyPressEvent(QKeyEvent* e)
{
    if ((e->key() == Qt::Key_Enter || e->key() == Qt::Key_Return) && !m_thread && !m_saveInProgress) {
        onScan();
        return;
    }
    QWidget::keyPressEvent(e);
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
        // 保存扫描元信息（用于状态栏与保存文件头）
        // 注意：worker 收到 timeRangeMs/folderBytes/fileBytes = 0（扫描时不过滤），
        // 这里替换为 UI 当前设置的过滤参数，供保存文件头显示用户实际选择的阈值
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
        // 新扫描结果：重置为无排序（树形模式），清除表头排序指示器
        ui->treeView->header()->setSortIndicator(-1, Qt::AscendingOrder);
        ui->treeView->header()->setSortIndicatorShown(false);

        // 同步 m_lastParams 元信息（root/threadCount 来自 worker，过滤参数来自 UI）
        m_lastParams.root = m_worker->params().root;
        m_lastParams.threadCount = m_worker->params().threadCount;

        // 多线程预热文件类型缓存：后台线程并行调用 SHGetFileInfoW 解析所有唯一
        // 扩展名的系统类型名（如"文本文档"），避免首次显示时主线程卡顿
        // 图标在主线程按需懒解析（QFileIconProvider 非线程安全）
        m_model->prewarmCacheAsync();

        // 应用结果过滤（时间 + 大小 + 搜索词）：统一通过后台线程计算，避免主线程卡顿
        // launchBackgroundFilter 内部会调用 setResultFilterSilent + QtConcurrent::run
        m_currentFilter = parseSearchFilter(ui->searchEdit->text());
    }
    if (m_cancelled) m_failed = false;

    // 启动后台过滤（即使无搜索词，也会应用时间/大小过滤）
    // 后台完成后 onSearchFinished 会刷新状态栏与按钮状态
    if (m_model->totalCount() > 0) {
        launchBackgroundFilter();
    } else {
        // 无结果：直接刷新 UI
        ui->treeView->scrollToTop();
        refreshStatusBar();
    }

    ui->currentLabel->clear();
    ui->progressBar->setRange(0, 100);
    ui->progressBar->setValue(100);

    ui->scanBtn->setEnabled(true);
    ui->cancelBtn->setEnabled(false);
    // saveBtn/clearBtn 的启用状态由 onSearchFinished 或 refreshStatusBar 设置

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

// 刷新状态栏（无过滤操作，仅显示当前统计）
void MainWindow::refreshStatusBar()
{
    const int totalAll = m_lastTotalAll;
    const int allDirs  = m_lastTotalDirs;
    const int allFiles = m_lastTotalFiles;
    const int totalFiltered = m_model->rowCount(QModelIndex());
    const double sec = m_lastElapsed / 1000.0;

    qint64 cutoffMs = 0, folderBytes = 0, fileBytes = 0;
    readResultFilterParams(cutoffMs, folderBytes, fileBytes);
    const bool hasFilter = !m_currentFilter.isEmpty()
                          || cutoffMs > 0 || folderBytes > 0 || fileBytes > 0;
    const bool filteredFlag = hasFilter && totalAll != totalFiltered;
    const QString filterSuffix = filteredFlag
        ? tr("（过滤后 %1 项）").arg(totalFiltered)
        : QString();

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

void MainWindow::onSearchDebounced()
{
    m_currentFilter = parseSearchFilter(ui->searchEdit->text());
    if (m_model->totalCount() == 0) return;  // 无结果时无需过滤
    // 多线程搜索：后台计算过滤索引，避免主线程卡顿
    launchBackgroundFilter();
}

// 后台线程计算过滤索引（搜索 + 时间 + 大小）
// 使用 QtConcurrent::run + QFutureWatcher，避免阻塞主线程
// 通过 m_searchGeneration 丢弃过期结果：用户输入新内容时旧任务结果被忽略
void MainWindow::launchBackgroundFilter()
{
    if (m_model->totalCount() == 0) return;

    // 取消对旧结果的关注（不中断后台计算，但结果会被丢弃）
    ++m_searchGeneration;
    const int gen = m_searchGeneration;

    // 快照过滤参数（避免后台线程访问 UI 控件）
    QStringList include = m_currentFilter.includeTerms;
    QStringList exclude = m_currentFilter.excludeTerms;
    qint64 cutoffMs = 0, folderBytes = 0, fileBytes = 0;
    readResultFilterParams(cutoffMs, folderBytes, fileBytes);

    // 静默更新模型内部阈值（不触发重建）：后台 computeFilteredIndices 会用这些值
    m_model->setResultFilterSilent(cutoffMs, folderBytes, fileBytes);
    // 同步 m_lastParams 用于保存文件头
    m_lastParams.timeRangeMs = ui->hoursCheck->isChecked()
        ? timeToMs(ui->hoursSpin->value(), ui->hoursUnit->currentIndex()) : 0;
    m_lastParams.folderBytesThreshold = folderBytes;
    m_lastParams.fileBytesThreshold = fileBytes;

    const bool hasSearch = !include.isEmpty() || !exclude.isEmpty();
    const bool hasSort = (m_model->sortColumn() >= 0);

    if (hasSearch || hasSort) {
        // 扁平模式（有搜索或排序）：后台计算过滤索引
        ScanResultsModel* model = m_model;
        QFuture<std::vector<int>> future = QtConcurrent::run(
            [model, include, exclude, cutoffMs, folderBytes, fileBytes]() {
                return model->computeFilteredIndices(include, exclude,
                                                      cutoffMs, folderBytes, fileBytes);
            });
        m_searchWatcher->setFuture(future);
    } else {
        // 树形模式（无搜索且无排序）：直接切换，应用树形过滤或恢复
        // setFilteredIndices({}, false) 会切回树形并按时间/大小过滤决定是否 applyTreeFilter
        m_model->setFilteredIndices({}, false);
        ui->treeView->scrollToTop();
        refreshStatusBar();
    }
    Q_UNUSED(gen);
}

// 后台搜索完成：注入索引到模型
void MainWindow::onSearchFinished()
{
    if (m_searchGeneration == 0) return;
    std::vector<int> indices = m_searchWatcher->result();

    // hasSearch 参数：只有搜索才触发扁平（时间/大小过滤在树形模式下处理）
    // 但如果有排序，setFilteredIndices 内部会根据 m_sortColumn 决定扁平
    const bool hasSearch = !m_currentFilter.isEmpty();
    m_model->setFilteredIndices(std::move(indices), hasSearch);
    ui->treeView->scrollToTop();
    refreshStatusBar();
}

// 时间/大小过滤 UI 变化：重新应用结果过滤
// 扫描进行中、保存中或无数据时跳过
void MainWindow::onResultFilterChanged()
{
    if (m_thread) return;  // 扫描中
    if (m_saveInProgress) return;  // 保存中：不重建过滤，避免影响保存的可见项集合
    if (m_model->totalCount() == 0) return;  // 无数据
    // 同步搜索框内容（避免丢失）
    m_currentFilter = parseSearchFilter(ui->searchEdit->text());
    launchBackgroundFilter();
}

// 从 UI 读取当前时间/大小过滤参数
//   cutoffMs：截止时刻（Unix 毫秒），0=不过滤
//   folderBytes/fileBytes：字节阈值，0=不过滤
void MainWindow::readResultFilterParams(qint64& cutoffMs, qint64& folderBytes, qint64& fileBytes) const
{
    if (ui->hoursCheck->isChecked()) {
        const qint64 rangeMs = timeToMs(ui->hoursSpin->value(), ui->hoursUnit->currentIndex());
        if (rangeMs > 0) {
            cutoffMs = QDateTime::currentDateTime().addMSecs(-rangeMs).toMSecsSinceEpoch();
        } else {
            cutoffMs = 0;
        }
    } else {
        cutoffMs = 0;
    }
    folderBytes = ui->folderSizeCheck->isChecked()
        ? sizeToBytes(ui->folderMbSpin->value(), ui->folderSizeUnit->currentIndex()) : 0;
    fileBytes = ui->fileSizeCheck->isChecked()
        ? sizeToBytes(ui->fileMbSpin->value(),   ui->fileSizeUnit->currentIndex())   : 0;
}

void MainWindow::startScan(const QString& root, qint64 timeRangeMs,
                           qint64 folderBytesThreshold, qint64 fileBytesThreshold,
                           int threadCount)
{
    // 取消后台搜索并等待完成：避免后台线程读取 m_allData 时
    // 主线程 clear()/setResultsWithTree() 修改 m_allData 导致悬空引用崩溃
    if (m_searchWatcher && m_searchWatcher->isRunning()) {
        ++m_searchGeneration;
        m_searchWatcher->cancel();
        m_searchWatcher->waitForFinished();
    }

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
    if (m_saveInProgress) return;  // 保存中不重置
    ui->scanBtn->setEnabled(true);
    ui->cancelBtn->setEnabled(false);
    ui->saveBtn->setEnabled(false);
    ui->clearBtn->setEnabled(false);
    ui->progressBar->setRange(0, 100);
    ui->progressBar->setValue(0);
    ui->statusLabel->setText(tr("就绪。请选择目录并设置参数后点击“开始扫描”。"));
    ui->currentLabel->clear();
}

// 配置文件路径：<软件根目录>/Settings/DiskScanner.ini
// 软件根目录 = 可执行文件所在目录（QCoreApplication::applicationDirPath）
// 启动时自动创建 Settings 目录
static QString settingsFilePath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString settingsDir = appDir + QStringLiteral("/Settings");
    QDir().mkpath(settingsDir);  // 确保目录存在
    return settingsDir + QStringLiteral("/DiskScanner.ini");
}

// 配置持久化：保存 扫描目录/过滤值/线程数/上次保存目录/列顺序与宽度 到 INI 文件
// INI 位置：<软件根目录>/Settings/DiskScanner.ini
void MainWindow::saveSettings() const
{
    QSettings settings(settingsFilePath(), QSettings::IniFormat);

    // 扫描目录（仅浏览选择的目录，即 dirEdit 中的文本）
    settings.setValue(QStringLiteral("Scan/Dir"), ui->dirEdit->text());

    // 时间过滤
    settings.setValue(QStringLiteral("Filter/HoursCheck"),  ui->hoursCheck->isChecked());
    settings.setValue(QStringLiteral("Filter/HoursValue"),  ui->hoursSpin->value());
    settings.setValue(QStringLiteral("Filter/HoursUnit"),   ui->hoursUnit->currentIndex());
    // 大小过滤：文件夹
    settings.setValue(QStringLiteral("Filter/FolderSizeCheck"), ui->folderSizeCheck->isChecked());
    settings.setValue(QStringLiteral("Filter/FolderSizeValue"), ui->folderMbSpin->value());
    settings.setValue(QStringLiteral("Filter/FolderSizeUnit"),  ui->folderSizeUnit->currentIndex());
    // 大小过滤：文件
    settings.setValue(QStringLiteral("Filter/FileSizeCheck"), ui->fileSizeCheck->isChecked());
    settings.setValue(QStringLiteral("Filter/FileSizeValue"), ui->fileMbSpin->value());
    settings.setValue(QStringLiteral("Filter/FileSizeUnit"),  ui->fileSizeUnit->currentIndex());

    // 并发线程数
    settings.setValue(QStringLiteral("Scan/Threads"), ui->threadSpin->value());

    // 上次保存目录
    settings.setValue(QStringLiteral("Save/LastDir"), m_lastSaveDir);

    // 结果展示框列宽（按逻辑索引保存）
    QHeaderView* h = ui->treeView->header();
    QStringList widths;
    for (int logical = 0; logical < h->count(); ++logical) {
        widths << QString::number(h->sectionSize(logical));
    }
    settings.setValue(QStringLiteral("View/ColumnWidths"), widths);

    // 结果展示框列顺序（按视觉位置保存逻辑索引）
    // 名称列（logical 0）固定在最左，从视觉位置 1 开始保存
    QStringList order;
    for (int visual = 0; visual < h->count(); ++visual) {
        order << QString::number(h->logicalIndex(visual));
    }
    settings.setValue(QStringLiteral("View/ColumnOrder"), order);
}

// 加载持久化配置到 UI 控件与成员变量
// 列宽与列顺序通过成员变量 m_savedColumn* 暂存，由构造函数中的 singleShot 应用
void MainWindow::loadSettings()
{
    QSettings settings(settingsFilePath(), QSettings::IniFormat);

    // 扫描目录
    const QString dir = settings.value(QStringLiteral("Scan/Dir")).toString();
    if (!dir.isEmpty()) ui->dirEdit->setText(dir);

    // 时间过滤
    if (settings.contains(QStringLiteral("Filter/HoursCheck"))) {
        ui->hoursCheck->setChecked(settings.value(QStringLiteral("Filter/HoursCheck")).toBool());
    }
    if (settings.contains(QStringLiteral("Filter/HoursValue"))) {
        ui->hoursSpin->setValue(settings.value(QStringLiteral("Filter/HoursValue")).toDouble());
    }
    if (settings.contains(QStringLiteral("Filter/HoursUnit"))) {
        const int idx = settings.value(QStringLiteral("Filter/HoursUnit")).toInt();
        if (idx >= 0 && idx < ui->hoursUnit->count()) ui->hoursUnit->setCurrentIndex(idx);
    }
    // 大小过滤：文件夹
    if (settings.contains(QStringLiteral("Filter/FolderSizeCheck"))) {
        ui->folderSizeCheck->setChecked(settings.value(QStringLiteral("Filter/FolderSizeCheck")).toBool());
    }
    if (settings.contains(QStringLiteral("Filter/FolderSizeValue"))) {
        ui->folderMbSpin->setValue(settings.value(QStringLiteral("Filter/FolderSizeValue")).toDouble());
    }
    if (settings.contains(QStringLiteral("Filter/FolderSizeUnit"))) {
        const int idx = settings.value(QStringLiteral("Filter/FolderSizeUnit")).toInt();
        if (idx >= 0 && idx < ui->folderSizeUnit->count()) ui->folderSizeUnit->setCurrentIndex(idx);
    }
    // 大小过滤：文件
    if (settings.contains(QStringLiteral("Filter/FileSizeCheck"))) {
        ui->fileSizeCheck->setChecked(settings.value(QStringLiteral("Filter/FileSizeCheck")).toBool());
    }
    if (settings.contains(QStringLiteral("Filter/FileSizeValue"))) {
        ui->fileMbSpin->setValue(settings.value(QStringLiteral("Filter/FileSizeValue")).toDouble());
    }
    if (settings.contains(QStringLiteral("Filter/FileSizeUnit"))) {
        const int idx = settings.value(QStringLiteral("Filter/FileSizeUnit")).toInt();
        if (idx >= 0 && idx < ui->fileSizeUnit->count()) ui->fileSizeUnit->setCurrentIndex(idx);
    }

    // 并发线程数
    if (settings.contains(QStringLiteral("Scan/Threads"))) {
        const int t = settings.value(QStringLiteral("Scan/Threads")).toInt();
        if (t >= ui->threadSpin->minimum() && t <= ui->threadSpin->maximum()) {
            ui->threadSpin->setValue(t);
        }
    }

    // 上次保存目录
    m_lastSaveDir = settings.value(QStringLiteral("Save/LastDir")).toString();

    // 列宽（按逻辑索引加载）
    const QStringList widths = settings.value(QStringLiteral("View/ColumnWidths")).toStringList();
    if (widths.size() >= 6) {
        m_savedColumnWidths.clear();
        m_savedColumnWidths.reserve(6);
        for (int i = 0; i < 6; ++i) {
            bool ok = false;
            const int w = widths[i].toInt(&ok);
            m_savedColumnWidths.push_back(ok && w > 0 ? w : 100);
        }
    }

    // 列顺序（按视觉位置加载逻辑索引）
    const QStringList order = settings.value(QStringLiteral("View/ColumnOrder")).toStringList();
    if (order.size() >= 6) {
        m_savedColumnOrder.clear();
        m_savedColumnOrder.reserve(6);
        bool ok = false;
        const int firstLogical = order[0].toInt(&ok);
        // 校验：名称列必须在最左
        if (ok && firstLogical == 0) {
            m_savedColumnOrder.push_back(0);
            for (int i = 1; i < 6; ++i) {
                bool ok2 = false;
                const int logical = order[i].toInt(&ok2);
                if (ok2 && logical >= 1 && logical <= 5) {
                    m_savedColumnOrder.push_back(logical);
                }
            }
            if (m_savedColumnOrder.size() != 6) {
                m_savedColumnOrder.clear();
            }
        }
    }
}


