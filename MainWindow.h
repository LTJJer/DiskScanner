#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QWidget>
#include <QThread>

#include "ScanTypes.h"

class ScanWorker;

namespace Ui { class MainWindow; }

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
    void onProgress(int scanned, const QString& currentPath);
    void onFinished();

private:
    Ui::MainWindow* ui = nullptr;
    QThread* m_thread = nullptr;
    ScanWorker* m_worker = nullptr;
    bool m_cancelled = false;
    bool m_failed = false;

    ScanParams m_lastParams;
    QVector<ScanItem> m_results;
    int m_lastScanned = 0;
    qint64 m_lastElapsed = 0;

    void startScan(const QString& root, double hours, double folderMb, double fileMb);
    void populateTree();
    void resetUiIdle();
    bool writeResults(const QString& path) const;
    void cleanupThread();  // 停止线程并释放 worker/thread 对象
};

#endif // MAINWINDOW_H
