// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/bttv/BttvLiveUpdates.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "liveupdates/BttvLiveUpdateClient.hpp"
#include "providers/bttv/BttvBadges.hpp"
#include "providers/liveupdates/BasicPubSubManager.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/Settings.hpp"

#include <QJsonDocument>

#include <chrono>
#include <unordered_set>
#include <utility>

namespace chatterino {

using namespace Qt::Literals::StringLiterals;

class BttvLiveUpdatesPrivate
    : public BasicPubSubManager<BttvLiveUpdatesPrivate, BttvLiveUpdateClient>
{
public:
    BttvLiveUpdatesPrivate(BttvLiveUpdates &parent, QString host);
    ~BttvLiveUpdatesPrivate() override;
    BttvLiveUpdatesPrivate(const BttvLiveUpdatesPrivate &) = delete;
    BttvLiveUpdatesPrivate(const BttvLiveUpdatesPrivate &&) = delete;
    BttvLiveUpdatesPrivate &operator=(const BttvLiveUpdatesPrivate &) = delete;
    BttvLiveUpdatesPrivate &operator=(const BttvLiveUpdatesPrivate &&) = delete;

    std::shared_ptr<BttvLiveUpdateClient> makeClient();

    std::unordered_set<QString> joinedChannels;
    BttvLiveUpdates &parent;

    friend BasicPubSubManager<BttvLiveUpdates, BttvLiveUpdateClient>;
    friend BttvLiveUpdates;
};

BttvLiveUpdatesPrivate::BttvLiveUpdatesPrivate(BttvLiveUpdates &parent,
                                               QString host)
    : BasicPubSubManager(std::move(host), u"BTTV"_s)
    , parent(parent)
{
}

BttvLiveUpdatesPrivate::~BttvLiveUpdatesPrivate()
{
    this->stop();
}

std::shared_ptr<BttvLiveUpdateClient> BttvLiveUpdatesPrivate::makeClient()
{
    return std::make_shared<BttvLiveUpdateClient>(this->parent);
}

BttvLiveUpdates::BttvLiveUpdates(QString host)
    : private_(std::make_unique<BttvLiveUpdatesPrivate>(*this, std::move(host)))
{
    // BetterTTV only sends a user's name effect and badge when that user is
    // announced, so a change made on the BetterTTV website would otherwise not
    // show up until the next chat message or restart
    this->announceTimer_.setInterval(std::chrono::seconds(5));
    QObject::connect(&this->announceTimer_, &QTimer::timeout, [this] {
        this->announceCurrentUser();
    });
    this->announceTimer_.start();
}

void BttvLiveUpdates::announceCurrentUser()
{
    auto *app = tryGetApp();
    if (!app || !getSettings()->sendBTTVActivity ||
        this->private_->joinedChannels.empty())
    {
        return;
    }

    auto account = app->getAccounts()->twitch.getCurrent();
    if (!account || account->isAnon())
    {
        return;
    }

    // Only BetterTTV Pro users have an effect or badge that can change
    auto *bttvBadges = app->getBttvBadges();
    if (!bttvBadges->hasNameEffect(account->getUserName()) &&
        !bttvBadges->getBadge(UserId{account->getUserId()}))
    {
        return;
    }

    // The effect is stored per user, so one channel is enough. The user's own
    // channel keeps BetterTTV from relaying this to a big chat.
    const auto &channels = this->private_->joinedChannels;
    const auto ownChannel = channels.find(account->getUserId());
    this->broadcastMe(
        ownChannel != channels.end() ? *ownChannel : *channels.begin(),
        account->getUserId());
}

BttvLiveUpdates::~BttvLiveUpdates() = default;

void BttvLiveUpdates::joinChannel(const QString &channelID,
                                  const QString &userID)
{
    if (this->private_->joinedChannels.insert(channelID).second)
    {
        this->private_->subscribe(
            {BttvLiveUpdateSubscriptionChannel{channelID}});
        this->private_->subscribe({BttvLiveUpdateBroadcastMe{
            .twitchID = channelID, .userID = userID}});
    }
}

void BttvLiveUpdates::partChannel(const QString &id)
{
    if (this->private_->joinedChannels.erase(id) > 0)
    {
        this->private_->unsubscribe({BttvLiveUpdateSubscriptionChannel{id}});
    }
}

void BttvLiveUpdates::broadcastMe(const QString &channelID,
                                  const QString &userID)
{
    // BetterTTV ignores an announcement from a connection that hasn't joined
    // the channel, and channels are spread over several connections
    for (const auto &[id, client] : this->private_->clients())
    {
        if (client->isInChannel(channelID))
        {
            client->broadcastMe(channelID, userID);
            return;
        }
    }
}

void BttvLiveUpdates::stop()
{
    this->private_->stop();
}

const liveupdates::Diag &BttvLiveUpdates::diag() const
{
    return this->private_->diag;
}

}
