#include "providers/twitch/ActivityFeed.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/seventv/eventapi/Dispatch.hpp"
#include "providers/seventv/SeventvAPI.hpp"
#include "providers/seventv/SeventvBadges.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/ChannelPointReward.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/UserColor.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Helpers.hpp"
#include "util/IrcHelpers.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/Window.hpp"

#include <IrcMessage>
#include <QHash>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace {

using namespace chatterino;

// Roughly once a minute, not on an exact beat
constexpr int FOLLOWER_POLL_MIN_MS = 50 * 1000;
constexpr int FOLLOWER_POLL_MAX_MS = 80 * 1000;

// Anonymous gifts are sent as this user ("AnAnonymousGifter")
constexpr auto ANONYMOUS_GIFTER_ID = "274598607";

struct ActivityUser {
    QString id;
    QString login;
    QString displayName;
    QColor color;
};

QString subPlanName(const QString &plan)
{
    if (plan.compare("Prime", Qt::CaseInsensitive) == 0)
    {
        return "Prime";
    }
    // msg-param-sub-plan is 1000, 2000 or 3000
    return "Tier " + (plan.isEmpty() ? QString("1") : plan.left(1));
}

// Sub upgrades and pay-it-forward gifts, shown with Twitch's own wording
const QSet<QString> OTHER_SUB_NOTICES{
    "giftpaidupgrade",     "anongiftpaidupgrade", "primepaidupgrade",
    "communitypayforward", "standardpayforward",
};

// The line already starts with the user's name, so drop it from system-msg
QString systemMessageAction(Communi::TagsRef tags, const ActivityUser &user)
{
    const auto text = parseTagString(tags.getOrEmpty("system-msg")).trimmed();
    for (const auto &name : {user.displayName, user.login})
    {
        if (!name.isEmpty() && text.startsWith(name + ' ', Qt::CaseInsensitive))
        {
            return text.mid(name.size() + 1);
        }
    }
    return text;
}

// Shared chat mirrors other channels' subs and cheers into this one
bool isFromSharedChat(Communi::TagsRef tags)
{
    if (tags.getOrEmpty("msg-id") == "sharedchatnotice")
    {
        return true;
    }
    auto roomId = tags.get("room-id");
    auto sourceRoomId = tags.get("source-room-id");
    return roomId.has_value() && sourceRoomId.has_value() &&
           *roomId != *sourceRoomId;
}

BoolSetting &typeSetting(activityfeed::EventType type)
{
    using activityfeed::EventType;
    auto *s = getSettings();
    switch (type)
    {
        case EventType::Follow:
            return s->activityFeedFollows;
        case EventType::Share:
            return s->activityFeedShares;
        case EventType::Sub:
            return s->activityFeedSubscriptions;
        case EventType::GiftSub:
            return s->activityFeedGiftSubs;
        case EventType::Paid:
            return s->activityFeedBits;
        case EventType::Raid:
            return s->activityFeedRaids;
        case EventType::Redemption:
            return s->activityFeedRedemptions;
        case EventType::Milestone:
            return s->activityFeedWatchStreaks;
    }
    return s->activityFeedSubscriptions;
}

BoolSetting &platformSetting(MessagePlatform platform)
{
    auto *s = getSettings();
    switch (platform)
    {
        case MessagePlatform::Kick:
            return s->activityFeedKick;
        case MessagePlatform::YouTube:
            return s->activityFeedYouTube;
        case MessagePlatform::TikTok:
            return s->activityFeedTikTok;
        case MessagePlatform::AnyOrTwitch:
            break;
    }
    return s->activityFeedTwitch;
}

int minimumAmount(MessagePlatform platform)
{
    auto *s = getSettings();
    switch (platform)
    {
        case MessagePlatform::Kick:
            return s->activityFeedMinKicks;
        case MessagePlatform::YouTube:
            return s->activityFeedMinSuperChat;
        case MessagePlatform::TikTok:
            return s->activityFeedMinTikTokGifts;
        case MessagePlatform::AnyOrTwitch:
            break;
    }
    return s->activityFeedMinBits;
}

QStringList nameList(const QStringSetting &setting)
{
    static const QRegularExpression separators(R"([\s,#]+)");
    return setting.getValue().split(separators, Qt::SkipEmptyParts);
}

