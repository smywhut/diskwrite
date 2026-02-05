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

// --- 新增辅助函数：解析 Windows 错误码 ---
QString getWinErrorString(DWORD errorCode) {
    LPWSTR buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&buf, 0, NULL);
    QString msg = QString::fromWCharArray(buf).trimmed();
    LocalFree(buf);
    return QString("Error %1: %2").arg(errorCode).arg(msg);
}

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

static QByteArray md5OfFile(const QString& path) {
    qDebug() << "Calculating MD5 for file:" << path;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};

    QCryptographicHash hash(QCryptographicHash::Md5);
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

static QByteArray md5OfDevice(HANDLE hDisk, quint64 offset, quint64 length, quint32 sectorSize) {
    qDebug() << "Verifying device MD5. Sector size:" << sectorSize;
    LARGE_INTEGER li;
    li.QuadPart = offset;
    if (!SetFilePointerEx(hDisk, li, nullptr, FILE_BEGIN)) return {};

    QCryptographicHash hash(QCryptographicHash::Md5);
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

bool writeDiskWithSeekAndMD5(QString inputFile, QString outputDevice, quint32 blockSize, quint32 seekBlocks) {
    // 这里保留你硬编码的路径用于测试，实际建议通过参数传入
    outputDevice = "\\\\?\\Volume{ce08e894-c8cd-11ee-9f32-bc6ee2faa397}";

    QFile src(inputFile);
    if (!src.open(QIODevice::ReadOnly)) return false;
    const quint64 fileSize = src.size();

    // 1. 打开设备
    HANDLE hDisk = CreateFileW(outputDevice.toStdWString().c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, nullptr);

    if (hDisk == INVALID_HANDLE_VALUE) {
        qDebug() << "ERROR: Failed to open device." << getWinErrorString(GetLastError());
        return false;
    }
    auto closeDisk = qScopeGuard([&] { if (hDisk != INVALID_HANDLE_VALUE) CloseHandle(hDisk); });

    // 2. 动态校准扇区大小 (覆盖传入的 blockSize 以确保安全)
    DWORD actualSectorSize = 512;
    if (getDeviceSectorSize(hDisk, actualSectorSize)) {
        qDebug() << "Detected physical sector size:" << actualSectorSize;
        blockSize = actualSectorSize;
    }

    // 3. 锁定检查
    if (!lockAndDismount(hDisk)) return false;

    // 4. 定位
    quint64 offset = quint64(seekBlocks) * blockSize;
    LARGE_INTEGER li;
    li.QuadPart = offset;
    SetFilePointerEx(hDisk, li, nullptr, FILE_BEGIN);

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
            SetFilePointerEx(hDisk, {0}, &currentPos, FILE_CURRENT);

            // 读取原始扇区数据
            std::unique_ptr<BYTE[]> originalData(new BYTE[blockSize]);
            DWORD bytesReadFromDisk = 0;
            ReadFile(hDisk, originalData.get(), blockSize, &bytesReadFromDisk, nullptr);

            // 将新数据合并到旧数据前端
            memcpy(originalData.get(), sectorBuffer.get(), br);
            memcpy(sectorBuffer.get(), originalData.get(), blockSize);

            // 跳回原来的位置准备写入
            SetFilePointerEx(hDisk, currentPos, nullptr, FILE_BEGIN);
            bytesToWrite = blockSize;
        }

        DWORD bw = 0;
        if (!WriteFile(hDisk, sectorBuffer.get(), bytesToWrite, &bw, nullptr)) {
            qDebug() << "Write failed:" << getWinErrorString(GetLastError());
            return false;
        }

        totalWrittenActual += br;
        blocksWritten++;
        if (blocksWritten % 100 == 0) qDebug() << "Progress:" << totalWrittenActual << "bytes written";
    }

    FlushFileBuffers(hDisk);
    CloseHandle(hDisk);
    hDisk = INVALID_HANDLE_VALUE;
    closeDisk.dismiss();

    // 6. MD5 校验
    qDebug() << "=== Starting MD5 verification ===";
    QByteArray srcMd5 = md5OfFile(inputFile);

    hDisk = CreateFileW(outputDevice.toStdWString().c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);

    if (hDisk == INVALID_HANDLE_VALUE) return false;
    QByteArray dstMd5 = md5OfDevice(hDisk, offset, fileSize, blockSize);
    CloseHandle(hDisk);

    bool match = (srcMd5 == dstMd5 && !srcMd5.isEmpty());
    qDebug() << "Source MD5:" << srcMd5.toHex();
    qDebug() << "Device MD5:" << dstMd5.toHex();
    qDebug() << (match ? "VERIFICATION SUCCESS" : "VERIFICATION FAILED");

    return match;
}

int main(int argc, char *argv[]) {
#ifdef Q_OS_WIN
    enableConsoleOutput();
#endif
    qDebug() << "=== Disk Utility Started ===";

    // 注意：请确保以管理员权限运行，否则 CreateFile 和 FSCTL_LOCK_VOLUME 会失败
    bool result = writeDiskWithSeekAndMD5("D:/elevoc_dnn_kernel.log", "", 512, 0);

    qDebug() << "Main result:" << (result ? "SUCCESS" : "FAILURE");
    std::cout << "Press Enter to exit...";
    std::cin.get();
    return result ? 0 : 1;
}
