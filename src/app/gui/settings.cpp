#include "app/gui/settings.h"

#include "app/gui/autostart.h"

#include <QCoreApplication>
#include <QUuid>

namespace vivora::gui {

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
constexpr const char* K_APPROVAL_MODE      = "security/approvalMode";
constexpr const char* K_SINGLE_SESSION     = "security/singleSessionLock";
constexpr const char* K_THEME              = "appearance/theme";
constexpr const char* K_HDR_PASSTHROUGH    = "capture/hdrPassthrough";
constexpr const char* K_HOST_FPS           = "capture/hostFps";
constexpr const char* K_VIEW_FPS_CAP       = "viewing/fpsCap";
constexpr const char* K_VIEW_MAX_KBPS      = "viewing/maxKbps";
constexpr const char* K_CLOUD_URL          = "account/cloudUrl";
constexpr const char* K_ACCOUNT_TOKEN      = "account/token";
constexpr const char* K_ACCOUNT_EMAIL      = "account/email";
constexpr const char* K_ACCOUNT_USERID     = "account/userId";

// Defaults.  Pre-fill rendezvous + relay with the Vivora-managed public
// endpoints so a fresh install talks to the same infra the CLI uses by
// default.  DNS names (Cloudflare A-records pointing at the Oracle box)
// instead of raw 89.168.124.37 so the IP can move without a client
// re-release.  Self-hosters override both via the Settings dialog.
constexpr const char* DEF_RENDEZVOUS = "rdv.vivora.dev:7000";
constexpr const char* DEF_RELAY      = "relay.vivora.dev:7100";
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
constexpr int   DEF_APPROVAL_MODE    = 0;   // always_prompt — safest default
constexpr bool  DEF_SINGLE_SESSION   = false;
constexpr int   DEF_THEME            = 0;   // light
constexpr bool  DEF_HDR_PASSTHROUGH  = true;
constexpr int   DEF_HOST_FPS         = 60;   // VIV-67 stream framerate cap
constexpr const char* DEF_CLOUD_URL  = "https://cloud.vivora.dev";
} // namespace

Settings::Settings(QObject* parent)
    : QObject(parent),
      q_("Vivora", "Vivora") {
    // QCoreApplication org/name fields are also used by other places
    // (peer pin file, etc.); set them once at app startup for clarity.
    QCoreApplication::setOrganizationName("Vivora");
    QCoreApplication::setOrganizationDomain("vivora.dev");
    QCoreApplication::setApplicationName("Vivora");

    // VIV-18 start-at-login: the OS registration (HKCU Run key on Windows)
    // is the source of truth, not our ini — the user can remove it via Task
    // Manager / regedit while Vivora isn't running.  Reconcile on startup so
    // the checkbox reflects reality, and re-register while enabled so a
    // moved/updated vivora.exe self-heals its stale registry path.
    if (Autostart::supported()) {
        const bool reg = Autostart::enabled();
        if (reg != q_.value(K_START_AT_LOGIN, DEF_START_AT_LOGIN).toBool())
            q_.setValue(K_START_AT_LOGIN, reg);
        if (reg)
            Autostart::setEnabled(true);
    }
}

QString Settings::rendezvous() const          { return q_.value(K_RENDEZVOUS, DEF_RENDEZVOUS).toString(); }
void Settings::setRendezvous(const QString& v){ if (v != rendezvous()) { q_.setValue(K_RENDEZVOUS, v); emit changed(); } }

QString Settings::relay() const               { return q_.value(K_RELAY, DEF_RELAY).toString(); }
void Settings::setRelay(const QString& v)     { if (v != relay()) { q_.setValue(K_RELAY, v); emit changed(); } }

QString Settings::licenseFile() const         { return q_.value(K_LICENSE, DEF_LICENSE).toString(); }
void Settings::setLicenseFile(const QString& v){ if (v != licenseFile()) { q_.setValue(K_LICENSE, v); emit changed(); } }

// Account/cloud state — write-through, no changed() (internal, not a UI knob).
QString Settings::cloudUrl() const             { return q_.value(K_CLOUD_URL, DEF_CLOUD_URL).toString(); }
void Settings::setCloudUrl(const QString& v)   { q_.setValue(K_CLOUD_URL, v); }
QString Settings::accountToken() const         { return q_.value(K_ACCOUNT_TOKEN).toString(); }
void Settings::setAccountToken(const QString& v){ q_.setValue(K_ACCOUNT_TOKEN, v); }
QString Settings::accountEmail() const         { return q_.value(K_ACCOUNT_EMAIL).toString(); }
void Settings::setAccountEmail(const QString& v){ q_.setValue(K_ACCOUNT_EMAIL, v); }
QString Settings::accountUserId() const        { return q_.value(K_ACCOUNT_USERID).toString(); }
void Settings::setAccountUserId(const QString& v){ q_.setValue(K_ACCOUNT_USERID, v); }

QString Settings::stunServer() const          { return q_.value(K_STUN, DEF_STUN).toString(); }
void Settings::setStunServer(const QString& v){ if (v != stunServer()) { q_.setValue(K_STUN, v); emit changed(); } }

