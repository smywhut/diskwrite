#include <QApplication>
#include <QWidget>
#include <QScreen>
#include <QDebug>
#include <QFile>
#include <QString>
#include <QCryptographicHash>
#include <QScopeGuard>
#include <QProcess>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QTextStream>
#include <iostream>
#include <memory>
#include <optional>

#include <windows.h>
#include <winioctl.h>
#include <winternl.h>

#include "disk_plan.h"

QString getWinErrorString(DWORD errorCode) {
    LPWSTR buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&buf, 0, NULL);
    QString msg = QString::fromWCharArray(buf).trimmed();
    LocalFree(buf);
    return QString("Error %1: %2").arg(errorCode).arg(msg);
}

struct WinHandle {
    HANDLE handle{INVALID_HANDLE_VALUE};
    ~WinHandle() { close(); }
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    WinHandle() = default;
    explicit WinHandle(HANDLE h) : handle(h) {}
    WinHandle(WinHandle&& other) noexcept : handle(other.handle) { other.handle = INVALID_HANDLE_VALUE; }
    WinHandle& operator=(WinHandle&& other) noexcept {
        if (this != &other) {
            close();
            handle = other.handle;
            other.handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    void reset(HANDLE h = INVALID_HANDLE_VALUE) { close(); handle = h; }
    HANDLE get() const { return handle; }
    bool valid() const { return handle != INVALID_HANDLE_VALUE; }
    void close() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    }
};

bool getDeviceSectorSize(HANDLE hDisk, DWORD &sectorSize) {
    DISK_GEOMETRY dg;
    DWORD bytesReturned;
    if (DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &dg, sizeof(dg), &bytesReturned, NULL)) {
        sectorSize = dg.BytesPerSector;
        return true;
    }
    return false;
}

std::optional<quint64> getDeviceLength(HANDLE hDisk) {
    GET_LENGTH_INFORMATION lengthInfo{};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDisk, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &lengthInfo, sizeof(lengthInfo),
                         &bytesReturned, nullptr)) {
        return std::nullopt;
    }
    return static_cast<quint64>(lengthInfo.Length.QuadPart);
}

#ifdef Q_OS_WIN
void enableConsoleOutput() {
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        freopen("CONIN$", "r", stdin);

        qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &context, const QString &msg) {
            QByteArray localMsg = msg.toLocal8Bit();
            const char *typeStr = "Debug";
            switch (type) {
                case QtDebugMsg: typeStr = "Debug"; break;
                case QtInfoMsg: typeStr = "Info"; break;
                case QtWarningMsg: typeStr = "Warning"; break;
                case QtCriticalMsg: typeStr = "Critical"; break;
                case QtFatalMsg: typeStr = "Fatal"; break;
            }
            fprintf(stderr, "%s: %s (%s:%u)\n", typeStr, localMsg.constData(), context.file, context.line);
        });
    }
}
#endif

static QByteArray sha256OfFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    constexpr qint64 bufSize = 1024 * 1024;
    while (!f.atEnd()) hash.addData(f.read(bufSize));
    return hash.result();
}

static bool lockAndDismount(HANDLE hDisk) {
    DWORD bytes = 0;
    if (!DeviceIoControl(hDisk, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &bytes, nullptr)) {
        qDebug() << "CRITICAL: Volume is occupied. Cannot lock." << getWinErrorString(GetLastError());
        return false;
    }
    if (!DeviceIoControl(hDisk, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &bytes, nullptr)) {
        qDebug() << "Warning: Dismount failed." << getWinErrorString(GetLastError());
    }
    return true;
}

static void unlockVolume(HANDLE hDisk) {
    DWORD bytes = 0;
    if (!DeviceIoControl(hDisk, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &bytes, nullptr)) {
        qDebug() << "Warning: Failed to unlock volume." << getWinErrorString(GetLastError());
    }
}

