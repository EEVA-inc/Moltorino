#include "providers/solrino/SolrinoBadges.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "singletons/Settings.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <algorithm>
#include <mutex>

namespace chatterino {
namespace {

constexpr auto BADGES_URL =
    "https://solrino-badges.solrino-badge-server.workers.dev/v2/badges";
constexpr auto IMAGE_HOST = "solrino.solrino-badge-server.workers.dev";
constexpr qsizetype MAX_PAYLOAD_SIZE = 4 * 1024 * 1024;
constexpr auto REFRESH_INTERVAL = 30 * 60 * 1000;

bool validImageUrl(const QString &value)
{
    const QUrl url(value, QUrl::StrictMode);
    return value.size() <= 2048 && url.isValid() &&
           url.scheme() == QStringLiteral("https") &&
           url.host() == QLatin1String(IMAGE_HOST) &&
           url.userInfo().isEmpty() && url.port(-1) == -1 &&
           !url.hasFragment() &&
           url.path().startsWith(QStringLiteral("/assets/badges/"));
}

bool validUserId(const QString &id)
{
    return !id.isEmpty() && id.size() <= 20 && id.front() != QLatin1Char('0') &&
           std::ranges::all_of(id, [](QChar c) {
               return c >= QLatin1Char('0') && c <= QLatin1Char('9');
           });
}

std::shared_ptr<const Emote> makeBadge(const QJsonObject &object)
{
    const auto id = object.value("id").toString();
    const auto tooltip = object.value("tooltip").toString();
    const auto images = object.value("images").toObject();
    const auto image1x = images.value("1x").toString();
    const auto image2x = images.value("2x").toString();
    const auto image3x = images.value("3x").toString();
    if (id.isEmpty() || id.size() > 64 || tooltip.isEmpty() ||
        tooltip.size() > 256 || !validImageUrl(image1x) ||
        !validImageUrl(image2x) || !validImageUrl(image3x))
    {
        return nullptr;
    }

    // Solrino's images are 18, 36 and 72 pixels square
    return std::make_shared<const Emote>(Emote{
        .name = EmoteName{QStringLiteral("solrino:") + id},
        .images =
            ImageSet{
                Image::fromUrl(Url{image1x}, 1.0, QSize(18, 18)),
                Image::fromUrl(Url{image2x}, 0.5, QSize(36, 36)),
                Image::fromUrl(Url{image3x}, 0.25, QSize(72, 72)),
            },
        .tooltip = Tooltip{tooltip.toHtmlEscaped()},
        .homePage = Url{},
        .id = EmoteId{image1x},
    });
}

}  // namespace

void SolrinoBadges::initialize()
{
    this->refreshTimer_.setInterval(REFRESH_INTERVAL);
    QObject::connect(&this->refreshTimer_, &QTimer::timeout, this, [this] {
        this->requestBadges();
    });
    this->refreshTimer_.start();

    // Nothing is requested from Solrino while its badges are turned off.
    getSettings()->showBadgesSolrino.connect(
        [this](bool enabled) {
            if (enabled)
            {
                QTimer::singleShot(3500, this, [this] {
                    this->requestBadges();
                });
            }
        },
        this->signalHolder_);
}

std::vector<std::shared_ptr<const Emote>> SolrinoBadges::getBadges(
    const UserId &userID) const
{
    std::shared_lock lock(this->mutex_);
    const auto found = this->users_.find(userID.string);
    if (found == this->users_.end())
    {
        return {};
    }
    return {found->second};
}

bool SolrinoBadges::applyPayload(const QByteArray &payload)
{
    if (payload.isEmpty() || payload.size() > MAX_PAYLOAD_SIZE)
    {
        return false;
    }
    const auto document = QJsonDocument::fromJson(payload);
    const auto root = document.object();
    if (!document.isObject() || root.value("schemaVersion").toInt() != 2 ||
        !root.value("badges").isArray() || !root.value("users").isObject())
    {
        return false;
    }

    boost::unordered_flat_map<QString, std::shared_ptr<const Emote>> catalog;
    for (const auto &value : root.value("badges").toArray())
    {
        const auto object = value.toObject();
        if (auto badge = makeBadge(object))
        {
            catalog.emplace(object.value("id").toString(), std::move(badge));
        }
        if (catalog.size() > 256)
        {
            return false;
        }
    }

    const auto entries = root.value("users").toObject();
    if (entries.size() > 200000)
    {
        return false;
    }
    boost::unordered_flat_map<QString, std::shared_ptr<const Emote>> users;
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it)
    {
        if (!validUserId(it.key()))
        {
            continue;
        }
        const auto user = it.value().toObject();
        // Someone who picked a badge in Solrino gets that one, and someone who
        // picked none gets none. Otherwise it's their first badge, like Solrino.
        QString chosen;
        if (user.contains("activeBadge"))
        {
            chosen = user.value("activeBadge").toString();
        }
        else
        {
            chosen = user.value("badges").toArray().first().toString();
        }
        const auto badge = catalog.find(chosen);
        if (badge != catalog.end())
        {
            users.emplace(it.key(), badge->second);
        }
    }

    std::unique_lock lock(this->mutex_);
    this->users_ = std::move(users);
    return true;
}

void SolrinoBadges::requestBadges()
{
    if (this->requestInFlight_ || !getSettings()->showBadgesSolrino)
    {
        return;
    }
    this->requestInFlight_ = true;
    NetworkRequest(BADGES_URL)
        .header("Accept", "application/json")
        .maximumResponseSize(MAX_PAYLOAD_SIZE)
        .timeout(8000)
        .caller(this)
        .onSuccess([this](const NetworkResult &result) {
            const auto payload = result.getData();
            if (payload == this->lastPayload_)
            {
                return;
            }
            if (!this->applyPayload(payload))
            {
                qCWarning(chatterinoApp)
                    << "[Solrino] Ignoring an invalid badge list.";
                return;
            }
            this->lastPayload_ = payload;
            this->badgesUpdated.invoke();
        })
        .onError([](const NetworkResult &result) {
            qCWarning(chatterinoApp)
                << "[Solrino] Badge request failed:" << result.formatError();
        })
        .finally([this] {
            this->requestInFlight_ = false;
        })
        .execute();
}

}  // namespace chatterino
