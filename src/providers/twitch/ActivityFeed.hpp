#pragma once

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <memory>
#include <optional>

namespace Communi {
class IrcMessage;
class IrcPrivateMessage;
}  // namespace Communi

namespace chatterino {

class TwitchChannel;
struct ChannelPointReward;
struct Message;
using MessagePtr = std::shared_ptr<const Message>;
enum class MessagePlatform : uint8_t;

/// Posts follows, subs, bits, raids and channel point redemptions from the
/// channels the current user moderates into the /activity split.
namespace activityfeed {

void addUserNotice(const Communi::IrcMessage &message, TwitchChannel &channel);
void addCheer(const Communi::IrcPrivateMessage &message,
              TwitchChannel &channel);
void addRedemption(const ChannelPointReward &reward, TwitchChannel &channel);

enum class EventType {
    Follow,
    Share,
    Sub,
    GiftSub,
    /// Cheers, Kicks, Super Chats, TikTok gifts and donations
    Paid,
    Raid,
    Redemption,
    /// Watch streaks and bits badges
    Milestone,
};

/// Kick, YouTube and TikTok events are shown as the platform's own message,
/// which already starts with the channel name. The caller checks that the
/// channel is one the user moderates or streams on. amount is what a paid
/// event was worth, for the minimum amount settings.
void addPlatformEvent(const MessagePtr &message, EventType type,
                      double amount = 0);

/// Number from a display amount like "$5.00" or "1.234,56 EUR"
double amountFromDisplayString(const QString &text);

/// Type of a line in the feed, if it's one the feed added
std::optional<EventType> eventTypeOf(const MessagePtr &message);
/// Plural name for the right-click menu, like "gifted subs"
QString eventTypeName(EventType type);

/// Right-click actions in the feed. Each one also takes matching lines that
/// are already shown back out.
void hideUser(const QString &login);
void hideType(EventType type);
void hidePlatform(MessagePlatform platform);

/// Twitch doesn't push follows to chat, so the newest followers are fetched
/// over GQL about once a minute while an Activity split is open and visible.
class FollowerPoller
{
public:
    FollowerPoller();

private:
    void poll();

    QTimer timer_;
    /// channel id -> newest follow that was already shown (or skipped on the
    /// first fetch, so opening the split doesn't dump old followers)
    std::shared_ptr<QHash<QString, QDateTime>> newestFollow_;
};

}  // namespace activityfeed

}  // namespace chatterino
