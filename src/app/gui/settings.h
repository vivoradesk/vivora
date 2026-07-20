#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>

namespace vivora::gui {

// Persistent settings exposed to QML.  Backed by QSettings (writes to a
// per-user ini file or registry, platform-conventional location).  Every
// setter writes-through immediately; the changed() signal fires on every
// mutation so QML bindings stay live.
//
// Setting keys mirror the CLI flag names where reasonable — easier to
// document and to translate between headless and GUI sessions.
class Settings : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString rendezvous      READ rendezvous      WRITE setRendezvous      NOTIFY changed)
    Q_PROPERTY(QString relay           READ relay           WRITE setRelay           NOTIFY changed)
    Q_PROPERTY(QString licenseFile     READ licenseFile     WRITE setLicenseFile     NOTIFY changed)
    Q_PROPERTY(QString stunServer      READ stunServer      WRITE setStunServer      NOTIFY changed)
    Q_PROPERTY(int     codecIndex      READ codecIndex      WRITE setCodecIndex      NOTIFY changed)   // 0=h264, 1=hevc
    Q_PROPERTY(int     encoderIndex    READ encoderIndex    WRITE setEncoderIndex    NOTIFY changed)   // 0=auto, 1=amf, 2=nvenc, 3=qsv
    Q_PROPERTY(int     bitrateMbps     READ bitrateMbps     WRITE setBitrateMbps     NOTIFY changed)   // 0 = auto
    Q_PROPERTY(int     displayIndex    READ displayIndex    WRITE setDisplayIndex    NOTIFY changed)
    Q_PROPERTY(int     hostPort        READ hostPort        WRITE setHostPort        NOTIFY changed)
    Q_PROPERTY(int     idleTimeoutMin  READ idleTimeoutMin  WRITE setIdleTimeoutMin  NOTIFY changed)
    Q_PROPERTY(int     idleWarningSec  READ idleWarningSec  WRITE setIdleWarningSec  NOTIFY changed)
    Q_PROPERTY(bool    startSharingOnLaunch READ startSharingOnLaunch
                                            WRITE setStartSharingOnLaunch NOTIFY changed)
    Q_PROPERTY(bool    minimizeToTray  READ minimizeToTray  WRITE setMinimizeToTray  NOTIFY changed)
    Q_PROPERTY(bool    startAtLogin    READ startAtLogin    WRITE setStartAtLogin    NOTIFY changed)
    // VIV-18: true when this build has a start-at-login backend for the
    // current OS (Windows today; macOS/Linux are a separate future issue).
    // The QML toggle stays disabled when unsupported.
    Q_PROPERTY(bool    startAtLoginSupported READ startAtLoginSupported CONSTANT)
    // VIV-53/59 Security.  approvalMode: 0=always_prompt, 1=prompt_unknown_only,
    // 2=auto_accept.  singleSessionLock: reject new clients while a session
    // is live.
    Q_PROPERTY(int     approvalMode    READ approvalMode    WRITE setApprovalMode    NOTIFY changed)
    Q_PROPERTY(bool    singleSessionLock READ singleSessionLock WRITE setSingleSessionLock NOTIFY changed)
    // VIV-65: initial value of the approval dialog's Audio grant toggle.
    // When on, freshly prompted connections default to receiving host audio.
    Q_PROPERTY(bool    audioGrantDefault READ audioGrantDefault WRITE setAudioGrantDefault NOTIFY changed)
    // Appearance.  theme: 0=light, 1=dark, 2=system.  Stored now; the
    // app is light-only today, dark/system land with the theme engine.
    Q_PROPERTY(int     theme           READ theme           WRITE setTheme           NOTIFY changed)
    // Codec.  hdrPassthrough: carry HDR10 metadata when the host display
    // is HDR (the existing FP16→HEVC Main10 auto-promote path).
    Q_PROPERTY(bool    hdrPassthrough  READ hdrPassthrough  WRITE setHdrPassthrough  NOTIFY changed)
    // VIV-67: stream framerate cap in fps (30/60/90/120/144).  The host
    // paces capture+encode to at most this rate; client-driven adaptive
    // framerate can only lower the effective rate, never exceed it.
    Q_PROPERTY(int     hostFps         READ hostFps         WRITE setHostFps         NOTIFY changed)
    // Viewing caps (client side, 0 = none).  fpsCap bounds the client's
    // adaptive framerate request; maxKbps is sent to the host as a hard
    // bitrate clamp for streams this client watches.
    Q_PROPERTY(int     viewFpsCap      READ viewFpsCap      WRITE setViewFpsCap      NOTIFY changed)
    Q_PROPERTY(int     viewMaxKbps     READ viewMaxKbps     WRITE setViewMaxKbps     NOTIFY changed)

public:
    explicit Settings(QObject* parent = nullptr);

    QString rendezvous() const;          void setRendezvous(const QString&);
    QString relay() const;               void setRelay(const QString&);
    QString licenseFile() const;         void setLicenseFile(const QString&);
    QString stunServer() const;          void setStunServer(const QString&);
    int     codecIndex() const;          void setCodecIndex(int);
    int     encoderIndex() const;        void setEncoderIndex(int);
    int     bitrateMbps() const;         void setBitrateMbps(int);
    int     displayIndex() const;        void setDisplayIndex(int);
    int     hostPort() const;            void setHostPort(int);
    int     idleTimeoutMin() const;      void setIdleTimeoutMin(int);
    int     idleWarningSec() const;      void setIdleWarningSec(int);
    bool    startSharingOnLaunch() const;void setStartSharingOnLaunch(bool);
    bool    minimizeToTray() const;      void setMinimizeToTray(bool);
    bool    startAtLogin() const;        void setStartAtLogin(bool);
    bool    startAtLoginSupported() const;
    int     approvalMode() const;        void setApprovalMode(int);
    bool    singleSessionLock() const;   void setSingleSessionLock(bool);
    bool    audioGrantDefault() const;   void setAudioGrantDefault(bool);
    int     theme() const;               void setTheme(int);
    bool    hdrPassthrough() const;      void setHdrPassthrough(bool);
    int     hostFps() const;             void setHostFps(int);
    int     viewFpsCap() const;          void setViewFpsCap(int);
    int     viewMaxKbps() const;         void setViewMaxKbps(int);

    // VIV-31 account/license cloud.  Persisted but not exposed as Q_PROPERTY
    // (QML talks to AppController's account state); the account setters do not
    // fire changed() so they don't trigger a license re-verify loop.
    QString cloudUrl() const;            void setCloudUrl(const QString&);
    QString accountToken() const;        void setAccountToken(const QString&);
    QString accountEmail() const;        void setAccountEmail(const QString&);
    QString accountUserId() const;       void setAccountUserId(const QString&);

    // VIV-70 announcements: ids the user has already seen / dismissed so the
    // launch modal doesn't re-show them.  Not Q_PROPERTY.
    QStringList seenAnnouncements() const;       void addSeenAnnouncement(const QString&);
    QStringList dismissedAnnouncements() const;  void addDismissedAnnouncement(const QString&);

    // VIV-71 polls: a stable per-install id for anonymous vote dedup, plus the
    // ids the user has already answered so the launch modal doesn't re-prompt.
    QString     clientId();                       // generates + persists on first use
    QStringList answeredPolls() const;            void addAnsweredPoll(const QString&);

signals:
    void changed();

private:
    QSettings q_;
};

} // namespace vivora::gui
