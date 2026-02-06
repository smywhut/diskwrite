#pragma once

#include <QString>
#include <QJsonObject>
#include <QList>

// 文件系统参数：若存在则代表该分区采用“格式化 + 文件拷贝”流程。
struct FilesystemConfig {
    QString type;          // 例如: NTFS/FAT32/exFAT
    QString label;         // 卷标
    bool quickFormat{true};
};

// 写入载荷配置。mode=auto 时会根据是否存在 filesystem 决策:
// - 有 filesystem -> copy
// - 无 filesystem -> raw
struct PayloadConfig {
    QString sourcePath;          // 需要写入/拷贝的源文件或目录
    QString mode{"auto"};       // auto/copy/raw
    quint64 targetOffsetSectors{0}; // raw 模式可选：相对分区起始偏移扇区
};

// 单分区配置。
struct PartitionSpec {
    int number{0};
    QString name;
    quint64 startSector{0};
    quint64 sizeSectors{0};
    QString typeGuid{"8300"};   // Linux filesystem 默认 GUID 简写
    QJsonObject extra;            // 预留扩展字段，避免未来重构

    bool hasFilesystem{false};
    FilesystemConfig filesystem;

    bool hasPayload{false};
    PayloadConfig payload;
};

// 全盘规划配置。
struct DiskPlanConfig {
    int schemaVersion{1};
    QString diskPath;             // 例如 \\.\PhysicalDrive3
    quint32 sectorSize{512};
    QString sgdiskPath;           // 例如 .\\tools\\sgdisk.exe
    bool wipeBeforePartition{true};
    QList<PartitionSpec> partitions;
};

bool loadDiskPlanConfig(const QString& configPath, DiskPlanConfig& outConfig, QString& outError);
