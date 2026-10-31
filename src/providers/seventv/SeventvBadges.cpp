#include "providers/seventv/SeventvBadges.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "singletons/Settings.hpp"
#include "util/PostToThread.hpp"

#include <QJsonArray>

namespace chatterino {

SeventvBadges::SeventvBadges()
{
    this->fetchTimer_.setSingleShot(true);
    QObject::connect(&this->fetchTimer_, &QTimer::timeout, &this->fetchTimer_,
                     [this] {
                         this->loadPendingBadges();
                     });
}

void SeventvBadges::loadBadgeByID(const QString &id)
{
    if (!SeventvPaints::isValidPaintID(id) || this->getBadgeByID(id))
    {
        return;
    }
    postToThread(
        [this, id] {
            if (this->deferredBadges_.contains(id))
            {
                return;
            }
            this->pendingBadges_.insert(id);
            if (!this->loading_ && !this->fetchTimer_.isActive())
            {
                this->fetchTimer_.start(0);
            }
        },
        &this->fetchTimer_);
}

void SeventvBadges::loadPendingBadges()
{
    this->pendingBadges_.removeIf([this](const auto &id) {
        return this->getBadgeByID(id).has_value() || !this->isBadgeAssigned(id);
    });
    if (this->loading_ || this->pendingBadges_.isEmpty())
    {
        return;
    }
    QJsonArray ids;
    for (const auto &id : this->pendingBadges_)
    {
        ids.append(id);
        if (ids.size() == 50)
        {
            break;
        }
    }
    this->loading_ = true;
    NetworkRequest("https://7tv.io/v3/gql", NetworkRequestType::Post)
        .caller(&this->fetchTimer_)
        .timeout(15000)
        .maximumResponseSize(2 * 1024 * 1024)
        .json(QJsonObject{{"query", QStringLiteral(R"(
query MoltorinoBadgesByID($list: [Id!]!) {
  cosmetics(list: $list) {
    badges {
      id name tooltip
      host { url files { name static_name format width height } }
    }
  }
}
)")},
                          {"variables", QJsonObject{{"list", ids}}}})
        .onSuccess([this, ids](const NetworkResult &result) {
            const auto root = result.parseJson();
            const auto cosmetics = root["data"]["cosmetics"];
            if (!root["errors"].toArray().isEmpty() || !cosmetics.isObject())
            {
                this->fetchTimer_.start(std::chrono::seconds(30));
                return;
            }
            for (const auto &badge : cosmetics["badges"].toArray())
            {
                this->registerBadge(badge.toObject(), false);
            }
            QStringList missing;
            for (const auto &id : ids)
            {
                this->pendingBadges_.remove(id.toString());
                if (!this->getBadgeByID(id.toString()))
                {
                    missing.push_back(id.toString());
                    this->deferredBadges_.insert(id.toString());
                }
            }
            if (!missing.isEmpty())
            {
                QTimer::singleShot(std::chrono::seconds(30), &this->fetchTimer_,
                                   [this, missing] {
                                       for (const auto &id : missing)
                                       {
                                           this->deferredBadges_.remove(id);
                                           if (this->isBadgeAssigned(id))
                                           {
                                               this->loadBadgeByID(id);
                                           }
                                       }
                                   });
            }
        })
        .onError([this](const auto &) {
            this->fetchTimer_.start(std::chrono::seconds(30));
        })
        .finally([this] {
            this->loading_ = false;
            if (!this->pendingBadges_.isEmpty() &&
                !this->fetchTimer_.isActive())
            {
                this->fetchTimer_.start(0);
            }
        })
        .execute();
}

QString SeventvBadges::idForBadge(const QJsonObject &badgeJson) const
{
    return badgeJson["id"].toString();
}

EmotePtr SeventvBadges::createBadge(const QString &id,
                                    const QJsonObject &badgeJson) const
{
    auto emote = Emote{

        .name = EmoteName{u"7tv:" % badgeJson["name"].toString()},
        .images = SeventvEmotes::createImageSet(
            badgeJson, !getSettings()->animateSevenTVBadges),
        .tooltip = Tooltip{badgeJson["tooltip"].toString()},
        .homePage = Url{},
        .id = EmoteId{id},
    };

    if (emote.images.getImage1()->isEmpty())
    {
        return nullptr;
    }

    return std::make_shared<const Emote>(std::move(emote));
}

}
