#include "providers/bttv/BttvNameEffect.hpp"

#include "singletons/Settings.hpp"

#include <QDateTime>
#include <QLinearGradient>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace chatterino {

using namespace Qt::Literals::StringLiterals;

namespace {

// Timings and sizes follow BetterTTV's own username effect styles
constexpr qint64 FLARE_PERIOD_MS = 8000;
constexpr qreal FLARE_ANGLE_DEGREES = 75;
constexpr qint64 TEXTURE_PERIOD_MS = 16000;
// BetterTTV tiles its 96px textures behind a name about 20px tall
constexpr qreal TEXTURE_TILE_PER_HEIGHT = 96.0 / 20.0;
// Animated effects only need a new cache key this often
constexpr qint64 FRAME_MS = 40;

constexpr float GLOW_RADIUS = 6;

struct TextureEffect {
    QString name;
    // a near black tint of the texture, used for its outline
    QColor outline;
};

const std::vector<TextureEffect> &textureEffects()
{
    static const std::vector<TextureEffect> effects{
        {u"iridescence"_s, QColor(0x2f395f)},
        {u"supernova"_s, QColor(0x702838)},
        {u"glacier"_s, QColor(0x166258)},
        {u"intergalactic"_s, QColor(0x4e316c)},
        {u"midas"_s, QColor(0x674911)},
        {u"peach"_s, QColor(0x5c2a14)},
        {u"abyss"_s, QColor(0x14247a)},
        {u"phoenix"_s, QColor(0x52000e)},
        {u"arcane"_s, QColor(0x2c1e84)},
        {u"evergreen"_s, QColor(0x0e4a1e)},
        {u"sakura"_s, QColor(0x8c2a63)},
        {u"onyx"_s, QColor(0x050506)},
        {u"crimson"_s, QColor(0x3a0610)},
    };
    return effects;
}

QString displayName(const QString &effect)
{
    if (effect.isEmpty())
    {
        return effect;
    }
    return u"BTTV "_s + effect.front().toUpper() + effect.mid(1);
}

std::vector<PaintDropShadow> makeDropShadows(BttvNameEffect::Kind kind,
                                             QColor outline)
{
    switch (kind)
    {
        case BttvNameEffect::Kind::Glow:
            // the color is replaced with the user's color when drawn
            return {PaintDropShadow(0, 0, GLOW_RADIUS, Qt::transparent)};
        case BttvNameEffect::Kind::Texture:
            // a thin outline, then a copy of it offset down so the bottom
            // edge reads as a drop shadow
            return {
                PaintDropShadow(0, 0, 0.35F, outline),
                PaintDropShadow(0, 1, 0.35F, outline),
            };
        case BttvNameEffect::Kind::Flare:
            break;
    }
    return {};
}

qint64 elapsedMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

}  // namespace

std::shared_ptr<Paint> BttvNameEffect::fromName(const QString &name)
{
    static std::unordered_map<QString, std::shared_ptr<Paint>> effects = [] {
        std::unordered_map<QString, std::shared_ptr<Paint>> map;
        map.emplace(u"glow"_s, std::make_shared<BttvNameEffect>(
                                   u"glow"_s, Kind::Glow, nullptr, QColor()));
        map.emplace(u"flare"_s,
                    std::make_shared<BttvNameEffect>(u"flare"_s, Kind::Flare,
                                                     nullptr, QColor()));
        for (const auto &effect : textureEffects())
        {
            auto texture = Image::fromUrl(
                Url{u"https://cdn.betterttv.net/assets/username_effects/"_s +
                    effect.name + u".png"_s});
            map.emplace(effect.name, std::make_shared<BttvNameEffect>(
                                         effect.name, Kind::Texture,
                                         std::move(texture), effect.outline));
        }
        return map;
    }();

    const auto it = effects.find(name);
    return it == effects.end() ? nullptr : it->second;
}

BttvNameEffect::BttvNameEffect(const QString &effect, Kind kind,
                               ImagePtr texture, QColor outline)
    : Paint(displayName(effect), u"bttv:"_s + effect)
    , kind_(kind)
    , texture_(std::move(texture))
    , dropShadows_(makeDropShadows(kind, outline))
{
}

QBrush BttvNameEffect::asBrush(QColor userColor, QRectF drawingRect) const
{
    switch (this->kind_)
    {
        case Kind::Flare:
            return this->flareBrush(userColor, drawingRect);
        case Kind::Texture:
            return this->textureBrush(userColor, drawingRect);
        case Kind::Glow:
            break;
    }
    return {userColor};
}