static QByteArray sha256OfDevice(HANDLE hDisk, quint64 offset, quint64 length, quint32 sectorSize) {
    LARGE_INTEGER li;
    li.QuadPart = offset;
    if (!SetFilePointerEx(hDisk, li, nullptr, FILE_BEGIN)) return {};

    QCryptographicHash hash(QCryptographicHash::Sha256);
    DWORD readBufSize = qMax(1024u * 1024u, sectorSize);
    std::unique_ptr<BYTE[]> buffer(new BYTE[readBufSize]);
    quint64 remainingBytes = length;

    while (remainingBytes > 0) {
        DWORD toRead = (DWORD)qMin<quint64>(readBufSize, ((remainingBytes + sectorSize - 1) / sectorSize) * sectorSize);
        DWORD br = 0;
        if (!ReadFile(hDisk, buffer.get(), toRead, &br, nullptr) || br == 0) break;

        DWORD bytesToHash = (DWORD)qMin<quint64>(br, remainingBytes);
        hash.addData(reinterpret_cast<const char*>(buffer.get()), bytesToHash);
        remainingBytes -= bytesToHash;
    }
    return hash.result();
}

bool writeDiskWithSeekAndSHA256(QString inputFile, QString outputDevice, quint32 blockSize, quint64 seekBlocks) {
    QFile src(inputFile);
    if (!src.open(QIODevice::ReadOnly)) return false;
    const quint64 fileSize = src.size();
    if (fileSize == 0) return false;

    WinHandle hDisk(CreateFileW(outputDevice.toStdWString().c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, nullptr));
    if (!hDisk.valid()) {
        qDebug() << "ERROR: Failed to open device." << getWinErrorString(GetLastError());
        return false;
    }

    DWORD actualSectorSize = 512;
    if (getDeviceSectorSize(hDisk.get(), actualSectorSize)) blockSize = actualSectorSize;
    if (blockSize == 0) return false;

    if (!lockAndDismount(hDisk.get())) return false;
    auto unlockGuard = qScopeGuard([&] { unlockVolume(hDisk.get()); });

    auto deviceLength = getDeviceLength(hDisk.get());
    if (!deviceLength.has_value()) return false;

    quint64 offset = seekBlocks * blockSize;
    if (offset > deviceLength.value() || fileSize > deviceLength.value() - offset) return false;

    LARGE_INTEGER li;
    li.QuadPart = offset;
    if (!SetFilePointerEx(hDisk.get(), li, nullptr, FILE_BEGIN)) return false;

    std::unique_ptr<BYTE[]> sectorBuffer(new BYTE[blockSize]);
    quint64 totalWrittenActual = 0;
    while (totalWrittenActual < fileSize) {
        qint64 br = src.read(reinterpret_cast<char*>(sectorBuffer.get()), blockSize);
        if (br <= 0) break;

        DWORD bytesToWrite = blockSize;
        if (br < (qint64)blockSize) {
            LARGE_INTEGER currentPos;
            LARGE_INTEGER zero{};
            if (!SetFilePointerEx(hDisk.get(), zero, &currentPos, FILE_CURRENT)) return false;

            std::unique_ptr<BYTE[]> originalData(new BYTE[blockSize]);
            DWORD bytesReadFromDisk = 0;
            if (!ReadFile(hDisk.get(), originalData.get(), blockSize, &bytesReadFromDisk, nullptr) ||
                bytesReadFromDisk != blockSize) return false;

            memcpy(originalData.get(), sectorBuffer.get(), br);
            memcpy(sectorBuffer.get(), originalData.get(), blockSize);

            if (!SetFilePointerEx(hDisk.get(), currentPos, nullptr, FILE_BEGIN)) return false;
            bytesToWrite = blockSize;
        }

        DWORD bw = 0;
        if (!WriteFile(hDisk.get(), sectorBuffer.get(), bytesToWrite, &bw, nullptr) || bw != bytesToWrite) return false;
        totalWrittenActual += br;
    }

    FlushFileBuffers(hDisk.get());
    unlockVolume(hDisk.get());
    unlockGuard.dismiss();
    hDisk.close();

    QByteArray srcSha = sha256OfFile(inputFile);
    hDisk.reset(CreateFileW(outputDevice.toStdWString().c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr));
    if (!hDisk.valid()) return false;
    QByteArray dstSha = sha256OfDevice(hDisk.get(), offset, fileSize, blockSize);

    return (srcSha == dstSha && !srcSha.isEmpty());
}

