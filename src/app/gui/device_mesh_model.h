#pragma once

#include <QAbstractListModel>
#include <QJsonArray>
#include <QVariantMap>
#include <QVector>

namespace vivora::gui {

class CloudClient;

// Account device-mesh list (VIV-52).  Populated from the /devices/me JSON and
// exposed to QML with the same role names the "My Devices" UI already consumes
// (devId, devName, os, online, current, warned, seen, peerCode, host), so the
// QML delegates work unchanged whether they read this real model or the mock.
//
// `seen` is a humanised last_seen ("now" / "3h ago" / "2d ago").  `host` has no
// dedicated backend field — we surface the peer code there, which is what the
// Settings table shows in place of the mock's fake IP.
//
// rename()/remove() call the cloud API via a borrowed CloudClient.  The caller
// (AppController) re-fetches on the resulting register/delete/SSE signals, so
// this model does not trigger its own refresh.
class DeviceMeshModel : public QAbstractListModel {
    Q_OBJECT
    // `count` + `get()` mirror the QML ListModel surface the mock exposed, so
    // MyDevicesBlock's online-count loop and count pill bind to either model.
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        DevIdRole = Qt::UserRole + 1,
        DevNameRole,
        OsRole,
        OnlineRole,
        CurrentRole,
        WarnedRole,
        SeenRole,
        PeerCodeRole,
        HostRole,
        PubkeyRole,
    };

    explicit DeviceMeshModel(QObject* parent = nullptr);

    // Borrowed; must outlive the model.  Used by rename()/remove().
    void setCloud(CloudClient* cloud) { cloud_ = cloud; }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(devices_.size()); }

    // Row snapshot as a role->value map (QML `model.get(i)` compatibility).
    Q_INVOKABLE QVariantMap get(int row) const;

    // Rebuild from a /devices/me `devices` array.
    void setDevices(const QJsonArray& devices);
    void clear();

    // VIV-52 trust: true iff some entry's pubkey matches `pubkeyHex`
    // (CASE-INSENSITIVE) AND that entry is not key_changed (`!warned`).
    // Drives host-side auto-accept and viewer-side pre-pin — an account
    // device whose key changed is deliberately excluded so it falls back to
    // the normal approval / TOFU path.  Ignores online/current.
    bool containsActivePubkey(const QString& pubkeyHex) const;

    // Rename is an upsert with the row's existing os/pubkey/peer_code so the
    // server keeps the same key (no key_changed).  Remove deletes remotely.
    // Both no-op without a CloudClient; the refresh is driven by the caller.
    Q_INVOKABLE void rename(const QString& deviceId, const QString& newName);
    Q_INVOKABLE void remove(const QString& deviceId);

signals:
    void countChanged();

private:
    struct Device {
        QString devId;
        QString name;
        QString os;
        QString pubkey;
        QString peerCode;
        bool    online  = false;
        bool    current = false;
        bool    warned  = false;   // key_changed
        QString seen;              // pre-humanised for display
    };

    static QString humanizeSeen(const QJsonValue& lastSeen);
    int indexOfId(const QString& deviceId) const;

    QVector<Device> devices_;
    CloudClient*    cloud_ = nullptr;
};

} // namespace vivora::gui
