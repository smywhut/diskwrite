#include <QApplication>
#include <QWidget>
#include <QScreen>
#include <QDebug>
#include <QFile>
#include <QString>
#include <QCryptographicHash>
#include <windows.h>
#include <winioctl.h>
#include <winternl.h>
#include <QScopeGuard>
#include <iostream>
#include <memory>
#include <optional>

// --- 新增辅助函数：解析 Windows 错误码 ---
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
    void reset(HANDLE h = INVALID_HANDLE_VALUE) {
        close();
        handle = h;
    }
    HANDLE get() const { return handle; }
    bool valid() const { return handle != INVALID_HANDLE_VALUE; }
    void close() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    }
};

// --- 新增辅助函数：获取物理磁盘扇区大小 ---
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
    qDebug() << "Calculating SHA256 for file:" << path;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};

    QCryptographicHash hash(QCryptographicHash::Sha256);
    constexpr qint64 bufSize = 1024 * 1024;
    while (!f.atEnd()) {
        hash.addData(f.read(bufSize));
    }
    return hash.result();
}

static bool lockAndDismount(HANDLE hDisk) {
    qDebug() << "Attempting to lock and dismount volume";
    DWORD bytes = 0;
    // 检查占用并尝试锁定
    if (!DeviceIoControl(hDisk, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &bytes, nullptr)) {
        qDebug() << "CRITICAL: Volume is occupied. Cannot lock." << getWinErrorString(GetLastError());
        return false;
    }
    if (!DeviceIoControl(hDisk, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &bytes, nullptr)) {
        qDebug() << "Warning: Dismount failed." << getWinErrorString(GetLastError());
        // 有时即使卸载失败，锁定成功也可以写入
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
    qDebug() << "Verifying device SHA256. Sector size:" << sectorSize;
    LARGE_INTEGER li;
    li.QuadPart = offset;
    if (!SetFilePointerEx(hDisk, li, nullptr, FILE_BEGIN)) {
        qDebug() << "Failed to seek for verification." << getWinErrorString(GetLastError());
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    DWORD readBufSize = qMax(1024u * 1024u, sectorSize);
    std::unique_ptr<BYTE[]> buffer(new BYTE[readBufSize]);
    quint64 remainingBytes = length;

    while (remainingBytes > 0) {
        DWORD toRead = (DWORD)qMin<quint64>(readBufSize, ((remainingBytes + sectorSize - 1) / sectorSize) * sectorSize);
        DWORD br = 0;
        if (!ReadFile(hDisk, buffer.get(), toRead, &br, nullptr) || br == 0) {
            qDebug() << "Read failed during verification." << getWinErrorString(GetLastError());
            break;
        }

        DWORD bytesToHash = (DWORD)qMin<quint64>(br, remainingBytes);
        hash.addData(reinterpret_cast<const char*>(buffer.get()), bytesToHash);
        remainingBytes -= bytesToHash;
    }
    return hash.result();
}

bool writeDiskWithSeekAndSHA256(QString inputFile, QString outputDevice, quint32 blockSize, quint32 seekBlocks) {
    if (inputFile.isEmpty() || outputDevice.isEmpty()) {
        qDebug() << "ERROR: inputFile or outputDevice is empty.";
        return false;
    }

    QFile src(inputFile);
    if (!src.open(QIODevice::ReadOnly)) {
        qDebug() << "ERROR: Failed to open input file.";
        return false;
    }
    const quint64 fileSize = src.size();
    if (fileSize == 0) {
        qDebug() << "ERROR: Input file is empty.";
        return false;
    }

    // 1. 打开设备
    WinHandle hDisk(CreateFileW(outputDevice.toStdWString().c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, nullptr));

    if (!hDisk.valid()) {
        qDebug() << "ERROR: Failed to open device." << getWinErrorString(GetLastError());
        return false;
    }

    // 2. 动态校准扇区大小 (覆盖传入的 blockSize 以确保安全)
    DWORD actualSectorSize = 512;
    if (getDeviceSectorSize(hDisk.get(), actualSectorSize)) {
        qDebug() << "Detected physical sector size:" << actualSectorSize;
        blockSize = actualSectorSize;
    }
    if (blockSize == 0) {
        qDebug() << "ERROR: Block size is zero.";
        return false;
    }

    // 3. 锁定检查
    if (!lockAndDismount(hDisk.get())) return false;
    auto unlockGuard = qScopeGuard([&] { unlockVolume(hDisk.get()); });

    auto deviceLength = getDeviceLength(hDisk.get());
    if (!deviceLength.has_value()) {
        qDebug() << "ERROR: Failed to query device length." << getWinErrorString(GetLastError());
        return false;
    }

    // 4. 定位
    quint64 offset = quint64(seekBlocks) * blockSize;
    if (offset > deviceLength.value() || fileSize > deviceLength.value() - offset) {
        qDebug() << "ERROR: Write range exceeds device size." << "Device bytes:" << deviceLength.value()
                 << "Offset:" << offset << "File bytes:" << fileSize;
        return false;
    }
    LARGE_INTEGER li;
    li.QuadPart = offset;
    if (!SetFilePointerEx(hDisk.get(), li, nullptr, FILE_BEGIN)) {
        qDebug() << "ERROR: Failed to seek device." << getWinErrorString(GetLastError());
        return false;
    }

    // 5. 写入循环 (引入 Read-Modify-Write)
    std::unique_ptr<BYTE[]> sectorBuffer(new BYTE[blockSize]);
    quint64 totalWrittenActual = 0;
    quint64 blocksWritten = 0;



    while (totalWrittenActual < fileSize) {
        qint64 br = src.read(reinterpret_cast<char*>(sectorBuffer.get()), blockSize);
        if (br <= 0) break;

        DWORD bytesToWrite = blockSize;

        // 如果最后一块不满一个扇区，执行 Read-Modify-Write 保护原有数据
        if (br < (qint64)blockSize) {
            qDebug() << "Partial sector detected (RMW). Reading original data first...";

            // 记录当前物理位置
            LARGE_INTEGER currentPos;
            LARGE_INTEGER zero{};
            if (!SetFilePointerEx(hDisk.get(), zero, &currentPos, FILE_CURRENT)) {
                qDebug() << "ERROR: Failed to query current position." << getWinErrorString(GetLastError());
                return false;
            }

            // 读取原始扇区数据
            std::unique_ptr<BYTE[]> originalData(new BYTE[blockSize]);
            DWORD bytesReadFromDisk = 0;
            if (!ReadFile(hDisk.get(), originalData.get(), blockSize, &bytesReadFromDisk, nullptr) ||
                bytesReadFromDisk != blockSize) {
                qDebug() << "ERROR: Failed to read original sector data." << getWinErrorString(GetLastError());
                return false;
            }

            // 将新数据合并到旧数据前端
            memcpy(originalData.get(), sectorBuffer.get(), br);
            memcpy(sectorBuffer.get(), originalData.get(), blockSize);

            // 跳回原来的位置准备写入
            if (!SetFilePointerEx(hDisk.get(), currentPos, nullptr, FILE_BEGIN)) {
                qDebug() << "ERROR: Failed to reset position." << getWinErrorString(GetLastError());
                return false;
            }
            bytesToWrite = blockSize;
        }

        DWORD bw = 0;
        if (!WriteFile(hDisk.get(), sectorBuffer.get(), bytesToWrite, &bw, nullptr) || bw != bytesToWrite) {
            qDebug() << "Write failed:" << getWinErrorString(GetLastError());
            return false;
        }

        totalWrittenActual += br;
        blocksWritten++;
        if (blocksWritten % 100 == 0) qDebug() << "Progress:" << totalWrittenActual << "bytes written";
    }

    if (!FlushFileBuffers(hDisk.get())) {
        qDebug() << "Warning: Failed to flush buffers." << getWinErrorString(GetLastError());
    }
    unlockVolume(hDisk.get());
    unlockGuard.dismiss();
    hDisk.close();

    // 6. SHA256 校验
    qDebug() << "=== Starting SHA256 verification ===";
    QByteArray srcSha = sha256OfFile(inputFile);

    hDisk.reset(CreateFileW(outputDevice.toStdWString().c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr));

    if (!hDisk.valid()) return false;
    QByteArray dstSha = sha256OfDevice(hDisk.get(), offset, fileSize, blockSize);
    hDisk.close();

    bool match = (srcSha == dstSha && !srcSha.isEmpty());
    qDebug() << "Source SHA256:" << srcSha.toHex();
    qDebug() << "Device SHA256:" << dstSha.toHex();
    qDebug() << (match ? "VERIFICATION SUCCESS" : "VERIFICATION FAILED");

    return match;
}

int main(int argc, char *argv[]) {
#ifdef Q_OS_WIN
    enableConsoleOutput();
#endif
    qDebug() << "=== Disk Utility Started ===";

    // 注意：请确保以管理员权限运行，否则 CreateFile 和 FSCTL_LOCK_VOLUME 会失败
    if (argc < 3) {
        qDebug() << "Usage:" << argv[0] << "<inputFile> <outputDevice> [blockSize] [seekBlocks]";
        qDebug() << "Example device path: \\\\?\\Volume{...} or \\\\?\\PhysicalDrive1";
        return 2;
    }
    QString inputFile = QString::fromLocal8Bit(argv[1]);
    QString outputDevice = QString::fromLocal8Bit(argv[2]);
    quint32 blockSize = 512;
    quint32 seekBlocks = 0;
    if (argc >= 4) {
        bool ok = false;
        blockSize = QString::fromLocal8Bit(argv[3]).toUInt(&ok);
        if (!ok || blockSize == 0) {
            qDebug() << "Invalid block size.";
            return 2;
        }
    }
    if (argc >= 5) {
        bool ok = false;
        seekBlocks = QString::fromLocal8Bit(argv[4]).toUInt(&ok);
        if (!ok) {
            qDebug() << "Invalid seek blocks.";
            return 2;
        }
    }

    bool result = writeDiskWithSeekAndSHA256(inputFile, outputDevice, blockSize, seekBlocks);

    qDebug() << "Main result:" << (result ? "SUCCESS" : "FAILURE");
    std::cout << "Press Enter to exit...";
    std::cin.get();
    return result ? 0 : 1;
}
