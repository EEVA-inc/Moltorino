// SPDX-FileCopyrightText: 2026 Contributors to Chatterino
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>
#include <QPointer>
#include <QTextCursor>

namespace chatterino
{

class ResizingTextEdit;
class Channel;
class NetworkResult;

using ChannelPtr = std::shared_ptr<Channel>;

class UrlShortener final
{
public:
    void shorten(const QString &url, ChannelPtr channel,
                 QPointer<ResizingTextEdit> textEdit,
                 QTextCursor cursor);

private:
    void sendShortenRequest(const QString &url, ChannelPtr channel,
                            QPointer<ResizingTextEdit> textEdit,
                            QTextCursor cursor);

    void handleSuccessfulShorten(const NetworkResult &result, ChannelPtr channel,
                                 QPointer<ResizingTextEdit> textEdit,
                                 QTextCursor cursor, const QString &url);

    void handleFailedShorten(const NetworkResult &result,
                             ChannelPtr channel);
};
}