#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QString>
#include <QVector>

namespace vivora::gui {

// Persisted list of remembered peers, shown in the main window's "Recent"
// section.  Lives in a JSON file under the per-user app config directory:
//   Win:   %APPDATA%\Vivora\peers.json
//   Linux: ~/.config/vivora/peers.json
//   Mac:   ~/Library/Application Support/Vivora/peers.json
//
// The pubkey hex is the stable identity — peer codes can change on
// reinstall.  Alias starts empty for new entries; the user can rename
// later from the UI.

// Per-entry direction of the most recent interaction.  Drives the
// ↑ / ↓ glyph in the Recent list — incoming = peer connected to us
// (we accepted), outgoing = we initiated.  Unknown for entries
// migrated from before the field landed.
enum class PeerDirection : int { Unknown = 0, Outgoing = 1, Incoming = 2 };

struct Peer {
    QString       alias;
    QString       pubkeyHex;       // 64 lowercase hex chars, stable identity
    QString       lastPeerCode;    // memorable code at last contact (for display)
    QDateTime     lastSeen;
    PeerDirection lastDirection = PeerDirection::Unknown;
};

class AddressBook : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        AliasRole = Qt::UserRole + 1,
        PubkeyRole,
        CodeRole,
        LastSeenRole,
        DirectionRole,
    };

    explicit AddressBook(QObject* parent = nullptr);

    // QAbstractListModel
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& idx, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Mutators — all persist immediately.
    // touch() is idempotent on pubkey: if the entry exists, updates
    // lastSeen + lastPeerCode in place.  Otherwise inserts a fresh entry
    // with empty alias (Option A from the design discussion — minimal
    // friction, user renames later).
    Q_INVOKABLE void touch(const QString& pubkeyHex, const QString& lastPeerCode);
    // Same as touch() but also records the connection direction so the
    // Recent list can show ↑ outgoing / ↓ incoming.  AppController
    // calls touchOutgoing on user-initiated connect, touchIncoming on
    // accepted host approval.
    void touchOutgoing(const QString& pubkeyHex, const QString& lastPeerCode);
    void touchIncoming(const QString& pubkeyHex, const QString& lastPeerCode);
    Q_INVOKABLE void setAlias(int row, const QString& alias);
    Q_INVOKABLE void remove(int row);

    // For C++ callers (AppController, primarily).
    const Peer* findByPubkey(const QString& pubkeyHex) const;
    const Peer* findByCode  (const QString& peerCode) const;

private:
    // Shared impl behind touch / touchOutgoing / touchIncoming.  dir
    // == Unknown leaves an existing entry's direction untouched.
    void applyTouch(const QString& pubkeyHex, const QString& lastPeerCode,
                    PeerDirection dir);
    void load();
    void save() const;
    QString filePath() const;

    QVector<Peer> peers_;
};

} // namespace vivora::gui
