#pragma once

#include "common/Aliases.hpp"
#include "util/QStringHash.hpp"  // IWYU pragma: keep

#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <memory>
#include <shared_mutex>
#include <vector>

namespace chatterino {

struct Emote;

class WydiosBadges final : public QObject
{
public:
    void initialize();

    // Returns cached badges and starts a request if the cache is stale.
    std::vector<std::shared_ptr<const Emote>> getBadges(
        const UserId &userID);

    pajlada::Signals::NoArgSignal badgesUpdated;

private:
    bool applyPayload(const QString &userID, const QByteArray &payload);
    void requestUserBadge(const QString &userID);

    mutable std::shared_mutex mutex_;

    // Twitch user ID -> badges belonging to that user.
    QHash<QString, std::vector<std::shared_ptr<const Emote>>> users_;

    // Twitch user ID -> time of the last successful lookup.
    QHash<QString, qint64> fetchedAt_;

    // Prevent duplicate requests for the same user.
    QSet<QString> requestsInFlight_;

    QTimer refreshTimer_;
    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino