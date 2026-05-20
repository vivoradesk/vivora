#pragma once

#include <QObject>
#include <QSettings>
#include <QString>

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

signals:
    void changed();

private:
    QSettings q_;
};

} // namespace vivora::gui