bool isShown(MessagePlatform platform, activityfeed::EventType type,
             const QString &channelName, const QString &login, double amount)
{
    if (!platformSetting(platform) || !typeSetting(type))
    {
        return false;
    }
    // An empty list means every channel you mod or own
    const auto channels = nameList(getSettings()->activityFeedChannels);
    if (!channels.isEmpty() &&
        !channels.contains(channelName, Qt::CaseInsensitive))
    {
        return false;
    }
    // amount is 0 when the event doesn't say how much it was
    if (type == activityfeed::EventType::Paid && amount > 0 &&
        amount < minimumAmount(platform))
    {
        return false;
    }
    return login.isEmpty() || !nameList(getSettings()->activityFeedHiddenUsers)
                                   .contains(login, Qt::CaseInsensitive);
}

// Lines in the feed and their type, so the right-click menu can offer to hide
// that kind of alert
struct ShownLine {
    std::weak_ptr<const Message> message;
    activityfeed::EventType type;
};

std::vector<ShownLine> &shownLines()
{
    static std::vector<ShownLine> lines;
    return lines;
}

void publish(const MessagePtr &message, activityfeed::EventType type)
{
    auto &lines = shownLines();
    std::erase_if(lines, [](const auto &line) {
        return line.message.expired();
    });
    lines.push_back({message, type});
    getApp()->getTwitch()->getActivityChannel()->addMessage(
        message, MessageContext::Original);
}

// Takes lines that were already shown back out after something got hidden
void removeLines(const std::function<bool(const Message &,
                                          activityfeed::EventType)> &matches)
{
    auto channel = getApp()->getTwitch()->getActivityChannel();
    const auto snapshot = channel->getMessageSnapshot();
    channel->clearMessages();
    for (const auto &message : snapshot)
    {
        const auto type = activityfeed::eventTypeOf(message);
        if (!type || !matches(*message, *type))
        {
            channel->addMessage(message, MessageContext::Repost);
        }
    }
}

struct Line {
    ActivityUser user;
    QString action;
    activityfeed::EventType type{};
    double amount = 0;
    // the flag the same event gets in chat, so it's colored the same
    MessageFlag highlight = MessageFlag::None;
    // Follows and redemptions don't come with IRC tags, so they get no Twitch
    // badges, only the ones looked up by user id
    QVariantMap tags;
    QString userText;
    QTime time = QTime::currentTime();
};

MessagePtr build(TwitchChannel &channel, const Line &line)
{
    const auto &user = line.user;
    const auto &action = line.action;
    const auto &userText = line.userText;
    const auto type = line.type;
    const auto amount = line.amount;
    const auto highlight = line.highlight;
    const auto time = line.time;
    const auto tags = Communi::TagsRef(line.tags);

    const auto name =
        user.displayName.isEmpty() ? user.login : user.displayName;
    const auto userColor =
        twitch::getUserColor({
                                 .userLogin = user.login,
                                 .userID = user.id,
                                 .userDataController = getApp()->getUserData(),
                                 .channelChatters = &channel,
                                 .color = user.color,
                             })
            .value_or(MessageColor::Text);
    const auto isCurrentUser =
        user.id == getApp()->getAccounts()->twitch.getCurrent()->getUserId();

    MessageBuilder builder;
    builder
        .emplace<ChannelNameElement>(u'#' + channel.getName(),
                                     channel.channelAvatar())
        ->setLink({Link::JumpToChannel, channel.getName()});
    builder.emplace<TimestampElement>(time);
    // Same badges and name paint as in chat, so the /vanity setup carries over
    builder.appendUserBadges(tags, &channel, user.id, isCurrentUser);
    builder.appendUsernameElement(user.login, user.displayName, userColor);
    // Twitch's own sub texts already end in a period or exclamation mark
    const bool needsColon =
        !userText.isEmpty() && !action.endsWith('.') && !action.endsWith('!');
    builder.emplace<TextElement>(needsColon ? action + ':' : action,
                                 MessageElementFlag::Text,
                                 MessageColor::System);
    builder.appendUserText(
        tags, &channel, user.id, userText,
        type == activityfeed::EventType::Paid ? static_cast<int>(amount) : 0);

    builder->flags.set(MessageFlag::System);
    builder->flags.set(MessageFlag::DoNotTriggerNotification);
    if (highlight != MessageFlag::None)
    {
        builder->flags.set(highlight);
    }
    builder->channelName = channel.getName();
    builder->messageText = name + ' ' + action;
    if (!userText.isEmpty())
    {
        builder->messageText += (needsColon ? ": " : " ") + userText;
    }
    builder->searchText = builder->messageText;
    return builder.release();
}

