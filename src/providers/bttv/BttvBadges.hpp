// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/BadgeRegistry.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;
class Paint;

class BttvBadges : public BadgeRegistry
{
public:
    BttvBadges() = default;

    /// Sets the BetterTTV Pro username effect of a Twitch login. An empty
    /// effect clears it. Returns whether anything changed.
    bool setNameEffect(const QString &login, const QString &effect);

    /// The username effect of a Twitch login, or nullptr. Only call this from
    /// the GUI thread.
    std::shared_ptr<Paint> getNameEffect(const QString &login) const;

    bool hasNameEffect(const QString &login) const;

    /// Loads BetterTTV's list of staff badges (developers, translators and
    /// others) from its API
    void loadStaffBadges();

    /// Reads the response of BetterTTV's cached badges endpoint
    void setStaffBadges(const QJsonArray &badges);

    std::optional<EmotePtr> getStaffBadge(const UserId &id) const;

    /// Sets the bots a channel listed on BetterTTV
    void setChannelBots(const QString &channelID, const QJsonArray &bots);

    /// The badge BetterTTV shows in place of the moderator badge for a known
    /// bot, or nullptr if the login isn't one
    EmotePtr getBotBadge(const QString &channelID, const QString &login) const;

protected:
    QString idForBadge(const QJsonObject &badgeJson) const override;
    EmotePtr createBadge(const QString &id,
                         const QJsonObject &badgeJson) const override;

private:
    mutable std::shared_mutex nameEffectsMutex_;
    // login -> effect name
    std::unordered_map<QString, QString> nameEffects_;

    mutable std::shared_mutex staffMutex_;
    // user ID -> badge
    std::unordered_map<QString, EmotePtr> staffBadges_;
    // channel ID -> bot logins
    std::unordered_map<QString, QSet<QString>> channelBots_;
};

}
