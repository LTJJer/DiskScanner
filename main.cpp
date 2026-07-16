#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <cstdio>

#include "MainWindow.h"
#include "ScanWorker.h"
#include "ResultsFormatter.h"

// 命令行模式：DiskScanner.exe --cli <root> <hours> <folderMb> <fileMb> <output.txt>
// 不启动 GUI，直接同步扫描并写入结果文件。返回 0 表示成功，非 0 表示失败。
static int runCli(int argc, char* argv[])
{
    QCoreApplication a(argc, argv);

    if (argc < 7) {
        fprintf(stderr, "Usage: DiskScanner --cli <root> <hours> <folderMb> <fileMb> <output.txt>\n");
        return 2;
    }

    bool ok1 = false, ok2 = false, ok3 = false;
    const double hours    = QString::fromLocal8Bit(argv[3]).toDouble(&ok1);
    const double folderMb = QString::fromLocal8Bit(argv[4]).toDouble(&ok2);
    const double fileMb   = QString::fromLocal8Bit(argv[5]).toDouble(&ok3);
    const QString outPath = QString::fromLocal8Bit(argv[6]);

    if (!ok1 || !ok2 || !ok3 || hours < 0 || folderMb < 0 || fileMb < 0) {
        fprintf(stderr, "Invalid numeric parameters.\n");
        return 2;
    }

    QDir rootDir(QString::fromLocal8Bit(argv[2]));
    if (!rootDir.exists()) {
        fprintf(stderr, "Directory does not exist: %s\n", argv[2]);
        return 3;
    }
    rootDir.makeAbsolute();

    fprintf(stderr, "Scanning: %s\n", argv[2]);
    fflush(stderr);

    ScanParams params{rootDir.absolutePath(), hours, folderMb, fileMb};
    ScanWorker worker(params);
    worker.doScan();  // 同步执行扫描，无需事件循环

    const QString text = formatResultsText(worker.results(), worker.params(),
                                            worker.scannedCount(), worker.elapsedMs());

    QFile f(outPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fprintf(stderr, "Cannot open output file: %s\n", argv[6]);
        return 4;
    }
    f.write(text.toUtf8());
    f.flush();
    f.close();

    fprintf(stderr, "Done. Scanned %d entries, %lld matches. Output: %s\n",
            worker.scannedCount(), qint64(worker.results().size()), argv[6]);
    return 0;
}

int main(int argc, char* argv[])
{
    // 命令行模式：--cli <root> <hours> <folderMb> <fileMb> <output.txt>
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--cli")) {
        return runCli(argc, argv);
    }

    QApplication a(argc, argv);
    a.setStyle(QStringLiteral("Fusion"));
    MainWindow w;
    w.show();
    return a.exec();
}