QBrush BttvNameEffect::flareBrush(QColor userColor, QRectF drawingRect) const
{
    const auto width = drawingRect.width();
    const auto height = drawingRect.height();
    if (width <= 0 || height <= 0)
    {
        return {userColor};
    }

    // The streak sits on a layer twice as wide as the name. It crosses the
    // name during the first half of each period and waits off to the side
    // for the second half.
    qreal progress = 0;
    if (this->animated())
    {
        progress = std::min<qreal>(
            1, static_cast<qreal>(elapsedMs() % FLARE_PERIOD_MS) /
                   static_cast<qreal>(FLARE_PERIOD_MS / 2));
    }
    const auto layerLeft = drawingRect.left() + ((-2 + (4 * progress)) * width);
    const QPointF center(layerLeft + width, drawingRect.center().y());

    const auto angle = qDegreesToRadians(FLARE_ANGLE_DEGREES);
    const QPointF direction(std::sin(angle), -std::cos(angle));
    const auto length = std::abs(2 * width * std::sin(angle)) +
                        std::abs(height * std::cos(angle));

    // 20% of the user's color mixed into white
    const QColor streak(qRound((userColor.red() * 0.2) + (255 * 0.8)),
                        qRound((userColor.green() * 0.2) + (255 * 0.8)),
                        qRound((userColor.blue() * 0.2) + (255 * 0.8)));

    QLinearGradient gradient(center - (direction * length / 2),
                             center + (direction * length / 2));
    gradient.setColorAt(0, userColor);
    gradient.setColorAt(0.4, userColor);
    gradient.setColorAt(0.5, streak);
    gradient.setColorAt(0.6, userColor);
    gradient.setColorAt(1, userColor);
    return {gradient};
}

std::optional<QPixmap> BttvNameEffect::texturePixmap() const
{
    if (this->texturePixmap_.isNull() && this->texture_)
    {
        if (auto pixmap = this->texture_->pixmapOrLoad())
        {
            this->texturePixmap_ = *pixmap;
        }
    }
    if (this->texturePixmap_.isNull())
    {
        return std::nullopt;
    }
    return this->texturePixmap_;
}

QBrush BttvNameEffect::textureBrush(QColor userColor, QRectF drawingRect) const
{
    const auto pixmap = this->texturePixmap();
    if (!pixmap || pixmap->isNull() || drawingRect.height() <= 0)
    {
        return {userColor};
    }

    const auto sourceSize = pixmap->deviceIndependentSize();
    if (sourceSize.isEmpty())
    {
        return {userColor};
    }

    const auto tile = drawingRect.height() * TEXTURE_TILE_PER_HEIGHT;
    qreal offset = 0;
    if (this->animated())
    {
        offset = -tile * static_cast<qreal>(elapsedMs() % TEXTURE_PERIOD_MS) /
                 static_cast<qreal>(TEXTURE_PERIOD_MS);
    }

    QBrush brush(*pixmap);
    QTransform transform;
    transform.translate(drawingRect.left() + offset,
                        drawingRect.top() + offset);
    transform.scale(tile / sourceSize.width(), tile / sourceSize.height());
    brush.setTransform(transform);
    return brush;
}

const std::vector<PaintDropShadow> &BttvNameEffect::getDropShadows() const
{
    return this->dropShadows_;
}

std::vector<PaintDropShadow> BttvNameEffect::dropShadowsFor(
    QColor userColor) const
{
    if (this->kind_ != Kind::Glow)
    {
        return this->dropShadows_;
    }

    auto glowColor = userColor;
    glowColor.setAlphaF(0.8F);
    return {PaintDropShadow(0, 0, GLOW_RADIUS, glowColor)};
}

bool BttvNameEffect::animated() const
{
    return this->kind_ != Kind::Glow && getSettings()->animateEmotes;
}

bool BttvNameEffect::loaded() const
{
    return !this->texture_ || !this->texturePixmap_.isNull() ||
           this->texture_->loaded();
}

bool BttvNameEffect::failed() const
{
    return this->texture_ && this->texture_->isEmpty();
}

void BttvNameEffect::ensureLoaded(bool retry) const
{
    if (!this->texture_)
    {
        return;
    }
    if (retry)
    {
        this->texture_->retryLoad();
    }
    else
    {
        this->texture_->load();
    }
}

qint64 BttvNameEffect::sourcePixmapCacheKey() const
{
    qint64 key = 0;
    if (const auto pixmap = this->texturePixmap())
    {
        key = pixmap->cacheKey();
    }
    if (this->animated())
    {
        key = (key * 31) + (elapsedMs() / FRAME_MS);
    }
    return key;
}

}  // namespace chatterino