void replaceLine(const MessagePtr &old, const MessagePtr &replacement)
{
    for (auto &line : shownLines())
    {
        if (line.message.lock() == old)
        {
            line.message = replacement;
        }
    }
    getApp()->getTwitch()->getActivityChannel()->replaceMessage(old,
                                                                replacement);
}

// Calls done once the paint and badge are loaded, or after about 5 seconds
void whenCosmeticsLoaded(const QString &paintID, const QString &badgeID,
                         std::function<void()> done, int attempt = 0)
{
    auto *app = getApp();
    const bool ready =
        (paintID.isEmpty() || app->getSeventvPaints()->getPaintByID(paintID)) &&
        (badgeID.isEmpty() ||
         app->getSeventvBadges()->getBadgeByID(badgeID).has_value());
    if (ready || attempt >= 10)
    {
        done();
        return;
    }
    QTimer::singleShot(
        500, [paintID, badgeID, done = std::move(done), attempt]() mutable {
            whenCosmeticsLoaded(paintID, badgeID, std::move(done), attempt + 1);
        });
}

// 7TV only tells clients about someone's paint and badge after it sees them
// active, so the first alert from a user could show without them. Users we
// know nothing about are looked up once per session and their lines are drawn
// again with the result.
void loadSeventvCosmetics(const ActivityUser &user, std::function<void()> done)
{
    static QSet<QString> lookedUp;
    // lines waiting for a lookup that's still running
    static QHash<QString, std::vector<std::function<void()>>> waiting;

    if (auto it = waiting.find(user.id); it != waiting.end())
    {
        it->push_back(std::move(done));
        return;
    }

    auto *app = getApp();
    const auto *settings = getSettings();
    if (user.id.isEmpty() || user.login.isEmpty() ||
        user.id == ANONYMOUS_GIFTER_ID || lookedUp.contains(user.id) ||
        (!settings->displaySevenTVPaints && !settings->showBadgesSevenTV) ||
        app->getSeventvPaints()->getPaint(user.login, false) ||
        app->getSeventvBadges()->getBadge({user.id}))
    {
        return;
    }
    lookedUp.insert(user.id);
    waiting[user.id].push_back(std::move(done));

    app->getSeventvAPI()->getUserByTwitchID(
        user.id,
        [user](const QJsonObject &json, const auto &) {
            auto callbacks = waiting.take(user.id);
            auto doneAll = [callbacks = std::move(callbacks)] {
                for (const auto &callback : callbacks)
                {
                    callback();
                }
            };

            const auto style = json["user"].toObject()["style"].toObject();
            const auto paintID = style["paint_id"].toString();
            const auto badgeID = style["badge_id"].toString();
            if (paintID.isEmpty() && badgeID.isEmpty())
            {
                return;
            }

            auto *app = getApp();
            if (!paintID.isEmpty())
            {
                const seventv::eventapi::User seventvUser =
                    seventv::eventapi::TwitchUser(QJsonObject{
                        {"id", user.id},
                        {"username", user.login},
                    });
                app->getSeventvPaints()->assignPaintToUsers(
                    paintID, std::span(&seventvUser, 1));
            }
            if (!badgeID.isEmpty())
            {
                app->getSeventvBadges()->assignBadgeToUser(badgeID,
                                                           UserId{user.id});
                app->getSeventvBadges()->loadBadgeByID(badgeID);
            }
            whenCosmeticsLoaded(paintID, badgeID, std::move(doneAll));
        },
        [id = user.id](const auto &) {
            // not on 7TV, the lines stay as they are
            waiting.remove(id);
        });
}

