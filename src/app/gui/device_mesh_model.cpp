#include "app/gui/device_mesh_model.h"

#include "app/gui/cloud_client.h"
#include "common/utils/log.h"

#include <QDateTime>
#include <QJsonObject>

namespace vivora::gui {

DeviceMeshModel::DeviceMeshModel(QObject* parent) : QAbstractListModel(parent) {}

int DeviceMeshModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(devices_.size());
}

QVariant DeviceMeshModel::data(const QModelIndex& index, int role) const {
    if (index.row() < 0 || index.row() >= devices_.size()) return {};
    const Device& d = devices_[index.row()];
    switch (role) {
        case DevIdRole:    return d.devId;
        case DevNameRole:  return d.name;
        case OsRole:       return d.os;
        case OnlineRole:   return d.online;
        case CurrentRole:  return d.current;
        case WarnedRole:   return d.warned;
        case SeenRole:     return d.seen;
        case PeerCodeRole: return d.peerCode;
        // No backend "host" field — surface the peer code (the Settings table
        // shows this instead of a fake IP).
        case HostRole:     return d.peerCode;
        default:           return {};
    }
}

QHash<int, QByteArray> DeviceMeshModel::roleNames() const {
    return {
        {DevIdRole,    "devId"},
        {DevNameRole,  "devName"},
        {OsRole,       "os"},
        {OnlineRole,   "online"},
        {CurrentRole,  "current"},
        {WarnedRole,   "warned"},
        {SeenRole,     "seen"},
        {PeerCodeRole, "peerCode"},
        {HostRole,     "host"},
    };
}

QVariantMap DeviceMeshModel::get(int row) const {
    QVariantMap m;
    if (row < 0 || row >= devices_.size()) return m;
    const Device& d = devices_[row];
    m["devId"]    = d.devId;
    m["devName"]  = d.name;
    m["os"]       = d.os;
    m["online"]   = d.online;
    m["current"]  = d.current;
    m["warned"]   = d.warned;
    m["seen"]     = d.seen;
    m["peerCode"] = d.peerCode;
    m["host"]     = d.peerCode;
    return m;
}

int DeviceMeshModel::indexOfId(const QString& deviceId) const {
    for (int i = 0; i < devices_.size(); ++i)
        if (devices_[i].devId == deviceId) return i;
    return -1;
}

// Humanise last_seen into "now" / "12m ago" / "3h ago" / "2d ago".  Accepts a
// unix-seconds number or an ISO-8601 string; returns "" if neither parses.
QString DeviceMeshModel::humanizeSeen(const QJsonValue& lastSeen) {
    qint64 secs = 0;
    if (lastSeen.isDouble()) {
        secs = static_cast<qint64>(lastSeen.toDouble());
    } else if (lastSeen.isString()) {
        const QString s = lastSeen.toString();
        QDateTime dt = QDateTime::fromString(s, Qt::ISODate);
        if (!dt.isValid()) dt = QDateTime::fromString(s, Qt::ISODateWithMs);
        if (!dt.isValid()) return {};
        secs = dt.toSecsSinceEpoch();
    } else {
        return {};
    }
    const qint64 delta = QDateTime::currentSecsSinceEpoch() - secs;
    if (delta < 0)       return "now";
    if (delta < 60)      return "now";
    if (delta < 3600)    return QString("%1m ago").arg(delta / 60);
    if (delta < 86400)   return QString("%1h ago").arg(delta / 3600);
    return QString("%1d ago").arg(delta / 86400);
}

void DeviceMeshModel::setDevices(const QJsonArray& devices) {
    beginResetModel();
    devices_.clear();
    devices_.reserve(devices.size());
    for (const QJsonValue& v : devices) {
        const QJsonObject o = v.toObject();
        Device d;
        d.devId    = o.value("device_id").toString();
        d.name     = o.value("name").toString();
        d.os       = o.value("os").toString();
        d.pubkey   = o.value("pubkey").toString();
        d.peerCode = o.value("peer_code").toString();
        d.online   = o.value("online").toBool();
        d.current  = o.value("is_current").toBool();
        d.warned   = o.value("key_changed").toBool();
        d.seen     = d.current ? QStringLiteral("now")
                               : humanizeSeen(o.value("last_seen"));
        devices_.push_back(std::move(d));
    }
    endResetModel();
    emit countChanged();
}

void DeviceMeshModel::clear() {
    if (devices_.isEmpty()) return;
    beginResetModel();
    devices_.clear();
    endResetModel();
    emit countChanged();
}

void DeviceMeshModel::rename(const QString& deviceId, const QString& newName) {
    if (!cloud_) return;
    const int i = indexOfId(deviceId);
    if (i < 0) return;
    const Device& d = devices_[i];
    // Re-register the same identity with the new name — an upsert keyed by
    // device_id.  Passing the row's existing pubkey keeps key_changed false.
    cloud_->registerDevice(deviceId, newName, d.os, d.pubkey, d.peerCode);
    log::info("DeviceMesh", "rename %s -> '%s'",
              deviceId.toUtf8().constData(), newName.toUtf8().constData());
}

void DeviceMeshModel::remove(const QString& deviceId) {
    if (!cloud_) return;
    cloud_->deleteDevice(deviceId);
    log::info("DeviceMesh", "remove %s", deviceId.toUtf8().constData());
}

} // namespace vivora::gui
