#include "providers/wydios/WydiosBadges.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"

#include <QUrlQuery>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <mutex>
#include <shared_mutex>

namespace chatterino {
namespace {

constexpr auto USER_URL = "https://api.wydios.de/user/get";
constexpr qsizetype MAX_PAYLOAD_SIZE = 1024 * 1024;
constexpr qint64 REFRESH_INTERVAL = 30LL * 60 * 1000;

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

    return value.size() <= 2048 && url.isValid() &&
           url.scheme() == QStringLiteral("https") &&
           url.host().compare(
               QStringLiteral("api.wydios.de"),
               Qt::CaseInsensitive) == 0 &&
           url.userInfo().isEmpty() && url.port(-1) == -1 &&
           !url.hasFragment();
}

std::shared_ptr<const Emote> makeBadge(const QJsonObject &object)
{
    const auto id = object.value("id").toVariant().toString();
    const auto slug = object.value("slug").toString();
    const auto name = object.value("name").toString();
    const auto description = object.value("description").toString();
    const auto urls = object.value("urls").toObject();

    const auto imageUrl18 = urls.value("18").toString();
    const auto imageUrl36 = urls.value("36").toString();
    const auto imageUrl72 = urls.value("72").toString();

    if (id.isEmpty() || id.size() > 64 ||
        slug.isEmpty() || slug.size() > 64 ||
        name.isEmpty() || name.size() > 256 ||
        description.size() > 1024 ||
        !validImageUrl(imageUrl18) ||
        !validImageUrl(imageUrl36) ||
        !validImageUrl(imageUrl72))
    {
        return nullptr;
    }

    return std::make_shared<const Emote>(Emote{
        .name = EmoteName{QStringLiteral("wydios:") + slug},
        .images = ImageSet{
            Image::fromUrl(Url{imageUrl18}, 1.0, QSize(18, 18)),
            Image::fromUrl(Url{imageUrl36}, 0.5, QSize(36, 36)),
            Image::fromUrl(Url{imageUrl72}, 0.25, QSize(72, 72)),
        },
        .tooltip = Tooltip{description.toHtmlEscaped()},
        .homePage = Url{},
        .id = EmoteId{id},
    });
}

}  // namespace

void WydiosBadges::initialize()
{
    // Refresh cached user badges periodically.
    this->refreshTimer_.setInterval(
        static_cast<int>(REFRESH_INTERVAL));

    QObject::connect(
        &this->refreshTimer_,
        &QTimer::timeout,
        this,
        [this] {
            std::unique_lock lock(this->mutex_);
            this->fetchedAt_.clear();
        });

    this->refreshTimer_.start();
}

std::vector<std::shared_ptr<const Emote>> WydiosBadges::getBadges(
    const UserId &userID)
{
    const auto id = userID.string;

    if (!validUserId(id))
    {
        return {};
    }

    std::vector<std::shared_ptr<const Emote>> result;
    bool needsRequest = false;

    {
        std::shared_lock lock(this->mutex_);

        const auto found = this->users_.constFind(id);
        if (found != this->users_.cend())
        {
            result = found.value();
        }

        const auto fetched = this->fetchedAt_.constFind(id);
        needsRequest =
            fetched == this->fetchedAt_.cend() ||
            QDateTime::currentMSecsSinceEpoch() - fetched.value() >=
                REFRESH_INTERVAL;
    }

    if (needsRequest)
    {
        this->requestUserBadge(id);
    }

    return result;
}

bool WydiosBadges::applyPayload(
    const QString &userID,
    const QByteArray &payload)
{
    if (payload.isEmpty() || payload.size() > MAX_PAYLOAD_SIZE)
    {
        return false;
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(payload, &parseError);

    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject())
    {
        return false;
    }

    const auto root = document.object();

    if (root.value("error").toBool())
    {
        const auto data = root.value("data").toObject();
        const auto code = data.value("code").toString();

        // A Twitch user without a Wydios account is a valid
        // negative result. Cache it to avoid repeated requests.
        if (code == QStringLiteral("USER_NOT_FOUND"))
        {
            std::unique_lock lock(this->mutex_);
            this->users_.insert(userID, {});
            this->fetchedAt_.insert(
                userID, QDateTime::currentMSecsSinceEpoch());
            return true;
        }

        return false;
    }

    const auto data = root.value("data").toObject();
    const auto user = data.value("user").toObject();

    if (user.isEmpty())
    {
        return false;
    }

    std::vector<std::shared_ptr<const Emote>> badges;

    const auto badgeValue = user.value("badge");

    if (badgeValue.isObject())
    {
        const auto badge = makeBadge(badgeValue.toObject());

        if (badge)
        {
            badges.push_back(badge);
        }
    }
    else if (!badgeValue.isNull() && !badgeValue.isUndefined())
    {
        return false;
    }

    {
        std::unique_lock lock(this->mutex_);

        this->users_.insert(userID, std::move(badges));
        this->fetchedAt_.insert(
            userID, QDateTime::currentMSecsSinceEpoch());
    }

    return true;
}

void WydiosBadges::requestUserBadge(const QString &userID)
{
    {
        std::unique_lock lock(this->mutex_);

        if (this->requestsInFlight_.contains(userID))
        {
            return;
        }

        this->requestsInFlight_.insert(userID);
    }

    QUrl url(QString::fromLatin1(USER_URL));

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("platform"), QStringLiteral("twitch"));
    query.addQueryItem(QStringLiteral("platform_id"), userID);
    url.setQuery(query);

    NetworkRequest(url.toString())
        .header("Accept", "application/json")
        .maximumResponseSize(MAX_PAYLOAD_SIZE)
        .timeout(8000)
        .caller(this)
        .onSuccess([this, userID](const NetworkResult &result) {
            if (!this->applyPayload(userID, result.getData()))
            {
                qCWarning(chatterinoApp)
                    << "[Wydios] Invalid user badge response for Twitch ID:"
                    << userID;
                return;
            }

            this->badgesUpdated.invoke();
        })
        .onError([userID](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[Wydios] User badge request failed for Twitch ID:"
                << userID << result.formatError();
        })
        .finally([this, userID] {
            std::unique_lock lock(this->mutex_);
            this->requestsInFlight_.remove(userID);
        })
        .execute();
}

}  // namespace chatterino