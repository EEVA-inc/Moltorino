// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/ImageSet.hpp"

#include <QJsonObject>

namespace chatterino {

struct ChannelPointReward {
    ChannelPointReward(const QJsonObject &redemption);
    ChannelPointReward() = delete;
    QString id;
    QString redemptionKey;
    QString channelId;
    QString title;
    int cost;
    ImageSet image;
    bool isUserInputRequired = false;
    /// What the user typed, for rewards that ask for text
    QString userInput;
    bool isBits = false;
    QString emoteId;    // currently only for celebrations
    QString emoteName;  // currently only for celebrations

    struct User {
        QString id;
        QString login;
        QString displayName;
    };
    User user;
    /// Who redeemed it, also for rewards with text, where `user` stays empty
    /// because chat takes the user from the IRC message
    User redeemedBy;

    QJsonObject toJson() const;
};

}  // namespace chatterino
