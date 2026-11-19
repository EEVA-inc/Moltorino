// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/IrcMessageHandler.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/Common.hpp"
#include "common/Literals.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/ignores/IgnoreController.hpp"
#include "controllers/recording/ChatRecordingMessage.hpp"
#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageColor.hpp"
#include "messages/MessageElement.hpp"
#include "messages/MessageSink.hpp"
#include "messages/MessageThread.hpp"
#include "providers/twitch/ActivityFeed.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchAccountManager.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchHelpers.hpp"
#include "providers/twitch/TwitchIrc.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/UserColor.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/WindowManager.hpp"
#include "util/FormatTime.hpp"
#include "util/Helpers.hpp"
#include "util/IrcHelpers.hpp"

#include <IrcMessage>
#include <QCache>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QStringBuilder>

#include <algorithm>
#include <memory>

using namespace chatterino::literals;

namespace {

using namespace chatterino;

void updateSharedFirstMessage(TwitchChannel *channel, const MessagePtr &message,
                              Communi::TagsRef tags)
{
    struct Copy {
        std::weak_ptr<TwitchChannel> channel;
        QString messageId;
    };

    static QMutex mutex;
    static QCache<QString, std::vector<Copy>> pending(256);
    auto sourceId = tags.getOrEmpty("source-id");
    if (sourceId.isEmpty())
    {
        sourceId = message->id;
    }
    if (sourceId.isEmpty())
    {
        return;
    }
    std::vector<Copy> copies;
    {
        const QMutexLocker lock(&mutex);
        if (message->flags.has(MessageFlag::FirstMessage))
        {
            if (auto *found = pending.object(sourceId))
            {
                copies = std::move(*found);
                pending.remove(sourceId);
            }
        }
        else if (message->flags.has(MessageFlag::SharedMessage))
        {
            if (!pending.contains(sourceId))
            {
                pending.insert(sourceId, new std::vector<Copy>);
            }
            auto *found = pending.object(sourceId);
            if (found->size() < 100)
            {
                found->push_back({channel->weakFromThis(), message->id});
            }
        }
    }
    for (const auto &copy : copies)
    {
        if (auto destination = copy.channel.lock())
        {
            if (auto original = destination->findMessageByID(copy.messageId);
                original && !original->flags.has(MessageFlag::FirstMessage))
            {
                auto replacement = original->clone();
                replacement->flags.set(MessageFlag::FirstMessage);
                destination->replaceMessage(original, replacement);
            }
        }
    }
}

// Message types below are the ones that might contain special user's message on USERNOTICE
const QSet<QString> SPECIAL_MESSAGE_TYPES{
    "sub",              //
    "resub",            // resub messages
    "bitsbadgetier",    // bits badge upgrade
    "ritual",           // new viewer ritual
    "announcement",     // new mod announcement thing
    "viewermilestone",  // watch streak, but other categories possible in future
    "modiversary",      // Mod anniversary.
    "socialsharingbadge",
};

const QSet<QString> SUB_MESSAGE_TYPES{
    "sub",              //
    "subgift",
    "resub",
};

void addRecordedUserNotice(MessageSink &sink, TwitchChannel *channel,
                           const MessagePtr &message, Communi::TagsRef tags)
{
    recording::deliverLive(
        message,
        [&] {
            auto noticeTags = tags.raw();
            noticeTags.remove("emotes");
            noticeTags.remove("gifs");
            return channel ? recording::normalizeTwitch(*channel, *message,
                                                        message->messageText,
                                                        noticeTags)
                           : QJsonObject{};
        },
        [&] {
            sink.addMessage(message, MessageContext::Original);
        });
}

bool deleteActionTargetsMessage(const MessagePtr &message,
                                const QString &messageID)
{
    if (!message || messageID.isEmpty() ||
        !message->flags.has(MessageFlag::ModerationAction))
    {
        return false;
    }

    return std::any_of(message->elements.cbegin(), message->elements.cend(),
                       [&](const auto &element) {
                           const auto link = element->getLink();
                           return link.type == Link::JumpToMessage &&
                                  link.value == messageID;
                       });
}

bool hasDeleteActionForMessage(Channel *channel, const QString &messageID)
{
    if (messageID.isEmpty())
    {
        return false;
    }

    const auto messages = channel->getMessageSnapshot();
    return std::any_of(messages.cbegin(), messages.cend(), [&](const auto &m) {
        return deleteActionTargetsMessage(m, messageID);
    });
}

MessagePtr generateBannedMessage(bool confirmedBan)
{
    const auto linkColor = MessageColor(MessageColor::Link);
    const auto accountsLink = Link(Link::Reconnect, QString());
    const auto bannedText =
        confirmedBan
            ? QString("You were banned from this channel!")
            : QString(
                  "Your connection to this channel was unexpectedly dropped.");

    const auto reconnectPromptText =
        confirmedBan
            ? QString(
                  "If you believe you have been unbanned, try reconnecting.")
            : QString("Try reconnecting.");

    MessageBuilder builder;
    auto text = QString("%1 %2").arg(bannedText, reconnectPromptText);
    builder.message().messageText = text;
    builder.message().searchText = text;
    builder.message().flags.set(MessageFlag::System);

    builder.emplace<TimestampElement>();
    builder.emplace<TextElement>(bannedText, MessageElementFlag::Text,
                                 MessageColor::System);
    builder
        .emplace<TextElement>(reconnectPromptText, MessageElementFlag::Text,
                              linkColor)
        ->setLink(accountsLink);

    return builder.release();
}

int stripLeadingReplyMention(Communi::TagsRef tags, QString &content)
{
    if (!getSettings()->stripReplyMention)
    {
        return 0;
    }
    if (getSettings()->hideReplyContext)
    {
        // Never strip reply mentions if reply contexts are hidden
        return 0;
    }

    if (const auto it = tags.get("reply-parent-display-name"); it.has_value())
    {
        auto displayName = parseTagString((*it));

        if (content.length() <= 1 + displayName.length())
        {
            // The reply contains no content
            return 0;
        }

        if (content.startsWith('@') &&
            content.at(1 + displayName.length()) == ' ' &&
            content.indexOf(displayName, 1) == 1)
        {
            int messageOffset = 1 + displayName.length() + 1;
            content.remove(0, messageOffset);
            return messageOffset;
        }
    }
    return 0;
}

void checkThreadSubscription(Communi::TagsRef tags, const QString &senderLogin,
                             std::shared_ptr<MessageThread> &thread)
{
    if (thread->subscribed() || thread->unsubscribed())
    {
        return;
    }

    if (getSettings()->autoSubToParticipatedThreads)
    {
        const auto &currentLogin =
            getApp()->getAccounts()->twitch.getCurrent()->getUserName();

        if (senderLogin == currentLogin)
        {
            thread->markSubscribed();
        }
        else if (const auto it = tags.get("reply-parent-user-login");
                 it.has_value())
        {
            auto name = (*it);
            if (name == currentLogin)
            {
                thread->markSubscribed();
            }
        }
    }
}

ChannelPtr channelOrEmptyByTarget(const QString &target,
                                  ITwitchIrcServer &server)
{
    QString channelName;
    if (!trimChannelName(target, channelName))
    {
        return Channel::getEmpty();
    }

    return server.getChannelOrEmpty(channelName);
}

QMap<QString, QString> parseBadges(const QString &badgesString)
{
    QMap<QString, QString> badges;

    for (const auto &badgeData : badgesString.split(','))
    {
        auto parts = badgeData.split('/');
        if (parts.length() != 2)
        {
            continue;
        }

        badges.insert(parts[0], parts[1]);
    }

    return badges;
}

struct ReplyContext {
    std::shared_ptr<MessageThread> thread;
    MessagePtr parent;
};

std::optional<ClearChatMessage> parseClearChatMessage(
    Communi::IrcMessage *message)
{
    // check parameter count
    if (message->parameters().length() < 1)
    {
        return std::nullopt;
    }

    // check if the chat has been cleared by a moderator
    if (message->parameters().length() == 1)
    {
        QString channelName;
        trimChannelName(message->parameter(0), channelName);

        return ClearChatMessage{
            .message = MessageBuilder::makeClearChatMessage(
                calculateMessageTime(message), {}, channelName),
            .disableAllMessages = true,
        };
    }

    // get username, duration and message of the timed out user
    QString username = message->parameter(1);
    QString durationInSeconds;
    QVariant v = message->tag("ban-duration");
    if (v.isValid())
    {
        durationInSeconds = v.toString();
    }

    auto timeoutMsg =
        MessageBuilder(timeoutMessage, username, durationInSeconds, false,
                       calculateMessageTime(message))
            .release();
    trimChannelName(message->parameter(0), timeoutMsg->channelName);

    return ClearChatMessage{.message = timeoutMsg,
                            .disableAllMessages = false,
                            .username = username};
}

/**
 * Parse a single IRC NOTICE message into a Chatterino message
 **/
MessagePtr parseNoticeMessage(Communi::IrcNoticeMessage *message)
{
    assert(message != nullptr);

    if (message->content().startsWith("Login auth", Qt::CaseInsensitive))
    {
        const auto linkColor = MessageColor(MessageColor::Link);
        const auto accountsLink = Link(Link::OpenAccountsPage, QString());
        const auto curUser = getApp()->getAccounts()->twitch.getCurrent();
        const auto expirationText = QString("Login expired for user \"%1\"!")
                                        .arg(curUser->getUserName());
        const auto loginPromptText = QString("Try adding your account again.");

        MessageBuilder builder;
        auto text = QString("%1 %2").arg(expirationText, loginPromptText);
        builder.message().messageText = text;
        builder.message().searchText = text;
        builder.message().flags.set(MessageFlag::System);
        builder.message().flags.set(MessageFlag::DoNotTriggerNotification);

        builder.emplace<TimestampElement>();
        builder.emplace<TextElement>(expirationText, MessageElementFlag::Text,
                                     MessageColor::System);
        builder
            .emplace<TextElement>(loginPromptText, MessageElementFlag::Text,
                                  linkColor)
            ->setLink(accountsLink);

        return builder.release();
    }

    if (message->content().startsWith("You are permanently banned "))
    {
        return {generateBannedMessage(true)};
    }

    if (message->tags().getOrEmpty("msg-id") == "msg_timedout")
    {
        QString remainingTime =
            formatTime(message->content().split(" ").value(5));
        QString formattedMessage =
            QString("You are timed out for %1.")
                .arg(remainingTime.isEmpty() ? "0s" : remainingTime);

        return makeSystemMessage(formattedMessage,
                                 calculateMessageTime(message).time());
    }

    // default case
    return makeSystemMessage(message->content(),
                             calculateMessageTime(message).time());
}

bool isWarningAcknowledgeNotice(const QString &text)
{
    return text.startsWith(
               "You received a Warning from a moderator in this channel.",
               Qt::CaseInsensitive) ||
           text.contains("Acknowledge the Warning at",
                         Qt::CaseInsensitive);
}

bool isRaidCanceledNoticeText(const QString &text)
{
    const auto content = parseTagString(text).trimmed();
    return content.contains("raid has been canceled", Qt::CaseInsensitive) ||
           content.contains("raid has been cancelled", Qt::CaseInsensitive);
}

bool isRaidCanceledNotice(Communi::IrcNoticeMessage *message)
{
    return message != nullptr && isRaidCanceledNoticeText(message->content());
}

}  // namespace