void post(TwitchChannel &channel, const Line &line)
{
    if (!channel.hasModRights() ||
        !isShown(MessagePlatform::AnyOrTwitch, line.type, channel.getName(),
                 line.user.login, line.amount))
    {
        return;
    }

    auto message = build(channel, line);
    publish(message, line.type);

    loadSeventvCosmetics(
        line.user, [weak = channel.weak_from_this(), line, message] {
            auto shared = std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (shared)
            {
                replaceLine(message, build(*shared, line));
            }
        });
}

bool activitySplitIsOpen()
{
    const auto activity = getApp()->getTwitch()->getActivityChannel();
    bool open = false;
    for (auto *window : getApp()->getWindows()->windows())
    {
        window->getNotebook().forEachSplit([&](Split *split) {
            open = open || split->getChannel() == activity;
        });
    }
    return open;
}

}  // namespace

namespace chatterino::activityfeed {

void addUserNotice(const Communi::IrcMessage &message, TwitchChannel &channel)
{
    const auto tags = message.tags();
    if (isFromSharedChat(tags))
    {
        return;
    }

    const auto kind = tags.getOrEmpty("msg-id");
    ActivityUser user{
        .id = tags.getOrEmpty("user-id"),
        .login = tags.getOrEmpty("login"),
        .displayName = tags.getOrEmpty("display-name"),
        .color = QColor::fromString(tags.getOrEmpty("color")),
    };
    if (user.id == ANONYMOUS_GIFTER_ID)
    {
        user.displayName = "An anonymous user";
    }
    const auto plan = subPlanName(tags.getOrEmpty("msg-param-sub-plan"));
    QString action;
    EventType type{};

    // For subs and gifts Twitch's own text is used where it exists, since it
    // has the total months, the streak when the user shares it and gift
    // totals, the same as on Twitch
    if (kind == "sub" || kind == "resub")
    {
        type = EventType::Sub;
        action = systemMessageAction(tags, user);
        if (action.isEmpty())
        {
            action = kind == "resub" ? "resubscribed" : "subscribed";
            action += plan == "Prime" ? " with Prime" : " at " + plan;
            const auto months =
                tags.getOrEmpty("msg-param-cumulative-months").toInt();
            if (months > 1)
            {
                action += QString(" (%1 months)").arg(months);
            }
        }
    }
    else if (kind == "subgift")
    {
        // Gift bombs send one submysterygift and then a subgift per
        // recipient, which would flood the feed
        if (tags.get("msg-param-community-gift-id").has_value())
        {
            return;
        }
        type = EventType::GiftSub;
        action = systemMessageAction(tags, user);
        if (action.isEmpty())
        {
            const auto recipient =
                tags.getOrEmpty("msg-param-recipient-display-name");
            const auto months =
                tags.getOrEmpty("msg-param-gift-months").toInt();
            action =
                months > 1
                    ? QString("gifted %1 months of %2 to %3")
                          .arg(QString::number(months), plan, recipient)
                    : QString("gifted a %1 sub to %2").arg(plan, recipient);
        }
    }
    else if (kind == "submysterygift")
    {
        type = EventType::GiftSub;
        action = systemMessageAction(tags, user);
        if (action.isEmpty())
        {
            const auto count =
                tags.getOrEmpty("msg-param-mass-gift-count").toInt();
            action = count > 1 ? QString("gifted %1 %2 subs")
                                     .arg(localizeNumbers(count), plan)
                               : QString("gifted a %1 sub").arg(plan);
        }
    }
    else if (kind == "raid")
    {
        type = EventType::Raid;
        user.login = tags.getOrEmpty("msg-param-login");
        user.displayName = tags.getOrEmpty("msg-param-displayName");
        const auto viewers = tags.getOrEmpty("msg-param-viewerCount").toInt();
        action = QString("raided with %1 %2")
                     .arg(localizeNumbers(viewers),
                          viewers == 1 ? "viewer" : "viewers");
    }
    else if (kind == "bitsbadgetier")
    {
        type = EventType::Milestone;
        action = QString("earned the %1 bits badge")
                     .arg(localizeNumbers(
                         tags.getOrEmpty("msg-param-threshold").toInt()));
    }
    else if (kind == "viewermilestone" || kind == "charitydonation" ||
             OTHER_SUB_NOTICES.contains(kind))
    {
        type = kind == "viewermilestone"     ? EventType::Milestone
               : kind == "charitydonation"   ? EventType::Paid
               : kind.endsWith("payforward") ? EventType::GiftSub
                                             : EventType::Sub;
        action = systemMessageAction(tags, user);
        if (action.isEmpty())
        {
            return;
        }
    }
    else
    {
        return;
    }

    post(channel,
         {
             .user = user,
             .action = action,
             .type = type,
             .highlight = kind == "viewermilestone" ? MessageFlag::WatchStreak
                                                    : MessageFlag::Subscription,
             .tags = tags.raw(),
             .userText = message.parameters().value(1),
         });
}

void addCheer(const Communi::IrcPrivateMessage &message, TwitchChannel &channel)
{
    const auto tags = message.tags();
    const auto bits = tags.getOrEmpty("bits").toInt();
    if (bits <= 0 || isFromSharedChat(tags))
    {
        return;
    }

    post(channel,
         {
             .user =
                 {
                     .id = tags.getOrEmpty("user-id"),
                     .login = message.nick(),
                     .displayName = tags.getOrEmpty("display-name"),
                     .color = QColor::fromString(tags.getOrEmpty("color")),
                 },
             .action =
                 bits == 1
                     ? QString("cheered 1 bit")
                     : QString("cheered %1 bits").arg(localizeNumbers(bits)),
             .type = EventType::Paid,
             .amount = static_cast<double>(bits),
             .tags = tags.raw(),
             .userText = message.content(),
         });
}

void addRedemption(const ChannelPointReward &reward, TwitchChannel &channel)
{
    QString action = "redeemed " + reward.title;
    if (reward.cost > 0)
    {
        action += QString(" for %1 %2")
                      .arg(localizeNumbers(reward.cost),
                           reward.isBits ? "bits" : "points");
    }

    post(channel, {
                      .user =
                          {
                              .id = reward.redeemedBy.id,
                              .login = reward.redeemedBy.login,
                              .displayName = reward.redeemedBy.displayName,
                          },
                      .action = action,
                      .type = EventType::Redemption,
                      .highlight = MessageFlag::RedeemedChannelPointReward,
                      .userText = reward.userInput,
                  });
}

void addPlatformEvent(const MessagePtr &message, EventType type, double amount)
{
    if (message && isShown(message->platform, type, message->channelName,
                           message->loginName, amount))
    {
        publish(message, type);
    }
}

double amountFromDisplayString(const QString &text)
{
    // Keep the digits of "$5.00", "1.234,56 EUR" or "JPY 500", then treat the
    // last separator as the decimal point when two digits follow it
    QString digits;
    for (const auto c : text)
    {
        if (c.isDigit() || c == u'.' || c == u',')
        {
            digits += c;
        }
    }
    const auto lastSeparator =
        std::max(digits.lastIndexOf(u'.'), digits.lastIndexOf(u','));
    QString whole = digits;
    QString fraction;
    if (lastSeparator >= 0 && digits.size() - lastSeparator - 1 == 2)
    {
        whole = digits.left(lastSeparator);
        fraction = digits.mid(lastSeparator + 1);
    }
    whole.remove(u'.');
    whole.remove(u',');
    return whole.toDouble() + fraction.toDouble() / 100;
}

std::optional<EventType> eventTypeOf(const MessagePtr &message)
{
    for (const auto &line : shownLines())
    {
        if (line.message.lock() == message)
        {
            return line.type;
        }
    }
    return std::nullopt;
}

QString eventTypeName(EventType type)
{
    switch (type)
    {
        case EventType::Follow:
            return "follows";
        case EventType::Share:
            return "shares";
        case EventType::Sub:
            return "subs";
        case EventType::GiftSub:
            return "gifted subs";
        case EventType::Paid:
            return "cheers and donations";
        case EventType::Raid:
            return "raids and hosts";
        case EventType::Redemption:
            return "redemptions";
        case EventType::Milestone:
            return "watch streaks and badges";
    }
    return {};
}

void hideUser(const QString &login)
{
    if (login.isEmpty())
    {
        return;
    }
    auto users = nameList(getSettings()->activityFeedHiddenUsers);
    if (!users.contains(login, Qt::CaseInsensitive))
    {
        users.append(login.toLower());
        getSettings()->activityFeedHiddenUsers = users.join(' ');
    }
    removeLines([&](const Message &message, EventType) {
        return message.loginName.compare(login, Qt::CaseInsensitive) == 0;
    });
}

void hideType(EventType type)
{
    typeSetting(type) = false;
    removeLines([type](const Message &, EventType lineType) {
        return lineType == type;
    });
}

void hidePlatform(MessagePlatform platform)
{
    platformSetting(platform) = false;
    removeLines([platform](const Message &message, EventType) {
        return message.platform == platform;
    });
}

FollowerPoller::FollowerPoller()
    : newestFollow_(std::make_shared<QHash<QString, QDateTime>>())
{
    this->timer_.setSingleShot(true);
    QObject::connect(&this->timer_, &QTimer::timeout, [this] {
        this->poll();
        this->timer_.start(QRandomGenerator::global()->bounded(
            FOLLOWER_POLL_MIN_MS, FOLLOWER_POLL_MAX_MS));
    });
    this->timer_.start(FOLLOWER_POLL_MIN_MS);
}

void FollowerPoller::poll()
{
    if (!getSettings()->activityFeedFollows || !activitySplitIsOpen())
    {
        // Start over when the split is opened again instead of posting
        // everyone who followed while it was closed
        this->newestFollow_->clear();
        return;
    }

    // Keep what was already shown while the split's tab is in the
    // background, then catch up once it's visible again
    const auto visible = getApp()->getWindows()->getVisibleChannelNames();
    if (!visible.contains(
            getApp()->getTwitch()->getActivityChannel()->getName()))
    {
        return;
    }

    // Only the user's own channel and moderated channels they're looking at,
    // so having a lot of tabs open doesn't turn into a lot of requests
    std::vector<std::shared_ptr<TwitchChannel>> channels;
    getApp()->getTwitch()->forEachChannel([&](const ChannelPtr &chan) {
        auto channel = std::dynamic_pointer_cast<TwitchChannel>(chan);
        if (channel && !channel->roomId().isEmpty() &&
            (channel->isBroadcaster() ||
             (channel->isMod() && visible.contains(channel->getName()))))
        {
            channels.push_back(std::move(channel));
        }
    });

    for (auto it = this->newestFollow_->begin();
         it != this->newestFollow_->end();)
    {
        const auto polled = std::ranges::any_of(channels, [&](const auto &c) {
            return c->roomId() == it.key();
        });
        it = polled ? std::next(it) : this->newestFollow_->erase(it);
    }

    std::weak_ptr<QHash<QString, QDateTime>> weakNewest = this->newestFollow_;
    for (const auto &channel : channels)
    {
        const auto channelId = channel->roomId();
        const auto auth = MoltorinoAuth::resolveModerationToken(
            channelId, channel->getName());
        if (!auth.hasToken() || auth.legacy)
        {
            continue;
        }

        std::weak_ptr<TwitchChannel> weakChannel = channel;
        TwitchGql::getRecentFollowers(
            channelId,
            TwitchGqlAuth{
                .oauthToken = auth.token,
                .clientId = auth.clientId,
            },
            [weakNewest, weakChannel,
             channelId](QVector<GqlFollower> followers) {
                auto newest = weakNewest.lock();
                auto channel = weakChannel.lock();
                if (!newest || !channel)
                {
                    return;
                }

                std::ranges::sort(followers, {}, &GqlFollower::followedAt);
                if (!newest->contains(channelId))
                {
                    newest->insert(channelId,
                                   followers.isEmpty()
                                       ? QDateTime()
                                       : followers.last().followedAt);
                    return;
                }

                auto &shownUntil = (*newest)[channelId];
                for (const auto &follower : followers)
                {
                    if (shownUntil.isValid() &&
                        follower.followedAt <= shownUntil)
                    {
                        continue;
                    }
                    shownUntil = follower.followedAt;
                    post(*channel,
                         {
                             .user =
                                 {
                                     .id = follower.user.id,
                                     .login = follower.user.login,
                                     .displayName = follower.user.displayName,
                                 },
                             .action = "followed",
                             .type = EventType::Follow,
                             .time = follower.followedAt.toLocalTime().time(),
                         });
                }
            },
            [channelId](const QString &error) {
                qCDebug(chatterinoTwitch)
                    << "Couldn't fetch followers for" << channelId << error;
            });
    }
}

}  // namespace chatterino::activityfeed
