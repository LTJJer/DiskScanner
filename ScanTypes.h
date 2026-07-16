#ifndef SCANTYPES_H
#define SCANTYPES_H

#include <QString>
#include <QDateTime>
#include <QVector>

// 单条扫描结果
struct ScanItem {
    bool isDir = false;
    QString path;
    qint64 size = 0;
    QDateTime mtime;
    QDateTime ctime;
};

// 扫描参数
struct ScanParams {
    QString root;
    double hours = 0;
    double folderMb = 0;
    double fileMb = 0;
};

#endif // SCANTYPES_H
