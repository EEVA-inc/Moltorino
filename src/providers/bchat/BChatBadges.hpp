
#pragma once

#include "common/Aliases.hpp"
#include "util/QStringHash.hpp"

#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <shared_mutex>
#include <vector>

namespace chatterino {

struct Emote;

class BChatBadges final : public QObject
{
public:
    void initialize();

    // Returns all cached badges belonging to a Twitch user.
    std::vector<std::shared_ptr<const Emote>> getBadges(
        const UserId &userID);

    pajlada::Signals::NoArgSignal badgesUpdated;

private:
    bool applyPayload(const QByteArray &payload);
    void requestBadges();

    mutable std::shared_mutex mutex_;

    // Twitch user ID -> badges.
    QHash<QString, std::vector<std::shared_ptr<const Emote>>> users_;

    bool requestInFlight_ = false;

    QTimer refreshTimer_;
    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino