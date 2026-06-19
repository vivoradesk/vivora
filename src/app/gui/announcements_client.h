#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QVariantList>

namespace vivora::gui {

// Fetches the in-app announcements feed (VIV-70) and emits the raw list.
// Filtering (seen/dismissed/target/display) is AppController's job.  Silent on
// any network/parse error — an announcement must never block startup.
class AnnouncementsClient : public QObject {
    Q_OBJECT
public:
    explicit AnnouncementsClient(QObject* parent = nullptr);

    // GET <url> (e.g. https://cloud.vivora.dev/announcements).
    void fetch(const QString& url);

signals:
    void fetched(const QVariantList& announcements);

private:
    QNetworkAccessManager nam_;
};

} // namespace vivora::gui
