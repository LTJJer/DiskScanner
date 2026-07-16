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
#include <algorithm>

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

MainWindow::MainWindow(QWidget* parent) : QWidget(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // 表头列宽模式
    ui->treeWidget->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ui->treeWidget->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->treeWidget->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    ui->treeWidget->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    ui->treeWidget->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    ui->treeWidget->header()->setSectionsMovable(false);

    connect(ui->browseBtn,  &QPushButton::clicked, this, &MainWindow::onBrowse);
    connect(ui->scanBtn,    &QPushButton::clicked, this, &MainWindow::onScan);
    connect(ui->cancelBtn,  &QPushButton::clicked, this, &MainWindow::onCancel);
    connect(ui->saveBtn,    &QPushButton::clicked, this, &MainWindow::onSave);
    connect(ui->clearBtn,   &QPushButton::clicked, this, &MainWindow::onClear);

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
    if (m_results.isEmpty()) {
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
    m_results.clear();
    ui->treeWidget->clear();
    ui->saveBtn->setEnabled(false);
    ui->clearBtn->setEnabled(false);
    ui->progressBar->setValue(0);
    ui->statusLabel->setText(tr("已清空结果。"));
    ui->currentLabel->clear();
}

void MainWindow::onProgress(int scanned, const QString& currentPath)
{
    ui->statusLabel->setText(tr("正在扫描… 已扫描 %1 个条目").arg(scanned));
    ui->currentLabel->setText(QDir::toNativeSeparators(currentPath));
}

void MainWindow::onFinished()
{
    if (m_worker) {
        m_results     = m_worker->results();
        m_lastParams   = m_worker->params();
        m_lastScanned  = m_worker->scannedCount();
        m_lastElapsed  = m_worker->elapsedMs();
    }
    if (m_cancelled) m_failed = false;
    populateTree();

    const int dirCount = int(std::count_if(m_results.begin(), m_results.end(),
                                           [](const ScanItem& s) { return s.isDir; }));
    const int fileCount = int(m_results.size()) - dirCount;
    const double sec = m_lastElapsed / 1000.0;

    QString summary;
    if (m_failed) {
        summary = tr("扫描失败。");
    } else if (m_cancelled) {
        summary = tr("已取消。匹配结果：%1 项（目录 %2，文件 %3）；扫描 %4 项，耗时 %5 秒")
            .arg(m_results.size()).arg(dirCount).arg(fileCount)
            .arg(m_lastScanned).arg(sec, 0, 'f', 2);
    } else {
        summary = tr("扫描完成。匹配结果：%1 项（目录 %2，文件 %3）；扫描 %4 项，耗时 %5 秒")
            .arg(m_results.size()).arg(dirCount).arg(fileCount)
            .arg(m_lastScanned).arg(sec, 0, 'f', 2);
    }
    ui->statusLabel->setText(summary);
    ui->currentLabel->clear();
    ui->progressBar->setRange(0, 100);
    ui->progressBar->setValue(100);

    ui->scanBtn->setEnabled(true);
    ui->cancelBtn->setEnabled(false);
    ui->saveBtn->setEnabled(!m_results.isEmpty());
    ui->clearBtn->setEnabled(!m_results.isEmpty());

    cleanupThread();  // 停止线程并释放 worker/thread 对象
}

void MainWindow::startScan(const QString& root, double hours, double folderMb, double fileMb)
{
    m_cancelled = false;
    m_failed = false;
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
    items.reserve(m_results.size());
    for (const ScanItem& s : std::as_const(m_results)) {
        const QString typeStr = s.isDir ? tr("目录") : tr("文件");
        const QString sizeStr = formatSize(s.size);
        const QString mtStr = s.mtime.isValid()
            ? s.mtime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QStringLiteral("-");
        const QString ctStr = s.ctime.isValid()
            ? s.ctime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QStringLiteral("-");
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
