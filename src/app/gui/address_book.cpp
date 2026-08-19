#include "app/gui/address_book.h"

#include "common/crypto/peer_pin.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <algorithm>

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
    case SeenRole:     return p.seen;
    case TrustedRole:  return p.trusted;
    case PinnedRole:   return p.pinned;
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
        {SeenRole,      "seen"},
        {TrustedRole,   "trusted"},
        {PinnedRole,    "pinned"},
    };
}

void AddressBook::sortPeers() {
    // Pinned entries first; within each pinned/unpinned group the most
    // recently seen entry floats to the top.  stable_sort keeps the prior
    // relative order for entries with equal lastSeen.
    std::stable_sort(peers_.begin(), peers_.end(),
                     [](const Peer& a, const Peer& b) {
                         if (a.pinned != b.pinned) return a.pinned;
                         return a.lastSeen > b.lastSeen;
                     });
}

void AddressBook::applyTouch(const QString& pubkeyHex,
                             const QString& lastPeerCode,
                             PeerDirection dir) {
    if (pubkeyHex.isEmpty()) return;
    // Touching an entry bumps its lastSeen, which can change its position in
    // the sorted list, so both branches re-sort inside a model reset.
    for (int i = 0; i < peers_.size(); ++i) {
        if (peers_[i].pubkeyHex == pubkeyHex) {
            beginResetModel();
            peers_[i].lastPeerCode = lastPeerCode;
            peers_[i].lastSeen     = QDateTime::currentDateTimeUtc();
            peers_[i].seen        += 1;
            if (dir != PeerDirection::Unknown) peers_[i].lastDirection = dir;
            sortPeers();
            endResetModel();
            save();
            return;
        }
    }
    Peer p;
    p.pubkeyHex     = pubkeyHex;
    p.lastPeerCode  = lastPeerCode;
    p.lastSeen      = QDateTime::currentDateTimeUtc();
    p.lastDirection = dir;
    p.seen          = 1;
    beginResetModel();
    peers_.push_back(p);
    sortPeers();
    endResetModel();
    emit countChanged();
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

void AddressBook::setTrustedByPubkey(const QString& pubkeyHex, bool trusted) {
    if (pubkeyHex.isEmpty()) return;
    for (int i = 0; i < peers_.size(); ++i) {
        if (peers_[i].pubkeyHex == pubkeyHex) {
            if (peers_[i].trusted == trusted) return;
            peers_[i].trusted = trusted;
            emit dataChanged(index(i), index(i), {TrustedRole});
            save();
            return;
        }
    }
}

void AddressBook::setGrantByPubkey(const QString& pubkeyHex,
                                   bool input, bool clipboard, bool audio, bool file) {
    if (pubkeyHex.isEmpty()) return;
    for (int i = 0; i < peers_.size(); ++i) {
        if (peers_[i].pubkeyHex == pubkeyHex) {
            peers_[i].grantInput     = input;
            peers_[i].grantClipboard = clipboard;
            peers_[i].grantAudio     = audio;
            peers_[i].grantFile      = file;
            save();
            return;
        }
    }
}

void AddressBook::setTrusted(int row, bool trusted) {
    if (row < 0 || row >= peers_.size()) return;
    if (peers_[row].trusted == trusted) return;
    peers_[row].trusted = trusted;
    emit dataChanged(index(row), index(row), {TrustedRole});
    save();
}

void AddressBook::setPinned(int row, bool pinned) {
    if (row < 0 || row >= peers_.size()) return;
    if (peers_[row].pinned == pinned) return;
    // Toggling pinned changes the row's sort group, so re-sort under a reset.
    beginResetModel();
    peers_[row].pinned = pinned;
    sortPeers();
    endResetModel();
    save();
}

void AddressBook::remove(int row) {
    if (row < 0 || row >= peers_.size()) return;
    // VIV-23: "Forget" also drops the TOFU pin from known_peers.txt so the
    // next connect re-runs the first-time trust prompt instead of silently
    // matching (or worse, mismatching) a key the user asked us to forget.
    // Matched on both the last-seen code and the stable pubkey.
    crypto::forget_peer_pin(peers_[row].lastPeerCode.toStdString(),
                            peers_[row].pubkeyHex.toStdString());
    beginRemoveRows({}, row, row);
    peers_.remove(row);
    endRemoveRows();
    emit countChanged();
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
        p.seen          = o.value("seen").toInt(0);
        p.trusted       = o.value("trusted").toBool(false);
        p.grantInput     = o.value("grantInput").toBool(true);
        p.grantClipboard = o.value("grantClipboard").toBool(true);
        // VIV-65: default TRUE when absent — peers approved through the old
        // dialog have no grantAudio persisted, and they must keep receiving
        // host audio (as before this grant existed) until re-approved.
        p.grantAudio     = o.value("grantAudio").toBool(true);
        p.grantFile      = o.value("grantFile").toBool(false);
        p.pinned         = o.value("pinned").toBool(false);
        if (!p.pubkeyHex.isEmpty()) peers_.push_back(p);
    }
    // Establish the pinned-first / most-recent-first order up front.  Called
    // from the constructor, so no model-reset signals are needed here.
    sortPeers();
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
        o["seen"]      = p.seen;
        o["trusted"]   = p.trusted;
        o["grantInput"]     = p.grantInput;
        o["grantClipboard"] = p.grantClipboard;
        o["grantAudio"]     = p.grantAudio;
        o["grantFile"]      = p.grantFile;
        o["pinned"]         = p.pinned;
        arr.append(o);
    }
    QFile f(filePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
}

} // namespace vivora::gui