namespace chatterino {

using namespace literals;

IrcMessageHandler &IrcMessageHandler::instance()
{
    static IrcMessageHandler instance;
    return instance;
}

void IrcMessageHandler::parseMessageInto(Communi::IrcMessage *message,
                                         MessageSink &sink,
                                         TwitchChannel *channel)
{
    auto command = message->command();

    if (command == u"PRIVMSG"_s)
    {
        parsePrivMessageInto(
            dynamic_cast<Communi::IrcPrivateMessage *>(message), sink, channel);
    }
    else if (command == u"USERNOTICE"_s)
    {
        parseUserNoticeMessageInto(message, sink, channel);
    }

    if (command == u"NOTICE"_s)
    {
        auto *notice = dynamic_cast<Communi::IrcNoticeMessage *>(message);
        if (isRaidCanceledNotice(notice))
        {
            if (channel != nullptr)
            {
                channel->clearActiveRaid();
            }
            return;
        }
        if (notice != nullptr && channel != nullptr &&
            isWarningAcknowledgeNotice(notice->content()))
        {
            channel->handleChatWarningNotice();
            return;
        }

        auto parsed = parseNoticeMessage(notice);
        if (parsed && isRaidCanceledNoticeText(parsed->messageText))
        {
            if (channel != nullptr)
            {
                channel->clearActiveRaid();
            }
            return;
        }
        sink.addMessage(parsed, MessageContext::Original);
    }

    if (command == u"CLEARCHAT"_s)
    {
        auto cc = parseClearChatMessage(message);
        if (!cc)
        {
            return;
        }
        auto &clearChat = *cc;
        auto time = calculateMessageTime(message);
        if (clearChat.disableAllMessages)
        {
            sink.addOrReplaceClearChat(std::move(clearChat.message), time);
        }
        else
        {
            sink.addOrReplaceTimeout(std::move(clearChat.message), time);
        }
    }

    if (command == u"CLEARMSG"_s)
    {
        // check parameter count
        if (message->parameters().length() < 1)
        {
            return;
        }

        QString chanName;
        if (!trimChannelName(message->parameter(0), chanName))
        {
            return;
        }

        auto tags = message->tags();

        QString targetID = tags.getOrEmpty("target-msg-id");

        auto msg = sink.findMessageByID(targetID);
        if (msg == nullptr)
        {
            return;
        }

        msg->flags.set(MessageFlag::Disabled);
        msg->flags.set(MessageFlag::InvalidReplyTarget);
        if (!getSettings()->hideDeletionActions)
        {
            sink.addMessage(MessageBuilder::makeDeletionMessageFromIRC(msg),
                            MessageContext::Original);
        }
    }
}

void IrcMessageHandler::handlePrivMessage(Communi::IrcPrivateMessage *message,
                                          ITwitchIrcServer &twitchServer)
{
    auto chan = channelOrEmptyByTarget(message->target(), twitchServer);
    if (chan->isEmpty())
    {
        return;
    }

    auto *twitchChannel = dynamic_cast<TwitchChannel *>(chan.get());
    if (!twitchChannel)
    {
        return;
    }

    parsePrivMessageInto(message, *twitchChannel, twitchChannel);
}

void IrcMessageHandler::parsePrivMessageInto(
    Communi::IrcPrivateMessage *message, MessageSink &sink,
    TwitchChannel *channel)
{
    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    auto userID = message->tags().get("user-id");
    auto isCurrentUser = userID && *userID == currentUser->getUserId();
    auto shouldUpdateChannelFromMessage =
        isCurrentUser &&
        sink.sinkTraits().has(MessageSinkTrait::UpdateCurrentUserState);

    if (shouldUpdateChannelFromMessage)
    {
        if (auto badgesTag = message->tags().get("badges"))
        {
            auto parsedBadges = parseBadges(*badgesTag);
            channel->setCurrentUserBadges(currentUser->getUserId(),
                                          parseBadgeTag(message->tags()));
            channel->setMod(parsedBadges.contains("moderator") ||
                            parsedBadges.contains("lead_moderator"));
            channel->setVIP(parsedBadges.contains("vip"));
            channel->setStaff(parsedBadges.contains("staff"));
        }

        // Clear the send wait timer when we are able to send a message
        channel->setSendWait(0);

        // Update send wait timer with slow mode timeout if this user is not a mod or vip.
        if (!channel->hasHighRateLimit())
        {
            auto roomModes = *channel->accessRoomModes();
            if (roomModes.slowMode > 0)
            {
                channel->setSendWait(roomModes.slowMode);
            }
        }
    }

    IrcMessageHandler::addMessage(message, sink, channel,
                                  unescapeZeroWidthJoiner(message->content()),
                                  *getApp()->getTwitch(),
                                  {
                                      .isAction = message->isAction(),
                                  });
}

void IrcMessageHandler::handleRoomStateMessage(Communi::IrcMessage *message)
{
    auto tags = message->tags();

    // get Twitch channel
    QString chanName;
    if (!trimChannelName(message->parameter(0), chanName))
    {
        return;
    }
    auto chan = getApp()->getTwitch()->getChannelOrEmpty(chanName);

    auto *twitchChannel = dynamic_cast<TwitchChannel *>(chan.get());
    if (!twitchChannel)
    {
        return;
    }

    // room-id

    if (auto it = tags.get("room-id"); it.has_value())
    {
        auto roomId = (*it);
        twitchChannel->setRoomId(roomId);
    }

    // Room modes
    {
        auto roomModes = *twitchChannel->accessRoomModes();

        if (auto it = tags.get("emote-only"); it.has_value())
        {
            roomModes.emoteOnly = (*it) == "1";
        }
        if (auto it = tags.get("subs-only"); it.has_value())
        {
            roomModes.submode = (*it) == "1";
        }
        if (auto it = tags.get("slow"); it.has_value())
        {
            roomModes.slowMode = it->toInt();
        }
        if (auto it = tags.get("r9k"); it.has_value())
        {
            roomModes.r9k = (*it) == "1";
        }
        if (auto it = tags.get("followers-only"); it.has_value())
        {
            roomModes.followerOnly = it->toInt();
        }
        twitchChannel->setRoomModes(roomModes);
    }

    twitchChannel->roomModesChanged.invoke();
}

void IrcMessageHandler::handleClearChatMessage(Communi::IrcMessage *message)
{
    auto cc = parseClearChatMessage(message);
    if (!cc)
    {
        return;
    }
    auto &clearChat = *cc;

    QString chanName;
    if (!trimChannelName(message->parameter(0), chanName))
    {
        return;
    }

    // get channel
    auto chan = getApp()->getTwitch()->getChannelOrEmpty(chanName);

    if (chan->isEmpty())
    {
        qCDebug(chatterinoTwitch)
            << "[IrcMessageHandler::handleClearChatMessage] Twitch channel"
            << chanName << "not found";
        return;
    }

    auto time = calculateMessageTime(message);
    // chat has been cleared by a moderator
    if (!message->tags().has("historical"))
    {
        recording::publicEvent(
            *chan, {{"kind", clearChat.disableAllMessages ? "chatCleared"
                                                          : "userTimedOut"},
                    {"text", clearChat.message->messageText},
                    {"targetUser", clearChat.username.value_or("")},
                    {"durationSeconds",
                     message->tags().getOrEmpty("ban-duration").toInt()},
                    {"createdAt", time.toUTC().toString(Qt::ISODateWithMs)}});
    }
    if (clearChat.disableAllMessages)
    {
        chan->disableAllMessages();
        chan->addOrReplaceClearChat(std::move(clearChat.message), time);
    }
    else
    {
        // Set send wait timer when the user is timed out
        const auto currentUsername =
            getApp()->getAccounts()->twitch.getCurrent()->getUserName();
        if (currentUsername == clearChat.username)
        {
            bool ok = false;
            int remainingTime =
                message->tags().getOrEmpty("ban-duration").toInt(&ok);
            if (ok)
            {
                auto *tc = dynamic_cast<TwitchChannel *>(chan.get());
                assert(tc != nullptr);
                if (tc != nullptr)
                {
                    tc->setSendWait(remainingTime);
                }
            }
        }

        chan->addOrReplaceTimeout(std::move(clearChat.message), time);
    }

    if (getSettings()->hideModerated)
    {
        // XXX: This is expensive. We could use a layout request if the layout
        //      would store the previous message flags.
        getApp()->getWindows()->forceLayoutChannelViews();
    }
}

void IrcMessageHandler::handleClearMessageMessage(Communi::IrcMessage *message)
{
    // check parameter count
    if (message->parameters().length() < 1)
    {
        return;
    }

    QString chanName;
    if (!trimChannelName(message->parameter(0), chanName))
    {
        return;
    }

    // get channel
    auto chan = getApp()->getTwitch()->getChannelOrEmpty(chanName);

    if (chan->isEmpty())
    {
        qCDebug(chatterinoTwitch)
            << "[IrcMessageHandler:handleClearMessageMessage] Twitch "
               "channel"
            << chanName << "not found";
        return;
    }

    auto tags = message->tags();

    QString targetID = tags.getOrEmpty("target-msg-id");
    if (!tags.has("historical"))
    {
        recording::publicEvent(*chan, {{"kind", "messageDeleted"},
                                       {"id", "delete:" + targetID},
                                       {"targetMessageId", targetID},
                                       {"text", "Message deleted"}});
    }

    auto msg = chan->findMessageByID(targetID);
    if (msg == nullptr)
    {
        return;
    }

    msg->flags.set(MessageFlag::Disabled);
    msg->flags.set(MessageFlag::InvalidReplyTarget);
    if (!getSettings()->hideDeletionActions &&
        !hasDeleteActionForMessage(chan.get(), targetID))
    {
        chan->addMessage(MessageBuilder::makeDeletionMessageFromIRC(msg),
                         MessageContext::Original);
    }

    if (getSettings()->hideModerated && !tags.has("historical"))
    {
        // XXX: This is expensive. We could use a layout request if the layout
        //      would store the previous message flags.
        getApp()->getWindows()->forceLayoutChannelViews();
    }
}

void IrcMessageHandler::handleUserStateMessage(Communi::IrcMessage *message)
{
    QString channelName;
    if (!trimChannelName(message->parameter(0), channelName))
    {
        return;
    }

    auto c = getApp()->getTwitch()->getChannelOrEmpty(channelName);
    if (c->isEmpty())
    {
        return;
    }

    auto *tc = dynamic_cast<TwitchChannel *>(c.get());
    if (tc != nullptr)
    {
        bool hasModBadge = false;

        // Checking if currentUser is a VIP, staff member or has moderator badges
        if (auto badgesTag = message->tags().get("badges"))
        {
            auto parsedBadges = parseBadges(*badgesTag);
            const auto currentUser =
                getApp()->getAccounts()->twitch.getCurrent();
            tc->setCurrentUserBadges(currentUser->getUserId(),
                                     parseBadgeTag(message->tags()));
            tc->setVIP(parsedBadges.contains("vip"));
            tc->setStaff(parsedBadges.contains("staff"));

            hasModBadge = parsedBadges.contains("moderator") ||
                          parsedBadges.contains("lead_moderator");
        }

        if (hasModBadge)
        {
            tc->setMod(true);
        }
        else
        {
            if (auto modTag = message->tags().get("mod"))
            {
                // Also checking if the mod tag is present, since badges sometimes disappear in IRC
                tc->setMod(*modTag == "1");
            }
        }

        // When a user is updated to mod or vip status, any previous
        // slow mode or timeout timers are no longer in effect.
        if (tc->hasHighRateLimit())
        {
            tc->setSendWait(0);
        }
        else
        {
            // USERSTATE reflects the current ability to chat for the
            // logged-in user. Recompute the send-wait timer from the
            // current room state instead of keeping an old timeout value
            // that may have already been removed by an untimeout/unban.
            tc->setSendWait(0);

            auto roomModes = *tc->accessRoomModes();
            if (roomModes.slowMode > 0)
            {
                tc->setSendWait(roomModes.slowMode);
            }
        }
    }
}

void IrcMessageHandler::handleWhisperMessage(Communi::IrcMessage *ircMessage)
{
    MessageParseArgs args;

    args.isReceivedWhisper = true;

    auto *c = getApp()->getTwitch()->getWhispersChannel().get();

    auto [message, alert] = MessageBuilder::makeIrcMessage(
        c, ircMessage, args, unescapeZeroWidthJoiner(ircMessage->parameter(1)),
        0);
    if (!message)
    {
        return;
    }

    message->flags.set(MessageFlag::Whisper);
    MessageBuilder::triggerHighlights(c, message, alert);

    getApp()->getTwitch()->setLastUserThatWhisperedMe(message->loginName);

    if (message->flags.has(MessageFlag::ShowInMentions))
    {
        getApp()->getTwitch()->getMentionsChannel()->addMessage(
            message, MessageContext::Original);
    }

    c->addMessage(message, MessageContext::Original);

    auto overrideFlags = std::optional<MessageFlags>(message->flags);
    overrideFlags->set(MessageFlag::DoNotTriggerNotification);
    overrideFlags->set(MessageFlag::DoNotLog);

    if (getSettings()->inlineWhispers &&
        !(getSettings()->streamerModeSuppressInlineWhispers &&
          getApp()->getStreamerMode()->isEnabled()))
    {
        getApp()->getTwitch()->forEachChannel([&message, overrideFlags](
                                                  ChannelPtr channel) {
            channel->addMessage(message, MessageContext::Repost, overrideFlags);
        });
    }
}

void IrcMessageHandler::handleUserNoticeMessage(Communi::IrcMessage *message,
                                                ITwitchIrcServer &twitchServer)
{
    auto target = message->parameter(0);
    auto *channel = dynamic_cast<TwitchChannel *>(
        twitchServer.getChannelOrEmpty(target).get());
    if (!channel)
    {
        return;
    }
    parseUserNoticeMessageInto(message, *channel, channel);
    activityfeed::addUserNotice(*message, *channel);
}

void IrcMessageHandler::parseUserNoticeMessageInto(Communi::IrcMessage *message,
                                                   MessageSink &sink,
                                                   TwitchChannel *channel)
{
    assert(message != nullptr);
    assert(channel != nullptr);

    const auto *userDataController = getApp()->getUserData();
    assert(userDataController != nullptr);

    auto tags = message->tags();
    auto parameters = message->parameters();

    QString msgType = tags.getOrEmpty("msg-id");
    bool mirrored = msgType == "sharedchatnotice";
    if (mirrored)
    {
        msgType = tags.getOrEmpty("source-msg-id");
    }
    else
    {
        auto rIt = tags.get("room-id");
        auto sIt = tags.get("source-room-id");
        if (rIt.has_value() && sIt.has_value())
        {
            mirrored = (*rIt) != (*sIt);
        }
    }

    if (mirrored && msgType != "announcement")
    {
        // avoid confusing broadcasters with user payments to other channels
        return;
    }

    QString content;
    if (parameters.size() >= 2)
    {
        content = parameters[1];
    }

    if (isIgnoredMessage({
            .message = content,
            .twitchUserID = tags.getOrEmpty("user-id"),
            .isMod = channel->isMod(),
            .isBroadcaster = channel->isBroadcaster(),
        }))
    {
        return;
    }

    if (msgType == "sub" || msgType == "resub" || msgType == "viewermilestone")
    {
        addMessage(
            message, sink, channel, content, *getApp()->getTwitch(),
            {.isSub = SUB_MESSAGE_TYPES.contains(msgType), .isSpecial = true});
        return;
    }

    if (SPECIAL_MESSAGE_TYPES.contains(msgType))
    {
        // Messages are not required, so they might be empty
        if (!content.isEmpty())
        {
            addMessage(message, sink, channel, content, *getApp()->getTwitch(),
                       {
                           .isSub = SUB_MESSAGE_TYPES.contains(msgType),
                           .isSpecial = true,
                       });
        }
    }

    auto it = tags.get("system-msg");

    if (it.has_value())
    {
        // By default, we return value of system-msg tag
        QString messageText = (*it);
        auto displayName = [&] {
            if (msgType == u"raid")
            {
                return tags.getOrEmpty("msg-param-displayName");
            }
            return tags.getOrEmpty("display-name");
        }();
        auto login = tags.getOrEmpty("login");
        if (displayName.isEmpty())
        {
            displayName = login;
        }

        if (msgType == "bitsbadgetier")
        {
            messageText =
                QString("%1 just earned a new %2 Bits badge!")
                    .arg(tags.getOrEmpty("display-name"),
                         kFormatNumbers(
                             tags.getOrEmpty("msg-param-threshold").toInt()));
        }
        else if (msgType == "announcement")
        {
            // Early out - announcement headers are added in MessageBuilder
            return;
        }
        else if (msgType == "subgift")
        {
            // subgifts are special because they include two users
            auto msg = MessageBuilder::makeSubgiftMessage(
                tags, calculateMessageTime(message).time(), channel);

            addRecordedUserNotice(sink, channel, msg, tags);
            return;
        }

        else if (msgType == "modiversary")
        {
            // Twitch currently omits the user name in some modiversary system
            // messages, unlike normal watch-streak notices.
            const auto startsWithUser =
                (!login.isEmpty() && messageText.startsWith(login)) ||
                (!displayName.isEmpty() && messageText.startsWith(displayName));
            if (!displayName.isEmpty() && !startsWithUser)
            {
                messageText = displayName % ' ' % messageText;
            }
        }
        else if (msgType == "socialsharingbadge")
        {
            int level =
                tags.getOrEmpty("msg-param-current-badge-level").toInt();
            messageText = QString("%1 earned a Level %2 Social Media Badge!")
                              .arg(tags.getOrEmpty("display-name"),
                                   QString::number(level));
        }

        auto userID = tags.getOrEmpty("user-id");
        auto userColor =
            twitch::getUserColor(
                {
                    .userLogin = login,
                    .userID = userID,
                    .userDataController = userDataController,
                    .channelChatters = channel,
                    .color = QColor::fromString(tags.getOrEmpty("color")),
                })
                .value_or(MessageColor::System);

        if (isRaidCanceledNoticeText(messageText))
        {
            if (channel != nullptr)
            {
                channel->clearActiveRaid();
            }
            return;
        }

        auto msg = MessageBuilder::makeSystemMessageWithUser(
            parseTagString(messageText), login, displayName, userColor,
            calculateMessageTime(message).time(), *message, channel);

        addRecordedUserNotice(sink, channel, msg, tags);
    }
}

void IrcMessageHandler::handleNoticeMessage(Communi::IrcNoticeMessage *message)
{
    auto msg = parseNoticeMessage(message);
    const auto isRaidCancelNotice =
        isRaidCanceledNotice(message) ||
        (msg && isRaidCanceledNoticeText(msg->messageText));

    if (message->content().startsWith("Login auth", Qt::CaseInsensitive))
    {
        getApp()->getAccounts()->twitch.loginExpired.invoke();
    }

    QString channelName;
    if (!trimChannelName(message->target(), channelName) ||
        channelName == "jtv")
    {
        if (isRaidCancelNotice)
        {
            return;
        }

        // Notice wasn't targeted at a single channel, send to all twitch
        // channels
        getApp()->getTwitch()->forEachChannelAndSpecialChannels(
            [msg](const auto &c) {
                c->addMessage(msg, MessageContext::Original);
            });

        return;
    }

    auto channel = getApp()->getTwitch()->getChannelOrEmpty(channelName);

    if (channel->isEmpty())
    {
        qCDebug(chatterinoTwitch)
            << "[IrcManager:handleNoticeMessage] Channel" << channelName
            << "not found in channel manager";
        return;
    }

    if (isRaidCancelNotice)
    {
        if (auto *tc = dynamic_cast<TwitchChannel *>(channel.get()))
        {
            tc->clearActiveRaid();
        }
        return;
    }

    if (isWarningAcknowledgeNotice(message->content()))
    {
        if (auto *tc = dynamic_cast<TwitchChannel *>(channel.get()))
        {
            tc->handleChatWarningNotice();
        }
        return;
    }

    QString tags = message->tags().getOrEmpty("msg-id");
    if (tags == "usage_delete")
    {
        channel->addSystemMessage(
            "Usage: /delete <msg-id> - Deletes the specified message. "
            "Can't take more than one argument.");
    }
    else if (tags == "bad_delete_message_error")
    {
        channel->addSystemMessage(
            "There was a problem deleting the message. "
            "It might be from another channel or too old to delete.");
    }
    else if (tags == "host_on" || tags == "host_target_went_offline")
    {
        bool hostOn = (tags == "host_on");
        QStringList parts = msg->messageText.split(QLatin1Char(' '));
        if ((hostOn && parts.size() != 3) || (!hostOn && parts.size() != 7))
        {
            return;
        }
        auto &hostedChannelName = hostOn ? parts[2] : parts[0];
        if (hostedChannelName.size() < 2)
        {
            return;
        }
        if (hostOn)
        {
            hostedChannelName.chop(1);
        }
        channel->addMessage(
            MessageBuilder::makeHostingSystemMessage(hostedChannelName, hostOn),
            MessageContext::Original);
    }
    else if (tags == "room_mods" || tags == "vips_success")
    {
        // /mods and /vips
        // room_mods: The moderators of this channel are: ampzyh, antichriststollen, apa420, ...
        // vips_success: The VIPs of this channel are: 8008, aiden, botfactory, ...

        QString noticeText = msg->messageText;
        if (tags == "vips_success")
        {
            // this one has a trailing period, need to get rid of it.
            noticeText.chop(1);
        }

        QStringList msgParts = noticeText.split(':');
        MessageBuilder builder;

        auto *tc = dynamic_cast<TwitchChannel *>(channel.get());
        assert(tc != nullptr &&
               "IrcMessageHandler::handleNoticeMessage. Twitch specific "
               "functionality called in non twitch channel");

        auto users = msgParts.at(1)
                         .mid(1)  // there is a space before the first user
                         .split(", ");
        users.sort(Qt::CaseInsensitive);
        channel->addMessage(
            MessageBuilder::makeListOfUsersMessage(msgParts.at(0), users, tc),
            MessageContext::Original);
    }
    else
    {
        channel->addMessage(msg, MessageContext::Original);
    }

    auto handleSendWait = [&channel](const QString &remaining) {
        bool ok = false;
        int seconds = remaining.toInt(&ok);
        if (ok)
        {
            auto *tc = dynamic_cast<TwitchChannel *>(channel.get());
            assert(tc != nullptr);
            if (tc != nullptr)
            {
                tc->setSendWait(seconds);
            }
        }
    };

    if (tags == "msg_slowmode")
    {
        // Notice received when the user sends a message too quickly during slow mode.
        // @msg-id=msg_slowmode :tmi.twitch.tv NOTICE #channel :This room is in slow mode and you are sending messages too quickly. You will be able to talk again in 10 seconds.
        handleSendWait(message->content().split(u' ').value(21));
    }
    else if (tags == "msg_timedout")
    {
        // Notice received when the user sends a message while timed out.
        // @msg-id=msg_timedout :tmi.twitch.tv NOTICE #twitch :You are timed out for 3600 more seconds.
        handleSendWait(message->content().split(u' ').value(5));
    }
}

void IrcMessageHandler::handleJoinMessage(Communi::IrcMessage *message)
{
    auto channel = getApp()->getTwitch()->getChannelOrEmpty(
        message->parameter(0).remove(0, 1));

    auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get());
    if (!twitchChannel)
    {
        return;
    }

