#include "providers/seventv/SeventvUserCosmetics.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "providers/seventv/eventapi/Dispatch.hpp"
#include "providers/seventv/SeventvBadges.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Settings.hpp"
#include "util/PostToThread.hpp"

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <chrono>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chatterino {

namespace {

const QString GQL_URL = QStringLiteral("https://7tv.io/v4/gql");
constexpr qsizetype BATCH_SIZE = 50;
constexpr auto BATCH_DELAY = std::chrono::milliseconds(500);
constexpr auto ERROR_DELAY = std::chrono::seconds(30);

struct LookupState {
    QSet<QString> lookedUp;
    // Twitch ID -> login, in the order they were seen
    std::vector<std::pair<QString, QString>> queue;
    bool scheduled = false;
    bool inFlight = false;
};

LookupState &state()
{
    static LookupState s;
    return s;
}

bool wanted()
{
    const auto *settings = getSettings();
    return settings->displaySevenTVPaints || settings->showBadgesSevenTV;
}

void sendNextBatch();

void scheduleBatch(std::chrono::milliseconds delay)
{
    auto &s = state();
    if (s.scheduled || s.inFlight || s.queue.empty())
    {
        return;
    }
    s.scheduled = true;
    QTimer::singleShot(delay, [] {
        state().scheduled = false;
        sendNextBatch();
    });
}

// Badges are added when a line is built, so lines that came in before the
// lookup finished get the badge added to a copy that replaces them
void addBadgeToExistingLines(const QString &badgeID,
                             const std::unordered_set<QString> &userIDs)
{
    auto *app = tryGetApp();
    if (!app)
    {
        return;
    }
    const auto badge = app->getSeventvBadges()->getBadgeByID(badgeID);
    if (!badge)
    {
        return;
    }

    app->getTwitch()->forEachChannelAndSpecialChannels(
        [&](const ChannelPtr &channel) {
            const auto snapshot = channel->getMessageSnapshot();
            for (size_t i = 0; i < snapshot.size(); i++)
            {
                const auto &message = snapshot[i];
                if (!userIDs.contains(message->userID))
                {
                    continue;
                }

                // after the last badge before the name, so it lands where
                // the builder would have put it, ahead of Homies badges
                std::optional<size_t> insertAt;
                bool hasSeventvBadge = false;
                for (size_t e = 0; e < message->elements.size(); e++)
                {
                    const auto flags = message->elements[e]->getFlags();
                    if (flags.has(MessageElementFlag::Username))
                    {
                        break;
                    }
                    if (flags.has(MessageElementFlag::BadgeSevenTV))
                    {
                        hasSeventvBadge = true;
                        break;
                    }
                    if (flags.hasAny(MessageElementFlag::BadgeHomies,
                                     MessageElementFlag::BadgeHomiesCustom))
                    {
                        insertAt = e;
                        break;
                    }
                    if (flags.hasAny(MessageElementFlag::Badges))
                    {
                        insertAt = e + 1;
                    }
                }
                if (hasSeventvBadge || !insertAt)
                {
                    continue;
                }

                auto copy = message->clone();
                copy->elements.insert(
                    copy->elements.begin() +
                        static_cast<std::ptrdiff_t>(*insertAt),
                    std::make_unique<BadgeElement>(
                        *badge, MessageElementFlag::BadgeSevenTV));
                copy->externalBadges.emplace_back((*badge)->name.string);
                channel->replaceMessage(i, message, copy);
            }
        });
}

// Waits for the badge image info to load, like the activity feed does
void whenBadgeLoaded(const QString &badgeID,
                     std::unordered_set<QString> userIDs, int attempt = 0)
{
    auto *app = tryGetApp();
    if (!app)
    {
        return;
    }
    if (app->getSeventvBadges()->getBadgeByID(badgeID))
    {
        addBadgeToExistingLines(badgeID, userIDs);
        return;
    }
    if (attempt >= 20)
    {
        return;
    }
    QTimer::singleShot(std::chrono::milliseconds(500),
                       [badgeID, userIDs = std::move(userIDs), attempt] {
                           whenBadgeLoaded(badgeID, userIDs, attempt + 1);
                       });
}

void applyResult(const QString &id, const QString &login,
                 const QJsonObject &style,
                 QHash<QString, std::unordered_set<QString>> &newBadges)
{
    auto *app = tryGetApp();
    if (!app)
    {
        return;
    }

    const auto paintID = style["activePaintId"].toString();
    // The live event connection may have sent something newer meanwhile
    if (!paintID.isEmpty() && !app->getSeventvPaints()->getPaint(login, false))
    {
        const seventv::eventapi::User user =
            seventv::eventapi::TwitchUser(QJsonObject{
                {"id", id},
                {"username", login},
            });
        app->getSeventvPaints()->assignPaintToUsers(paintID,
                                                    std::span(&user, 1));
    }

    const auto badgeID = style["activeBadgeId"].toString();
    if (!badgeID.isEmpty() && !app->getSeventvBadges()->getBadge({id}))
    {
        app->getSeventvBadges()->assignBadgeToUser(badgeID, UserId{id});
        app->getSeventvBadges()->loadBadgeByID(badgeID);
        newBadges[badgeID].insert(id);
    }
}

void sendNextBatch()
{
    auto &s = state();
    if (s.inFlight || s.queue.empty() || !tryGetApp())
    {
        return;
    }

    const auto count =
        std::min<qsizetype>(BATCH_SIZE, static_cast<qsizetype>(s.queue.size()));
    std::vector<std::pair<QString, QString>> batch(s.queue.begin(),
                                                   s.queue.begin() + count);
    s.queue.erase(s.queue.begin(), s.queue.begin() + count);

    QString query = QStringLiteral("{ users {");
    for (qsizetype i = 0; i < count; i++)
    {
        // IDs are checked to be digits before they're queued
        query += QStringLiteral(
                     " u%1: userByConnection(platform: TWITCH, platformId: "
                     "\"%2\") { style { activePaintId activeBadgeId } }")
                     .arg(i)
                     .arg(batch[static_cast<size_t>(i)].first);
    }
    query += QStringLiteral(" } }");

    s.inFlight = true;
    NetworkRequest(GQL_URL, NetworkRequestType::Post)
        .json(QJsonObject{{"query", query}})
        .timeout(15000)
        .maximumResponseSize(1024 * 1024)
        .onSuccess([batch](const NetworkResult &result) {
            const auto users =
                result.parseJson()["data"].toObject()["users"].toObject();
            QHash<QString, std::unordered_set<QString>> newBadges;
            for (size_t i = 0; i < batch.size(); i++)
            {
                const auto user =
                    users[QStringLiteral("u%1").arg(i)].toObject();
                if (!user.isEmpty())
                {
                    applyResult(batch[i].first, batch[i].second,
                                user["style"].toObject(), newBadges);
                }
            }
            for (auto it = newBadges.begin(); it != newBadges.end(); ++it)
            {
                whenBadgeLoaded(it.key(), std::move(it.value()));
            }
            state().inFlight = false;
            scheduleBatch(BATCH_DELAY);
        })
        .onError([batch](const NetworkResult &) {
            // Forget them so their next message tries again
            auto &s = state();
            for (const auto &[id, login] : batch)
            {
                s.lookedUp.remove(id);
            }
            s.inFlight = false;
            scheduleBatch(ERROR_DELAY);
        })
        .execute();
}

}  // namespace

void lookUpSeventvCosmetics(const QString &twitchID, const QString &login)
{
    if (twitchID.isEmpty() || login.isEmpty())
    {
        return;
    }
    for (const auto c : twitchID)
    {
        if (!c.isDigit())
        {
            return;
        }
    }

    postToThread([twitchID, login = login.toLower()] {
        auto *app = tryGetApp();
        if (!app || app->isTest() || !wanted())
        {
            return;
        }
        auto &s = state();
        // Someone can have a paint we know of but no badge yet, like the
        // current user, whose paint is set at sign in
        if (s.lookedUp.contains(twitchID) ||
            (app->getSeventvPaints()->getPaint(login, false) &&
             app->getSeventvBadges()->getBadge({twitchID})))
        {
            return;
        }
        s.lookedUp.insert(twitchID);
        s.queue.emplace_back(twitchID, login);
        scheduleBatch(BATCH_DELAY);
    });
}

}  // namespace chatterino
