#include "ScanWorker.h"

ScanWorker::ScanWorker(const ScanParams& params, QObject* parent)
    : QObject(parent), m_params(params)
{
    m_folderBytesThreshold = qint64(m_params.folderMb * 1024.0 * 1024.0);
    m_fileBytesThreshold   = qint64(m_params.fileMb   * 1024.0 * 1024.0);
    // 截止时刻 = 当前时间 - 指定小时数
    m_cutoff = QDateTime::currentDateTime().addMSecs(-qint64(m_params.hours * 3600.0 * 1000.0));
}

void ScanWorker::doScan()
{
    m_scanned = 0;
    m_elapsed.start();
    QDir root(m_params.root);
    if (!root.exists()) {
        emit failed(tr("指定的目录不存在：\n%1").arg(m_params.root));
        emit finished();
        return;
    }
    root.makeAbsolute();
    m_params.root = root.absolutePath();
    processDir(root);
    m_elapsedMs = m_elapsed.elapsed();
    emit finished();
}

void ScanWorker::cancel()
{
    m_cancel = true;
}

// 修改时间或创建时间在指定小时之内即视为匹配
bool ScanWorker::timeMatches(const QFileInfo& info) const
{
    const QDateTime mt = info.lastModified();
    if (mt.isValid() && mt >= m_cutoff) return true;
    const QDateTime ct = info.birthTime();
    if (ct.isValid() && ct >= m_cutoff) return true;
    return false;
}

void ScanWorker::addItem(bool isDir, const QFileInfo& info, qint64 size)
{
    ScanItem it;
    it.isDir = isDir;
    it.path  = info.filePath();
    it.size  = size;
    it.mtime = info.lastModified();
    it.ctime = info.birthTime();
    m_results.append(std::move(it));
}

// 递归处理目录，返回该目录（含其全部子内容）的总字节数
qint64 ScanWorker::processDir(const QDir& dir)
{
    // 循环防护：跳过已访问过的真实路径（目录联接/重解析点）
    const QString canon = dir.canonicalPath();
    if (!canon.isEmpty()) {
        if (m_visited.contains(canon))
            return 0;
        m_visited.insert(canon);
    }

    qint64 total = 0;
    const QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::NoSort);

    for (const QFileInfo& info : entries) {
        if (m_cancel) return total;
        ++m_scanned;

        if (info.isSymLink()) continue;  // 跳过符号链接，避免循环

        if (info.isDir()) {
            const qint64 subSize = processDir(QDir(info.filePath()));
            total += subSize;
            // 任何匹配大小和时间条件的子目录都加入结果（含顶层直接子目录）
            if (subSize > m_folderBytesThreshold && timeMatches(info)) {
                addItem(true, info, subSize);
            }
        } else if (info.isFile()) {
            const qint64 sz = info.size();
            total += sz;
            if (sz > m_fileBytesThreshold && timeMatches(info)) {
                addItem(false, info, sz);
            }
        }

        if ((m_scanned & 0x3F) == 0)
            emit progress(m_scanned, info.filePath());
    }
    return total;
}