// 执行外部命令并记录输出。用于 sgdisk/powershell 调用。
static bool runProcess(const QString& program, const QStringList& args, QString* combinedOutput = nullptr) {
    QProcess p;
    p.start(program, args);
    if (!p.waitForStarted() || !p.waitForFinished(-1)) return false;

    QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
    QString err = QString::fromLocal8Bit(p.readAllStandardError());
    if (combinedOutput) *combinedOutput = out + "\n" + err;
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        qDebug() << "Process failed:" << program << args << "\n" << out << err;
        return false;
    }
    return true;
}

// 按配置创建 GPT 分区表。采用 sgdisk.exe 实现。
static bool applyPartitionTable(const DiskPlanConfig& cfg) {
    if (cfg.wipeBeforePartition) {
        if (!runProcess(cfg.sgdiskPath, {"--zap-all", cfg.diskPath})) return false;
    }

    for (const auto& p : cfg.partitions) {
        quint64 endSector = p.startSector + p.sizeSectors - 1;
        QString nArg = QString("-n%1:%2:%3").arg(p.number).arg(p.startSector).arg(endSector);
        QString tArg = QString("-t%1:%2").arg(p.number).arg(p.typeGuid);
        QString cArg = QString("-c%1:%2").arg(p.number).arg(p.name);
        if (!runProcess(cfg.sgdiskPath, {nArg, tArg, cArg, cfg.diskPath})) return false;
    }

    return runProcess(cfg.sgdiskPath, {"-p", cfg.diskPath});
}

// 获取磁盘索引：将 \\.\PhysicalDrive3 解析为 3。
static int parseDiskIndex(const QString& diskPath) {
    const QString prefix = "\\\\.\\PhysicalDrive";
    if (!diskPath.startsWith(prefix, Qt::CaseInsensitive)) return -1;
    bool ok = false;
    int idx = diskPath.mid(prefix.size()).toInt(&ok);
    return ok ? idx : -1;
}

// 为分区分配临时盘符并返回盘符（如 "R"）。
static QString assignDriveLetter(int diskNumber, int partitionNumber) {
    QString script = QString(
        "$dl=(Get-Partition -DiskNumber %1 -PartitionNumber %2 | Add-PartitionAccessPath -AssignDriveLetter -PassThru).DriveLetter;"
        "Write-Output $dl")
        .arg(diskNumber)
        .arg(partitionNumber);

    QString output;
    if (!runProcess("powershell", {"-NoProfile", "-Command", script}, &output)) return {};

    const QString trimmed = output.trimmed();
    if (trimmed.isEmpty()) return {};
    return QString(trimmed[0]);
}

// 格式化分区。这里采用 PowerShell Format-Volume，满足“WindowsAPI 或第三方工具”的要求。
static bool formatPartition(int diskNumber, const PartitionSpec& p) {
    QString fs = p.filesystem.type;
    QString label = p.filesystem.label;
    QString quick = p.filesystem.quickFormat ? "$true" : "$false";
    QString script = QString(
        "Format-Volume -DiskNumber %1 -PartitionNumber %2 -FileSystem %3 -NewFileSystemLabel '%4' -Confirm:$false -Force -Full:(!$quick)")
        .arg(diskNumber)
        .arg(p.number)
        .arg(fs)
        .arg(label)
        .replace("$quick", quick);

    return runProcess("powershell", {"-NoProfile", "-Command", script});
}

static bool copyPathRecursively(const QString& srcPath, const QString& dstRoot) {
    QFileInfo srcInfo(srcPath);
    if (!srcInfo.exists()) return false;

    if (srcInfo.isFile()) {
        QString dstFile = QDir(dstRoot).filePath(srcInfo.fileName());
        QFile::remove(dstFile);
        return QFile::copy(srcPath, dstFile);
    }

    QDir srcDir(srcPath);
    QDir dstDir(dstRoot);
    if (!dstDir.exists() && !QDir().mkpath(dstRoot)) return false;

    QDirIterator it(srcPath, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString current = it.next();
        QString rel = srcDir.relativeFilePath(current);
        QString target = dstDir.filePath(rel);
        QFileInfo fi(current);
        if (fi.isDir()) {
            if (!QDir().mkpath(target)) return false;
        } else {
            QDir().mkpath(QFileInfo(target).path());
            QFile::remove(target);
            if (!QFile::copy(current, target)) return false;
        }
    }
    return true;
}

