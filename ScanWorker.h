#ifndef SCANWORKER_H
#define SCANWORKER_H

#include <QObject>
#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QSet>
#include <atomic>

#include "ScanTypes.h"

// 后台扫描工作对象，运行在独立线程中
class ScanWorker : public QObject {
    Q_OBJECT
public:
    explicit ScanWorker(const ScanParams& params, QObject* parent = nullptr);

    const QVector<ScanItem>& results() const { return m_results; }
    int scannedCount() const { return m_scanned; }
    qint64 elapsedMs() const { return m_elapsedMs; }
    const ScanParams& params() const { return m_params; }

public slots:
    void doScan();
    void cancel();

signals:
    void progress(int scanned, const QString& currentPath);
    void finished();
    void failed(const QString& msg);

private:
    ScanParams m_params;
    qint64 m_folderBytesThreshold = 0;
    qint64 m_fileBytesThreshold = 0;
    QDateTime m_cutoff;
    QVector<ScanItem> m_results;
    QSet<QString> m_visited;  // 规范化路径集合，防止目录联接/重解析点循环
    int m_scanned = 0;
    std::atomic<bool> m_cancel{false};
    QElapsedTimer m_elapsed;
    qint64 m_elapsedMs = 0;

    bool timeMatches(const QFileInfo& info) const;
    void addItem(bool isDir, const QFileInfo& info, qint64 size);
    qint64 processDir(const QDir& dir);
};

#endif // SCANWORKER_H