int Settings::codecIndex() const              { return q_.value(K_CODEC, DEF_CODEC).toInt(); }
void Settings::setCodecIndex(int v)           { if (v != codecIndex()) { q_.setValue(K_CODEC, v); emit changed(); } }

int Settings::encoderIndex() const            { return q_.value(K_ENCODER, DEF_ENCODER).toInt(); }
void Settings::setEncoderIndex(int v)         { if (v != encoderIndex()) { q_.setValue(K_ENCODER, v); emit changed(); } }

// Sanitised on read: 0 = auto, manual values clamp to [1, 100] Mbps so a
// hand-edited ini (or a stale pre-slider value) can't push the encoder
// into a degenerate rate.
int Settings::bitrateMbps() const {
    int v = q_.value(K_BITRATE, DEF_BITRATE).toInt();
    if (v <= 0)   return 0;
    if (v > 100)  return 100;
    return v;
}
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
void Settings::setStartAtLogin(bool v)        { if (v != startAtLogin()) { q_.setValue(K_START_AT_LOGIN, v); Autostart::setEnabled(v); emit changed(); } }
bool Settings::startAtLoginSupported() const  { return Autostart::supported(); }

int Settings::approvalMode() const            { return q_.value(K_APPROVAL_MODE, DEF_APPROVAL_MODE).toInt(); }
void Settings::setApprovalMode(int v)         { if (v != approvalMode()) { q_.setValue(K_APPROVAL_MODE, v); emit changed(); } }

bool Settings::singleSessionLock() const      { return q_.value(K_SINGLE_SESSION, DEF_SINGLE_SESSION).toBool(); }
void Settings::setSingleSessionLock(bool v)   { if (v != singleSessionLock()) { q_.setValue(K_SINGLE_SESSION, v); emit changed(); } }

int Settings::theme() const                   { return q_.value(K_THEME, DEF_THEME).toInt(); }
void Settings::setTheme(int v)                { if (v != theme()) { q_.setValue(K_THEME, v); emit changed(); } }

bool Settings::hdrPassthrough() const         { return q_.value(K_HDR_PASSTHROUGH, DEF_HDR_PASSTHROUGH).toBool(); }
void Settings::setHdrPassthrough(bool v)      { if (v != hdrPassthrough()) { q_.setValue(K_HDR_PASSTHROUGH, v); emit changed(); } }

// VIV-67 stream framerate cap.  Sanitised on read so a hand-edited ini
// can't push the host loop into a degenerate cadence (0 fps → div-by-zero,
// 1000 fps → busy spin).
int Settings::hostFps() const {
    int v = q_.value(K_HOST_FPS, DEF_HOST_FPS).toInt();
    if (v < 15)  return 15;
    if (v > 240) return 240;
    return v;
}
void Settings::setHostFps(int v)              { if (v != hostFps()) { q_.setValue(K_HOST_FPS, v); emit changed(); } }

// Viewing caps (client side).  0 = no cap.  Sanitised on read like
// hostFps; nonzero fps caps clamp into [15, 240], bitrate into
// [500 kbps, 500 Mbps].
int Settings::viewFpsCap() const {
    int v = q_.value(K_VIEW_FPS_CAP, 0).toInt();
    if (v <= 0)  return 0;
    if (v < 15)  return 15;
    if (v > 240) return 240;
    return v;
}
void Settings::setViewFpsCap(int v)           { if (v != viewFpsCap()) { q_.setValue(K_VIEW_FPS_CAP, v); emit changed(); } }
int Settings::viewMaxKbps() const {
    int v = q_.value(K_VIEW_MAX_KBPS, 0).toInt();
    if (v <= 0)       return 0;
    if (v < 500)      return 500;
    if (v > 500'000)  return 500'000;
    return v;
}
void Settings::setViewMaxKbps(int v)          { if (v != viewMaxKbps()) { q_.setValue(K_VIEW_MAX_KBPS, v); emit changed(); } }

// VIV-70 announcement dedup sets (stored as QStringList).
QStringList Settings::seenAnnouncements() const { return q_.value("announce/seen").toStringList(); }
void Settings::addSeenAnnouncement(const QString& id) {
    QStringList s = seenAnnouncements();
    if (!s.contains(id)) { s.append(id); q_.setValue("announce/seen", s); }
}
QStringList Settings::dismissedAnnouncements() const { return q_.value("announce/dismissed").toStringList(); }
void Settings::addDismissedAnnouncement(const QString& id) {
    QStringList d = dismissedAnnouncements();
    if (!d.contains(id)) { d.append(id); q_.setValue("announce/dismissed", d); }
}

// VIV-71 polls.
QString Settings::clientId() {
    QString id = q_.value("poll/clientId").toString();
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        q_.setValue("poll/clientId", id);
    }
    return id;
}
QStringList Settings::answeredPolls() const { return q_.value("poll/answered").toStringList(); }
void Settings::addAnsweredPoll(const QString& id) {
    QStringList a = answeredPolls();
    if (!a.contains(id)) { a.append(id); q_.setValue("poll/answered", a); }
}

} // namespace vivora::gui