// 按规则写入数据：
// - 有 filesystem -> copy（先格式化后拷贝）
// - 无 filesystem -> raw（直接写入磁盘）
static bool writePartitionPayloads(const DiskPlanConfig& cfg) {
    int diskNumber = parseDiskIndex(cfg.diskPath);
    if (diskNumber < 0) {
        qDebug() << "diskPath must be like \\\\.\\PhysicalDriveN";
        return false;
    }

    for (const auto& p : cfg.partitions) {
        if (!p.hasPayload || p.payload.sourcePath.isEmpty()) continue;

        QString mode = p.payload.mode.toLower();
        if (mode == "auto") mode = p.hasFilesystem ? "copy" : "raw";

        if (p.hasFilesystem && !formatPartition(diskNumber, p)) {
            qDebug() << "Format failed for partition" << p.number;
            return false;
        }

        if (mode == "copy") {
            QString letter = assignDriveLetter(diskNumber, p.number);
            if (letter.isEmpty()) return false;
            QString dstRoot = letter + ":/";
            if (!copyPathRecursively(p.payload.sourcePath, dstRoot)) return false;
        } else if (mode == "raw") {
            quint64 seek = p.startSector + p.payload.targetOffsetSectors;
            if (!writeDiskWithSeekAndSHA256(p.payload.sourcePath, cfg.diskPath, cfg.sectorSize, seek)) return false;
        } else {
            qDebug() << "Unsupported payload mode:" << mode;
            return false;
        }
    }

    return true;
}

static bool executeDiskPlan(const QString& configPath) {
    DiskPlanConfig cfg;
    QString err;
    if (!loadDiskPlanConfig(configPath, cfg, err)) {
        qDebug() << "Config parse error:" << err;
        return false;
    }

    qDebug() << "Applying partition layout...";
    if (!applyPartitionTable(cfg)) return false;

    qDebug() << "Writing payloads...";
    if (!writePartitionPayloads(cfg)) return false;

    qDebug() << "Disk plan completed.";
    return true;
}

int main(int argc, char *argv[]) {
#ifdef Q_OS_WIN
    enableConsoleOutput();
#endif
    qDebug() << "=== Disk Utility Started ===";

    // 新增配置驱动入口：
    //   app --config disk_plan.json
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == "--config") {
        const QString configPath = QString::fromLocal8Bit(argv[2]);
        bool ok = executeDiskPlan(configPath);
        qDebug() << "Config mode result:" << (ok ? "SUCCESS" : "FAILURE");
        return ok ? 0 : 1;
    }

    // 兼容旧模式：直接写入文件到设备（带 seek + SHA256 校验）
    if (argc < 3) {
        qDebug() << "Usage:" << argv[0] << "<inputFile> <outputDevice> [blockSize] [seekBlocks]";
        qDebug() << "Or:" << argv[0] << "--config <diskPlan.json>";
        return 2;
    }

    QString inputFile = QString::fromLocal8Bit(argv[1]);
    QString outputDevice = QString::fromLocal8Bit(argv[2]);
    quint32 blockSize = 512;
    quint64 seekBlocks = 0;

    if (argc >= 4) {
        bool ok = false;
        blockSize = QString::fromLocal8Bit(argv[3]).toUInt(&ok);
        if (!ok || blockSize == 0) return 2;
    }
    if (argc >= 5) {
        bool ok = false;
        seekBlocks = QString::fromLocal8Bit(argv[4]).toULongLong(&ok);
        if (!ok) return 2;
    }

    bool result = writeDiskWithSeekAndSHA256(inputFile, outputDevice, blockSize, seekBlocks);
    qDebug() << "Main result:" << (result ? "SUCCESS" : "FAILURE");
    return result ? 0 : 1;
}
