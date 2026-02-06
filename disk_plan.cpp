#include "disk_plan.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

namespace {

bool readRequiredString(const QJsonObject& obj, const QString& key, QString& out, QString& err) {
    if (!obj.contains(key) || !obj.value(key).isString()) {
        err = QString("Missing or invalid string field: %1").arg(key);
        return false;
    }
    out = obj.value(key).toString();
    return true;
}

bool readRequiredUInt64(const QJsonObject& obj, const QString& key, quint64& out, QString& err) {
    if (!obj.contains(key) || !obj.value(key).isDouble()) {
        err = QString("Missing or invalid numeric field: %1").arg(key);
        return false;
    }
    const double v = obj.value(key).toDouble(-1);
    if (v < 0) {
        err = QString("Field must be non-negative: %1").arg(key);
        return false;
    }
    out = static_cast<quint64>(v);
    return true;
}

}

bool loadDiskPlanConfig(const QString& configPath, DiskPlanConfig& outConfig, QString& outError) {
    QFile f(configPath);
    if (!f.open(QIODevice::ReadOnly)) {
        outError = QString("Failed to open config file: %1").arg(configPath);
        return false;
    }

    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        outError = QString("Invalid JSON config: %1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    outConfig.schemaVersion = root.value("schemaVersion").toInt(1);

    if (!readRequiredString(root, "diskPath", outConfig.diskPath, outError)) return false;
    outConfig.sectorSize = static_cast<quint32>(root.value("sectorSize").toInt(512));
    if (!readRequiredString(root, "sgdiskPath", outConfig.sgdiskPath, outError)) return false;
    outConfig.wipeBeforePartition = root.value("wipeBeforePartition").toBool(true);

    if (!root.contains("partitions") || !root.value("partitions").isArray()) {
        outError = "Missing or invalid partitions array";
        return false;
    }

    const QJsonArray partitions = root.value("partitions").toArray();
    outConfig.partitions.clear();

    for (const auto& pVal : partitions) {
        if (!pVal.isObject()) {
            outError = "Partition item must be an object";
            return false;
        }

        const QJsonObject pObj = pVal.toObject();
        PartitionSpec spec;
        spec.number = pObj.value("number").toInt(0);
        if (spec.number <= 0) {
            outError = "Partition number must be > 0";
            return false;
        }

        spec.name = pObj.value("name").toString(QString("partition_%1").arg(spec.number));
        if (!readRequiredUInt64(pObj, "startSector", spec.startSector, outError)) return false;
        if (!readRequiredUInt64(pObj, "sizeSectors", spec.sizeSectors, outError)) return false;
        spec.typeGuid = pObj.value("typeGuid").toString("8300");

        if (pObj.contains("filesystem") && pObj.value("filesystem").isObject()) {
            spec.hasFilesystem = true;
            const QJsonObject fsObj = pObj.value("filesystem").toObject();
            spec.filesystem.type = fsObj.value("type").toString("NTFS");
            spec.filesystem.label = fsObj.value("label").toString(spec.name);
            spec.filesystem.quickFormat = fsObj.value("quick").toBool(true);
        }

        if (pObj.contains("payload") && pObj.value("payload").isObject()) {
            spec.hasPayload = true;
            const QJsonObject payloadObj = pObj.value("payload").toObject();
            spec.payload.sourcePath = payloadObj.value("sourcePath").toString();
            spec.payload.mode = payloadObj.value("mode").toString("auto");
            spec.payload.targetOffsetSectors = static_cast<quint64>(payloadObj.value("targetOffsetSectors").toDouble(0));
        }

        spec.extra = pObj.value("extra").toObject();
        outConfig.partitions.push_back(spec);
    }

    return true;
}
