#include "app/gui/address_book.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace vivora::gui {

AddressBook::AddressBook(QObject* parent) : QAbstractListModel(parent) {
    load();
}

int AddressBook::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return peers_.size();
}

QVariant AddressBook::data(const QModelIndex& idx, int role) const {
    if (!idx.isValid() || idx.row() < 0 || idx.row() >= peers_.size()) return {};
    const Peer& p = peers_[idx.row()];
    switch (role) {
    case AliasRole:    return p.alias;
    case PubkeyRole:   return p.pubkeyHex;
    case CodeRole:     return p.lastPeerCode;
    case LastSeenRole: return p.lastSeen;
    case DirectionRole: return static_cast<int>(p.lastDirection);
    case Qt::DisplayRole: return p.alias.isEmpty() ? p.lastPeerCode : p.alias;
    default: return {};
    }
}

QHash<int, QByteArray> AddressBook::roleNames() const {
    return {
        {AliasRole,     "alias"},
        {PubkeyRole,    "pubkey"},
        {CodeRole,      "code"},
        {LastSeenRole,  "lastSeen"},
        {DirectionRole, "direction"},
    };
}

void AddressBook::applyTouch(const QString& pubkeyHex,
                             const QString& lastPeerCode,
                             PeerDirection dir) {
    if (pubkeyHex.isEmpty()) return;
    for (int i = 0; i < peers_.size(); ++i) {
        if (peers_[i].pubkeyHex == pubkeyHex) {
            peers_[i].lastPeerCode = lastPeerCode;
            peers_[i].lastSeen     = QDateTime::currentDateTimeUtc();
            if (dir != PeerDirection::Unknown) peers_[i].lastDirection = dir;
            emit dataChanged(index(i), index(i),
                {CodeRole, LastSeenRole, DirectionRole, Qt::DisplayRole});
            save();
            return;
        }
    }
    Peer p;
    p.pubkeyHex     = pubkeyHex;
    p.lastPeerCode  = lastPeerCode;
    p.lastSeen      = QDateTime::currentDateTimeUtc();
    p.lastDirection = dir;
    beginInsertRows({}, peers_.size(), peers_.size());
    peers_.push_back(p);
    endInsertRows();
    save();
}

void AddressBook::touch(const QString& pubkeyHex, const QString& lastPeerCode) {
    applyTouch(pubkeyHex, lastPeerCode, PeerDirection::Unknown);
}

void AddressBook::touchOutgoing(const QString& pubkeyHex, const QString& lastPeerCode) {
    applyTouch(pubkeyHex, lastPeerCode, PeerDirection::Outgoing);
}

void AddressBook::touchIncoming(const QString& pubkeyHex, const QString& lastPeerCode) {
    applyTouch(pubkeyHex, lastPeerCode, PeerDirection::Incoming);
}

void AddressBook::setAlias(int row, const QString& alias) {
    if (row < 0 || row >= peers_.size()) return;
    if (peers_[row].alias == alias) return;
    peers_[row].alias = alias;
    emit dataChanged(index(row), index(row), {AliasRole, Qt::DisplayRole});
    save();
}

void AddressBook::remove(int row) {
    if (row < 0 || row >= peers_.size()) return;
    beginRemoveRows({}, row, row);
    peers_.remove(row);
    endRemoveRows();
    save();
}

const Peer* AddressBook::findByPubkey(const QString& pubkeyHex) const {
    for (const auto& p : peers_) {
        if (p.pubkeyHex == pubkeyHex) return &p;
    }
    return nullptr;
}

const Peer* AddressBook::findByCode(const QString& peerCode) const {
    for (const auto& p : peers_) {
        if (p.lastPeerCode == peerCode) return &p;
    }
    return nullptr;
}

QString AddressBook::filePath() const {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(base);  // idempotent
    return base + QStringLiteral("/peers.json");
}

void AddressBook::load() {
    QFile f(filePath());
    if (!f.open(QIODevice::ReadOnly)) return;     // first run — empty list
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) return;
    for (const QJsonValue& v : doc.array()) {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        Peer p;
        p.alias         = o.value("alias").toString();
        p.pubkeyHex     = o.value("pubkey").toString();
        p.lastPeerCode  = o.value("code").toString();
        p.lastSeen      = QDateTime::fromString(o.value("lastSeen").toString(), Qt::ISODate);
        p.lastDirection = static_cast<PeerDirection>(o.value("direction").toInt(0));
        if (!p.pubkeyHex.isEmpty()) peers_.push_back(p);
    }
}

void AddressBook::save() const {
    QJsonArray arr;
    for (const auto& p : peers_) {
        QJsonObject o;
        o["alias"]     = p.alias;
        o["pubkey"]    = p.pubkeyHex;
        o["code"]      = p.lastPeerCode;
        o["lastSeen"]  = p.lastSeen.toString(Qt::ISODate);
        o["direction"] = static_cast<int>(p.lastDirection);
        arr.append(o);
    }
    QFile f(filePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
}

} // namespace vivora::gui
