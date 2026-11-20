// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/bttv/BttvBadges.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "providers/bttv/BttvNameEffect.hpp"

#include <mutex>

using namespace Qt::Literals::StringLiterals;

namespace {

using namespace chatterino;

// BetterTTV tags these for everyone, alongside the bots a channel lists
const QSet<QString> GLOBAL_BOTS{u"nightbot"_s, u"moobot"_s};

EmotePtr makeBadge(const QString &name, const QString &url,
                   const QString &tooltip)
{
    return std::make_shared<const Emote>(Emote{
        .name = EmoteName{u"betterttv:"_s + name},
        .images = ImageSet(Image::fromUrl(Url{url})),
        .tooltip = Tooltip{tooltip},
        .homePage = Url{},
        .id = EmoteId{url},
    });
}

}  // namespace

namespace chatterino {

QString BttvBadges::idForBadge(const QJsonObject &badgeJson) const
{
    return badgeJson["url"].toString();
}

EmotePtr BttvBadges::createBadge(const QString &id,
                                 const QJsonObject & ) const
{
    auto emote = Emote{
        .name = EmoteName{u"betterttv:pro"_s},
        .images = ImageSet(Image::fromUrl(Url{id}, 18.0 / 72.0)),
        .tooltip = Tooltip{"BTTV Pro"},
        .homePage = Url{},
        .id = EmoteId{id},
    };

    return std::make_shared<const Emote>(std::move(emote));
}

bool BttvBadges::setNameEffect(const QString &login, const QString &effect)
{
    const auto key = login.toLower();
    std::unique_lock lock(this->nameEffectsMutex_);
    const auto it = this->nameEffects_.find(key);
    if (effect.isEmpty())
    {
        if (it == this->nameEffects_.end())
        {
            return false;
        }
        this->nameEffects_.erase(it);
        return true;
    }
    if (it != this->nameEffects_.end() && it->second == effect)
    {
        return false;
    }
    this->nameEffects_[key] = effect;
    return true;
}

void BttvBadges::loadStaffBadges()
{
    NetworkRequest(QUrl(u"https://api.betterttv.net/3/cached/badges/twitch"_s))
        .timeout(20000)
        .onSuccess([this](const auto &result) {
            this->setStaffBadges(result.parseJsonArray());
        })
        .onError([](const auto &result) {
            qCWarning(chatterinoBttv)
                << "Failed to load BTTV staff badges:" << result.formatError();
        })
        .execute();
}

void BttvBadges::setStaffBadges(const QJsonArray &badges)
{
    // users share one Emote per badge image
    std::unordered_map<QString, EmotePtr> emotes;
    std::unordered_map<QString, EmotePtr> staffBadges;
    for (const auto &value : badges)
    {
        const auto entry = value.toObject();
        const auto userID = entry["providerId"_L1].toString();
        const auto badge = entry["badge"_L1].toObject();
        const auto url = badge["svg"_L1].toString();
        if (userID.isEmpty() || url.isEmpty())
        {
            continue;
        }

        auto &emote = emotes[url];
        if (!emote)
        {
            const auto name =
                url.section(u'/', -1).section(u'.', 0, 0).toLower();
            emote = makeBadge(name, url, badge["description"_L1].toString());
        }
        staffBadges[userID] = emote;
    }

    std::unique_lock lock(this->staffMutex_);
    this->staffBadges_ = std::move(staffBadges);
}

std::optional<EmotePtr> BttvBadges::getStaffBadge(const UserId &id) const
{
    std::shared_lock lock(this->staffMutex_);
    const auto it = this->staffBadges_.find(id.string);
    if (it == this->staffBadges_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

void BttvBadges::setChannelBots(const QString &channelID,
                                const QJsonArray &bots)
{
    QSet<QString> logins;
    for (const auto &bot : bots)
    {
        const auto login = bot.toString().toLower();
        if (!login.isEmpty())
        {
            logins.insert(login);
        }
    }

    std::unique_lock lock(this->staffMutex_);
    this->channelBots_[channelID] = std::move(logins);
}

EmotePtr BttvBadges::getBotBadge(const QString &channelID,
                                 const QString &login) const
{
    static const auto badge = makeBadge(
        u"bot"_s, u"https://cdn.betterttv.net/tags/bot.png"_s, u"Bot"_s);

    const auto key = login.toLower();
    if (GLOBAL_BOTS.contains(key))
    {
        return badge;
    }

    std::shared_lock lock(this->staffMutex_);
    const auto it = this->channelBots_.find(channelID);
    if (it != this->channelBots_.end() && it->second.contains(key))
    {
        return badge;
    }
    return nullptr;
}

bool BttvBadges::hasNameEffect(const QString &login) const
{
    std::shared_lock lock(this->nameEffectsMutex_);
    return this->nameEffects_.contains(login.toLower());
}

std::shared_ptr<Paint> BttvBadges::getNameEffect(const QString &login) const
{
    QString effect;
    {
        std::shared_lock lock(this->nameEffectsMutex_);
        const auto it = this->nameEffects_.find(login.toLower());
        if (it == this->nameEffects_.end())
        {
            return nullptr;
        }
        effect = it->second;
    }
    return BttvNameEffect::fromName(effect);
}

}
