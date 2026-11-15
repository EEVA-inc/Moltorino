
#include "providers/bchat/BChatBadges.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <mutex>

namespace chatterino {
namespace {

constexpr auto BADGES_URL = "https://www.itsbr0dyy.dev/api/badges";
constexpr qsizetype MAX_PAYLOAD_SIZE = 1024 * 1024;
constexpr int REFRESH_INTERVAL = 30 * 60 * 1000;

bool validUserId(const QString &id)
{
    if (id.isEmpty() || id.size() > 20)
    {
        return false;
    }

    for (const QChar c : id)
    {
        if (c < QLatin1Char('0') || c > QLatin1Char('9'))
        {
            return false;
        }
    }

    return true;
}

bool validImageUrl(const QString &value)
{
    const QUrl url(value, QUrl::StrictMode);

    return value.size() <= 2048 &&
           url.isValid() &&
           url.scheme() == QStringLiteral("https") &&
           url.host().compare(
               QStringLiteral("itsbr0dyy.dev"),
               Qt::CaseInsensitive) == 0 &&
           url.userInfo().isEmpty() &&
           url.port(-1) == -1 &&
           !url.hasFragment();
}

std::shared_ptr<const Emote> makeBadge(
    const QString &id,
    const QJsonObject &object)
{
    const auto title = object.value("title").toString();
    const auto images = object.value("images").toObject();

    const auto imageUrl1x = images.value("1x").toString();
    const auto imageUrl2x = images.value("2x").toString();
    const auto imageUrl4x = images.value("4x").toString();

    if (id.isEmpty() || id.size() > 64 ||
        title.isEmpty() || title.size() > 256 ||
        !validImageUrl(imageUrl1x) ||
        !validImageUrl(imageUrl2x) ||
        !validImageUrl(imageUrl4x))
    {
        return nullptr;
    }

    return std::make_shared<const Emote>(Emote{
        .name = EmoteName{QStringLiteral("bchat:") + id},
        // The badge image is 256x256, but we want to display it at 18x18, 36x36, and 72x72 sizes.
        .images = ImageSet{
            Image::fromUrl(Url{imageUrl1x}, 18.0 / 256.0, QSize(18, 18)),
            Image::fromUrl(Url{imageUrl2x}, 36.0 / 256.0, QSize(36, 36)),
            Image::fromUrl(Url{imageUrl4x}, 72.0 / 256.0, QSize(72, 72)),
        },
        .tooltip = Tooltip{QStringLiteral("BChat ") + title.toHtmlEscaped()},
        .homePage = Url{},
        .id = EmoteId{id},
    });
}

}  // namespace

void BChatBadges::initialize()
{
    this->refreshTimer_.setInterval(REFRESH_INTERVAL);

    QObject::connect(
        &this->refreshTimer_,
        &QTimer::timeout,
        this,
        [this] {
            this->requestBadges();
        });

    this->refreshTimer_.start();

    // Load all badges when the application starts.
    this->requestBadges();
}

std::vector<std::shared_ptr<const Emote>> BChatBadges::getBadges(
    const UserId &userID)
{
    const auto id = userID.string;

    if (!validUserId(id))
    {
        return {};
    }

    std::shared_lock lock(this->mutex_);

    const auto found = this->users_.constFind(id);

    if (found == this->users_.cend())
    {
        return {};
    }

    return found.value();
}

bool BChatBadges::applyPayload(const QByteArray &payload)
{
    if (payload.isEmpty() || payload.size() > MAX_PAYLOAD_SIZE)
    {
        return false;
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(
        payload, &parseError);

    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject())
    {
        return false;
    }

    const auto root = document.object();
    const auto badgesObject = root.value("badges").toObject();

    if (!root.value("badges").isObject())
    {
        return false;
    }

    QHash<QString, std::vector<std::shared_ptr<const Emote>>> users;

    for (auto it = badgesObject.constBegin();
         it != badgesObject.constEnd(); ++it)
    {
        const auto badgeID = it.key();
        const auto badgeObject = it.value().toObject();

        if (badgeObject.isEmpty())
        {
            continue;
        }

        const auto badge = makeBadge(badgeID, badgeObject);

        if (!badge)
        {
            qCWarning(chatterinoApp)
                << "[BChat] Invalid badge:" << badgeID;
            continue;
        }

        const auto userValue = badgeObject.value("users");

        if (!userValue.isArray())
        {
            continue;
        }

        const auto userArray = userValue.toArray();

        for (const auto &userValue : userArray)
        {
            const auto userID = userValue.toString();

            if (!validUserId(userID))
            {
                continue;
            }

            users[userID].push_back(badge);
        }
    }

    {
        std::unique_lock lock(this->mutex_);
        this->users_.swap(users);
    }

    return true;
}

void BChatBadges::requestBadges()
{
    {
        std::unique_lock lock(this->mutex_);

        if (this->requestInFlight_)
        {
            return;
        }

        this->requestInFlight_ = true;
    }

    NetworkRequest(BADGES_URL)
        .header("Accept", "application/json")
        .maximumResponseSize(MAX_PAYLOAD_SIZE)
        .timeout(8000)
        .caller(this)
        .onSuccess([this](const NetworkResult &result) {
            if (!this->applyPayload(result.getData()))
            {
                qCWarning(chatterinoApp)
                    << "[BChat] Invalid badge API response";
                return;
            }

            this->badgesUpdated.invoke();
        })
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[BChat] Badge request failed:"
                << result.formatError();
        })
        .finally([this] {
            std::unique_lock lock(this->mutex_);
            this->requestInFlight_ = false;
        })
        .execute();
}

}  // namespace chatterino