    const auto ownNick =
        message->connection()
            ? message->connection()->nickName()
            : getApp()->getAccounts()->twitch.getCurrent()->getUserName();
    if (message->nick().compare(ownNick, Qt::CaseInsensitive) == 0)
    {
        twitchChannel->addSystemMessage("joined channel",
                                        MessageFlag::ConnectionNotice);
        twitchChannel->joined.invoke();
    }
    else if (getSettings()->showJoins.getValue())
    {
        twitchChannel->addJoinedUser(message->nick(), twitchChannel->isMod(),
                                     twitchChannel->isBroadcaster());
    }
}

void IrcMessageHandler::handlePartMessage(Communi::IrcMessage *message)
{
    auto channel = getApp()->getTwitch()->getChannelOrEmpty(
        message->parameter(0).remove(0, 1));

    auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get());
    if (!twitchChannel)
    {
        return;
    }

    const auto selfAccountName =
        message->connection()
            ? message->connection()->nickName()
            : getApp()->getAccounts()->twitch.getCurrent()->getUserName();
    if (message->nick() != selfAccountName &&
        getSettings()->showParts.getValue())
    {
        twitchChannel->addPartedUser(message->nick(), twitchChannel->isMod(),
                                     twitchChannel->isBroadcaster());
    }

    if (message->nick() == selfAccountName)
    {
        if (!twitchChannel->consumeExpectedReconnectPart())
        {
            channel->addMessage(generateBannedMessage(false),
                                MessageContext::Original);
        }
    }
}

