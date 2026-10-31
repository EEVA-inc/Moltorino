#pragma once

#include "util/BadgeRegistry.hpp"

#include <QJsonObject>
#include <QSet>
#include <QTimer>

#include <memory>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

class SeventvBadges : public BadgeRegistry
{
public:
    SeventvBadges();
    void loadBadgeByID(const QString &id);

protected:
    QString idForBadge(const QJsonObject &badgeJson) const override;
    EmotePtr createBadge(const QString &id,
                         const QJsonObject &badgeJson) const override;

private:
    void loadPendingBadges();

    QTimer fetchTimer_;
    QSet<QString> pendingBadges_;
    QSet<QString> deferredBadges_;
    bool loading_ = false;
};

}
