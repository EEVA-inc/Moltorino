
// SPDX-FileCopyrightText: 2026 Contributors to Chatterino
//
// SPDX-License-Identifier: MIT

#include "singletons/UrlShortener.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/MessageBuilder.hpp"
#include "singletons/Settings.hpp"
#include "widgets/helper/ResizingTextEdit.hpp"
#include "common/Channel.hpp"

#include <QTextCursor>

namespace chatterino {

namespace {

QString getResponseValue(const NetworkResult &result, QString pattern)
{
    QRegularExpression regExp(
        "{(.+)}", QRegularExpression::InvertedGreedinessOption);

    auto match = regExp.match(pattern);
    auto jsonRoot = result.parseJsonValue();

    while (match.hasMatch())
    {
        QString value;
        auto jsonValue = jsonRoot;

        for (auto key : match.capturedView(1).tokenize(u'.'))
        {
            if (jsonValue.isObject())
            {
                jsonValue = jsonValue[key];
            }
            else if (jsonValue.isArray())
            {
                bool ok = false;
                auto index = key.toLongLong(&ok);

                if (ok)
                {
                    jsonValue = jsonValue[index];
                }
            }
            else
            {
                break;
            }
        }

        value = jsonValue.toString();

        pattern.replace(match.captured(0), value);
        match = regExp.match(pattern);
    }

    return pattern;
}

} // namespace

void UrlShortener::shorten(const QString &url, ChannelPtr channel,
                           QPointer<ResizingTextEdit> textEdit,
                           QTextCursor cursor)
{
    this->sendShortenRequest(url, std::move(channel), 
                             std::move(textEdit), cursor);
}

void UrlShortener::sendShortenRequest(
    const QString &url, ChannelPtr channel,
    QPointer<ResizingTextEdit> textEdit, QTextCursor cursor)
{
    QUrl requestUrl(getSettings()->urlShortenerUrl.getValue());

    if (!requestUrl.isValid() || requestUrl.isEmpty())
    {
        channel->addSystemMessage("Invalid URL shortener API URL.");
        return;
    }

    QJsonObject body;
    body["url"] = url;

    NetworkRequest(requestUrl, NetworkRequestType::Post)
        .header("Content-Type", "application/json")
        .headerList(parseHeaderList(
            getSettings()->urlShortenerHeaders.getValue()))
        .json(body)
        .onSuccess([this, channel, textEdit, cursor, url](NetworkResult result) {
            this->handleSuccessfulShorten(result, channel, textEdit, cursor, url);
        })
        .onError([this, channel](NetworkResult result) -> bool {
            this->handleFailedShorten(result, channel);
            return true;
        })
        .execute();
}

void UrlShortener::handleSuccessfulShorten(
    const NetworkResult &result, ChannelPtr channel,
    QPointer<ResizingTextEdit> textEdit, QTextCursor cursor,
    const QString &url)
{
    if (textEdit == nullptr)
    {
        return;
    }

    QString shortUrl =
        getSettings()->urlShortenerLink.getValue().isEmpty()
            ? result.getData()
            : getResponseValue(
                  result, getSettings()->urlShortenerLink.getValue());

    QString deletionLink =
        getSettings()->urlShortenerDeletionLink.getValue().isEmpty()
            ? ""
            : getResponseValue(
                  result,
                  getSettings()->urlShortenerDeletionLink.getValue());
    
    shortUrl = shortUrl.trimmed();
    
    if (shortUrl.isEmpty())
    {
        channel->addSystemMessage("The URL shortener returned an empty link.");
        return;
    }

    if (cursor.selectedText() != url)
    {
        return;
    }

    cursor.insertText(shortUrl + " ");

    MessageBuilder builder(urlShortenerResultMessage, shortUrl,
                           deletionLink);

    channel->addMessage(builder.release(), MessageContext::Original);
}

void UrlShortener::handleFailedShorten(const NetworkResult &result,
                                       ChannelPtr channel)
{
    channel->addSystemMessage(
        QString("An error happened while shortening your URL: %1")
            .arg(result.formatError()));
}

} // namespace chatterino