void IrcMessageHandler::addMessage(Communi::IrcMessage *message,
                                   MessageSink &sink, TwitchChannel *chan,
                                   const QString &originalContent,
                                   ITwitchIrcServer &twitch,
                                   AddMessageArgs addArgs)
{
    assert(chan);

    auto isSub = addArgs.isSub;
    auto isAction = addArgs.isAction;

    MessageParseArgs args;
    args.isSubscriptionMessage = isSub;
    if (addArgs.isSpecial)
    {
        args.trimSubscriberUsername = true;
    }

    args.isAction = isAction;

    auto tags = message->tags();
    const bool isGigantifiedEmote =
        tags.getOrEmpty("msg-id") == "gigantified-emote-message";
    args.isGigantifiedEmote = isGigantifiedEmote;
    QString rewardId;
    if (const auto it = tags.get("custom-reward-id"); it.has_value())
    {
        rewardId = (*it);
    }
    else if (const auto typeIt = tags.get("msg-id"); typeIt.has_value())
    {
        // slight hack to treat bits power-ups as channel point redemptions
        const auto msgId = (*typeIt);
        if (msgId == "animated-message" || msgId == "gigantified-emote-message")
        {
            rewardId = msgId;
        }
    }
    if (!rewardId.isEmpty() &&
        sink.sinkTraits().has(
            MessageSinkTrait::RequiresKnownChannelPointReward))
    {
        const auto messageId = tags.getOrEmpty("id");
        if (!messageId.isEmpty())
        {
            auto roomId = tags.getOrEmpty("room-id");
            if (roomId.isEmpty())
            {
                roomId = chan->roomId();
            }

            if (!chan->markChannelPointRedemptionSeen(u"irc:" % roomId %
                                                      u':' % messageId))
            {
                return;
            }
        }
    }

    if (!rewardId.isEmpty() &&
        sink.sinkTraits().has(
            MessageSinkTrait::RequiresKnownChannelPointReward) &&
        !chan->isChannelPointRewardKnown(rewardId))
    {
        // Need to wait for pubsub reward notification
        qCDebug(chatterinoTwitch) << "TwitchChannel reward added ADD "
                                     "callback since reward is not known:"
                                  << rewardId;
        chan->addQueuedRedemption(rewardId, originalContent, message);
    }
    args.channelPointRewardId = rewardId;

    QString content = originalContent;
    int messageOffset = stripLeadingReplyMention(tags, content);

    ReplyContext replyCtx;

    if (const auto it = tags.get("reply-thread-parent-msg-id"); it.has_value())
    {
        const QString replyID = (*it);
        auto threadIt = chan->threads().find(replyID);
        std::shared_ptr<MessageThread> rootThread;
        if (threadIt != chan->threads().end() && !threadIt->second.expired())
        {
            // Thread already exists (has a reply)
            auto thread = threadIt->second.lock();
            checkThreadSubscription(tags, message->nick(), thread);
            replyCtx.thread = thread;
            rootThread = thread;
        }
        else
        {
            // Thread does not yet exist, find root reply and create thread.
            auto root = sink.findMessageByID(replyID);
            if (root)
            {
                // Found root reply message
                auto newThread = std::make_shared<MessageThread>(root);
                checkThreadSubscription(tags, message->nick(), newThread);

                replyCtx.thread = newThread;
                rootThread = newThread;
                // Store weak reference to thread in channel
                chan->addReplyThread(newThread);
            }
        }

        if (const auto parentIt = tags.get("reply-parent-msg-id");
            parentIt.has_value())
        {
            const QString parentID = (*parentIt);
            if (replyID == parentID)
            {
                if (rootThread)
                {
                    replyCtx.parent = rootThread->root();
                }
            }
            else
            {
                auto parentThreadIt = chan->threads().find(parentID);
                if (parentThreadIt != chan->threads().end())
                {
                    auto thread = parentThreadIt->second.lock();
                    if (thread)
                    {
                        replyCtx.parent = thread->root();
                    }
                }
                else
                {
                    auto parent = sink.findMessageByID(parentID);
                    if (parent)
                    {
                        replyCtx.parent = parent;
                    }
                }
            }
        }
    }

    args.allowIgnore = !isSub;
    auto [msg, alert] = MessageBuilder::makeIrcMessage(
        chan, message, args, content, messageOffset, replyCtx.thread,
        replyCtx.parent);

    if (msg)
    {
        chan->applyMonitoredStatus(msg);
        sink.applySimilarityFilters(msg);

        if (!msg->flags.has(MessageFlag::Similar) ||
            (!getSettings()->hideSimilar &&
             getSettings()->shownSimilarTriggerHighlights))
        {
            MessageBuilder::triggerHighlights(chan, msg, alert);
        }

        const auto highlighted = msg->flags.has(MessageFlag::Highlighted);
        const auto showInMentions = msg->flags.has(MessageFlag::ShowInMentions);

        if (highlighted && showInMentions &&
            sink.sinkTraits().has(MessageSinkTrait::AddMentionsToGlobalChannel))
        {
            twitch.getMentionsChannel()->addMessage(msg,
                                                    MessageContext::Original);
        }

        recording::deliverLive(
            msg,
            [&] {
                return recording::normalizeTwitch(*chan, *msg, originalContent,
                                                  tags.raw());
            },
            [&] {
                if (msg->flags.has(MessageFlag::SharedMessage))
                {
                    chan->probeSharedChatSession();
                }

                sink.addMessage(msg, MessageContext::Original);
            });
        chan->addRecentChatter(msg->displayName);
        if (&sink == static_cast<MessageSink *>(chan) ||
            msg->flags.has(MessageFlag::FirstMessage))
        {
            updateSharedFirstMessage(chan, msg, tags);
        }
    }
}

}  // namespace chatterino
