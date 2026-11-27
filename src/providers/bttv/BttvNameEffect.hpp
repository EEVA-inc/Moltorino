#pragma once

#include "messages/Image.hpp"
#include "providers/seventv/paints/Paint.hpp"

#include <QColor>
#include <QPixmap>
#include <QString>

#include <memory>
#include <optional>
#include <vector>

namespace chatterino {

/// A BetterTTV Pro username effect, drawn the same way as a 7TV paint
class BttvNameEffect : public Paint
{
public:
    enum class Kind {
        /// The user's color with a soft glow around it
        Glow,
        /// The user's color with a light streak sweeping across it
        Flare,
        /// A scrolling image texture with a dark outline
        Texture,
    };

    /// The shared effect for an effect name sent by BetterTTV, or nullptr if
    /// the name is unknown. Only call this from the GUI thread.
    static std::shared_ptr<Paint> fromName(const QString &name);

    BttvNameEffect(const QString &effect, Kind kind, ImagePtr texture,
                   QColor outline);

    QBrush asBrush(QColor userColor, QRectF drawingRect) const override;
    const std::vector<PaintDropShadow> &getDropShadows() const override;
    bool animated() const override;
    bool loaded() const override;
    bool failed() const override;
    void ensureLoaded(bool retry = false) const override;

protected:
    std::vector<PaintDropShadow> dropShadowsFor(
        QColor userColor) const override;

private:
    qint64 sourcePixmapCacheKey() const override;

    std::optional<QPixmap> texturePixmap() const;
    QBrush flareBrush(QColor userColor, QRectF drawingRect) const;
    QBrush textureBrush(QColor userColor, QRectF drawingRect) const;

    const Kind kind_;
    const ImagePtr texture_;
    // Kept once loaded so the image cache can't drop it while a 7TV paint is
    // shown instead, which would make switching to the effect wait on the CDN
    mutable QPixmap texturePixmap_;
    const std::vector<PaintDropShadow> dropShadows_;
};

}  // namespace chatterino
