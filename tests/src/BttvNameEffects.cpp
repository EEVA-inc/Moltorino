#include "messages/Emote.hpp"
#include "providers/bttv/BttvBadges.hpp"
#include "providers/bttv/liveupdates/BttvLiveUpdateMessages.hpp"
#include "Test.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace chatterino;

TEST(BttvNameEffects, ParsesLookupUser)
{
    const auto json = QJsonDocument::fromJson(R"({
        "name": "SomeUser",
        "providerId": "413635733",
        "channel": "twitch:71092938",
        "pro": true,
        "usernameEffect": "iridescence",
        "usernameHoverEffect": "flip",
        "usernameFont": "comic",
        "badge": null
    })")
                          .object();

    BttvLiveUpdateUserUpdateMessage message(json);
    ASSERT_TRUE(message.validate());
    EXPECT_EQ(message.userID, "413635733");
    EXPECT_EQ(message.userName, "someuser");
    EXPECT_EQ(message.nameEffect, "iridescence");
    EXPECT_FALSE(message.hasBadge());
}

TEST(BttvNameEffects, NullEffectIsEmpty)
{
    const auto json = QJsonDocument::fromJson(R"({
        "name": "someuser",
        "providerId": "1",
        "usernameEffect": null
    })")
                          .object();

    BttvLiveUpdateUserUpdateMessage message(json);
    EXPECT_TRUE(message.nameEffect.isEmpty());
}

TEST(BttvNameEffects, ReportsChanges)
{
    BttvBadges badges;

    EXPECT_FALSE(badges.setNameEffect("someuser", {}));
    EXPECT_TRUE(badges.setNameEffect("SomeUser", "glow"));
    EXPECT_FALSE(badges.setNameEffect("someuser", "glow"));
    EXPECT_TRUE(badges.setNameEffect("someuser", "flare"));
    EXPECT_TRUE(badges.setNameEffect("someuser", {}));
    EXPECT_EQ(badges.getNameEffect("someuser"), nullptr);
}

TEST(BttvNameEffects, StaffBadges)
{
    BttvBadges badges;
    badges.setStaffBadges(QJsonDocument::fromJson(R"([
        {"providerId": "11785491", "badge": {"type": 1,
         "description": "NightDev Developer",
         "svg": "https://cdn.betterttv.net/badges/developer.svg"}},
        {"providerId": "2", "badge": {"type": 4,
         "description": "BetterTTV Translator",
         "svg": "https://cdn.betterttv.net/badges/translator.svg"}},
        {"providerId": "3", "badge": {"type": 4,
         "description": "BetterTTV Translator",
         "svg": "https://cdn.betterttv.net/badges/translator.svg"}}
    ])")
                              .array());

    const auto developer = badges.getStaffBadge(UserId{"11785491"});
    ASSERT_TRUE(developer.has_value());
    EXPECT_EQ((*developer)->name.string, "betterttv:developer");
    EXPECT_EQ((*developer)->tooltip.string, "NightDev Developer");

    // users with the same badge share one Emote
    EXPECT_EQ(*badges.getStaffBadge(UserId{"2"}),
              *badges.getStaffBadge(UserId{"3"}));
    EXPECT_FALSE(badges.getStaffBadge(UserId{"4"}).has_value());
}

TEST(BttvNameEffects, BotBadges)
{
    BttvBadges badges;
    badges.setChannelBots(
        "100",
        QJsonDocument::fromJson(R"(["StreamElements", "fossabot"])").array());

    EXPECT_NE(badges.getBotBadge("1", "Nightbot"), nullptr);
    EXPECT_NE(badges.getBotBadge("100", "streamelements"), nullptr);
    EXPECT_EQ(badges.getBotBadge("1", "streamelements"), nullptr);
    EXPECT_EQ(badges.getBotBadge("100", "someuser"), nullptr);
}
