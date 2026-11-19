#include "providers/youtube/YouTubeInnertubeParser.hpp"

#include "Test.hpp"

#include <QJsonArray>
#include <QJsonObject>

using namespace chatterino;

namespace {

// Same shape as a giftMessageViewModel from YouTube's live chat: the handle,
// the text and the picture, with no channel id or Jewels value
QJsonObject giftAction(const QJsonValue &text)
{
    QJsonObject model{
        {"id", "gift-1"},
        {"authorName", QJsonObject{{"content", "@SomeViewer "}}},
        {"giftImage",
         QJsonObject{
             {"sources",
              QJsonArray{
                  QJsonObject{{"url", "//example.com/hat.png=w480"},
                              {"width", 480}},
                  QJsonObject{{"url", "//example.com/hat.png=w640"},
                              {"width", 640}},
              }},
         }},
    };
    if (!text.isUndefined())
    {
        model.insert("text", QJsonObject{{"content", text}});
    }
    return {
        {"addChatItemAction",
         QJsonObject{{"item", QJsonObject{{"giftMessageViewModel", model}}}}},
    };
}

YouTubeInnertubeParseResult parse(const QJsonObject &action)
{
    return parseYouTubeInnertubeChat(
        QJsonObject{
            {"continuationContents",
             QJsonObject{
                 {"liveChatContinuation",
                  QJsonObject{{"actions", QJsonArray{action}}}},
             }},
        },
        u"live-chat-id");
}

}  // namespace

TEST(YouTubeInnertubeParser, JewelsGift)
{
    const auto result = parse(giftAction("sent Party hat"));
    ASSERT_TRUE(result.valid) << result.error.toStdString();
    ASSERT_EQ(result.messages.size(), 1);

    const auto &message = result.messages.front();
    EXPECT_EQ(message.kind, YouTubeMessageKind::Gift);
    EXPECT_EQ(message.id, "gift-1");
    EXPECT_EQ(message.eventText, "sent Party hat");
    EXPECT_EQ(message.giftImageUrl, "https://example.com/hat.png=w640");
    EXPECT_TRUE(message.author.channelId.isEmpty());
    EXPECT_FALSE(message.author.displayName.isEmpty());
}

TEST(YouTubeInnertubeParser, JewelsGiftWithoutTextIsSkipped)
{
    const auto result = parse(giftAction(QJsonValue::Undefined));
    ASSERT_TRUE(result.valid) << result.error.toStdString();
    EXPECT_TRUE(result.messages.empty());
}
