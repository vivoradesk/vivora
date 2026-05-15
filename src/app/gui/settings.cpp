#include "app/gui/settings.h"

#include <QCoreApplication>

namespace deskbeam::gui {

namespace {
constexpr const char* K_RENDEZVOUS         = "network/rendezvous";
constexpr const char* K_RELAY              = "network/relay";
constexpr const char* K_LICENSE            = "network/licenseFile";
constexpr const char* K_STUN               = "network/stunServer";
constexpr const char* K_CODEC              = "capture/codecIndex";
constexpr const char* K_ENCODER            = "capture/encoderIndex";
constexpr const char* K_BITRATE            = "capture/bitrateMbps";
constexpr const char* K_DISPLAY            = "capture/displayIndex";
constexpr const char* K_HOST_PORT          = "network/hostPort";
constexpr const char* K_IDLE_TIMEOUT_MIN   = "idle/timeoutMin";
constexpr const char* K_IDLE_WARNING_SEC   = "idle/warningSec";
constexpr const char* K_START_SHARING      = "general/startSharingOnLaunch";
constexpr const char* K_MIN_TO_TRAY        = "general/minimizeToTray";
constexpr const char* K_START_AT_LOGIN     = "general/startAtLogin";

// Defaults.  Pre-fill rendezvous + relay with the public Oracle endpoints
// so a fresh install talks to the same infra the CLI uses by default.
// Self-hosters override both via the Settings dialog.
constexpr const char* DEF_RENDEZVOUS = "89.168.124.37:7000";
constexpr const char* DEF_RELAY      = "89.168.124.37:7100";
constexpr const char* DEF_LICENSE    = "";
constexpr const char* DEF_STUN       = "stun.l.google.com:19302";
constexpr int   DEF_CODEC            = 1;            // hevc
constexpr int   DEF_ENCODER          = 0;            // auto
constexpr int   DEF_BITRATE          = 0;            // auto
constexpr int   DEF_DISPLAY          = 0;
// 9876 was the historical default; bumped to 9900 because Windows kept
// keeping the old port in TIME_WAIT-like state across crashed launches
// during P5 testing, which silently broke direct LAN.  Settable from
// the UI for users with a port conflict on the new default.
constexpr int   DEF_HOST_PORT        = 9900;
constexpr int   DEF_IDLE_TIMEOUT_MIN = 10;
constexpr int   DEF_IDLE_WARNING_SEC = 30;
constexpr bool  DEF_START_SHARING    = false;
constexpr bool  DEF_MIN_TO_TRAY      = true;
constexpr bool  DEF_START_AT_LOGIN   = false;
} // namespace

Settings::Settings(QObject* parent)
    : QObject(parent),
      q_("DeskBeam", "DeskBeam") {
    // QCoreApplication org/name fields are also used by other places
    // (peer pin file, etc.); set them once at app startup for clarity.
    QCoreApplication::setOrganizationName("DeskBeam");
    QCoreApplication::setApplicationName("DeskBeam");
}

QString Settings::rendezvous() const          { return q_.value(K_RENDEZVOUS, DEF_RENDEZVOUS).toString(); }
void Settings::setRendezvous(const QString& v){ if (v != rendezvous()) { q_.setValue(K_RENDEZVOUS, v); emit changed(); } }

QString Settings::relay() const               { return q_.value(K_RELAY, DEF_RELAY).toString(); }
void Settings::setRelay(const QString& v)     { if (v != relay()) { q_.setValue(K_RELAY, v); emit changed(); } }

QString Settings::licenseFile() const         { return q_.value(K_LICENSE, DEF_LICENSE).toString(); }
void Settings::setLicenseFile(const QString& v){ if (v != licenseFile()) { q_.setValue(K_LICENSE, v); emit changed(); } }

QString Settings::stunServer() const          { return q_.value(K_STUN, DEF_STUN).toString(); }
void Settings::setStunServer(const QString& v){ if (v != stunServer()) { q_.setValue(K_STUN, v); emit changed(); } }

int Settings::codecIndex() const              { return q_.value(K_CODEC, DEF_CODEC).toInt(); }
void Settings::setCodecIndex(int v)           { if (v != codecIndex()) { q_.setValue(K_CODEC, v); emit changed(); } }

int Settings::encoderIndex() const            { return q_.value(K_ENCODER, DEF_ENCODER).toInt(); }
void Settings::setEncoderIndex(int v)         { if (v != encoderIndex()) { q_.setValue(K_ENCODER, v); emit changed(); } }

int Settings::bitrateMbps() const             { return q_.value(K_BITRATE, DEF_BITRATE).toInt(); }
void Settings::setBitrateMbps(int v)          { if (v != bitrateMbps()) { q_.setValue(K_BITRATE, v); emit changed(); } }

int Settings::displayIndex() const            { return q_.value(K_DISPLAY, DEF_DISPLAY).toInt(); }
void Settings::setDisplayIndex(int v)         { if (v != displayIndex()) { q_.setValue(K_DISPLAY, v); emit changed(); } }

int Settings::hostPort() const                { return q_.value(K_HOST_PORT, DEF_HOST_PORT).toInt(); }
void Settings::setHostPort(int v)             { if (v != hostPort()) { q_.setValue(K_HOST_PORT, v); emit changed(); } }

int Settings::idleTimeoutMin() const          { return q_.value(K_IDLE_TIMEOUT_MIN, DEF_IDLE_TIMEOUT_MIN).toInt(); }
void Settings::setIdleTimeoutMin(int v)       { if (v != idleTimeoutMin()) { q_.setValue(K_IDLE_TIMEOUT_MIN, v); emit changed(); } }

int Settings::idleWarningSec() const          { return q_.value(K_IDLE_WARNING_SEC, DEF_IDLE_WARNING_SEC).toInt(); }
void Settings::setIdleWarningSec(int v)       { if (v != idleWarningSec()) { q_.setValue(K_IDLE_WARNING_SEC, v); emit changed(); } }

bool Settings::startSharingOnLaunch() const   { return q_.value(K_START_SHARING, DEF_START_SHARING).toBool(); }
void Settings::setStartSharingOnLaunch(bool v){ if (v != startSharingOnLaunch()) { q_.setValue(K_START_SHARING, v); emit changed(); } }

bool Settings::minimizeToTray() const         { return q_.value(K_MIN_TO_TRAY, DEF_MIN_TO_TRAY).toBool(); }
void Settings::setMinimizeToTray(bool v)      { if (v != minimizeToTray()) { q_.setValue(K_MIN_TO_TRAY, v); emit changed(); } }

bool Settings::startAtLogin() const           { return q_.value(K_START_AT_LOGIN, DEF_START_AT_LOGIN).toBool(); }
void Settings::setStartAtLogin(bool v)        { if (v != startAtLogin()) { q_.setValue(K_START_AT_LOGIN, v); emit changed(); } }

} // namespace deskbeam::gui
