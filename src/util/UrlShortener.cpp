// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/UrlShortener.hpp"

#include "singletons/Settings.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace chatterino::urlshortener::detail {

namespace {

QString parseUrl(const QString &url)
{
    if (url.isEmpty() || url.compare("{response}", Qt::CaseInsensitive) == 0)
    {
        return {};
    }

    static const QRegularExpression tokenRegex(
        R"(\{json:([^{}]*(?:\{[^{}]*\}[^{}]*)*)\}*)",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression arrayRegex(R"(\[(\d+)\])");

    QString out;
    qsizetype last = 0;
    bool changed = false;

    auto it = tokenRegex.globalMatch(url);
    while (it.hasNext())
    {
        auto match = it.next();

        out += url.mid(last, match.capturedStart() - last);

        QString inner = match.captured(1).trimmed();

        qsizetype pipeIndex = inner.lastIndexOf('|');
        if (pipeIndex != -1)
        {
            inner = inner.mid(pipeIndex + 1).trimmed();
        }

        inner.replace(arrayRegex, R"(.\1)");

        out += '{' + inner + '}';
        last = match.capturedEnd();
        changed = true;
    }

    out += url.mid(last);
    return changed ? out : QString(url);
}

QStringList parseHeaders(const QJsonObject &headersObj)
{
    QStringList headerLines;

    for (auto it = headersObj.begin(); it != headersObj.end(); ++it)
    {
        if (it.value().isString())
        {
            headerLines.append(
                QString("%1: %2").arg(it.key(), it.value().toString()));
        }
    }

    return headerLines;
}

}

QJsonObject exportSettings(const Settings &s)
{
    QJsonObject settingsObj;

    settingsObj["Version"] = "1.0.0";
    settingsObj["Name"] = "Chatterino URL Shortener Settings";
    settingsObj["RequestMethod"] = "POST";
    settingsObj["RequestURL"] = s.urlShortenerUrl.getValue();
    settingsObj["URL"] = s.urlShortenerLink.getValue();
    settingsObj["DeletionURL"] = s.urlShortenerDeletionLink.getValue();

    QString headers = s.urlShortenerHeaders.getValue();

    if (!headers.isEmpty())
    {
        QJsonObject headersObj;
        QStringList headerLines = headers.split(';', Qt::SkipEmptyParts);

        for (const QString &line : headerLines)
        {
            QStringList parts = line.split(':', Qt::SkipEmptyParts);

            if (parts.size() >= 2)
            {
                QString key = parts[0].trimmed();
                QString value = parts.mid(1).join(':').trimmed();
                headersObj[key] = value;
            }
        }

        if (!headersObj.isEmpty())
        {
            settingsObj["Headers"] = headersObj;
        }
    }

    return settingsObj;
}

bool importSettings(const QJsonObject &settingsObj, Settings &s)
{
    if (!settingsObj.contains("RequestURL") ||
        !settingsObj["RequestURL"].isString() ||
        !settingsObj.contains("URL") ||
        !settingsObj["URL"].isString())
    {
        return false;
    }

    s.urlShortenerUrl = settingsObj["RequestURL"].toString();
    s.urlShortenerLink = parseUrl(settingsObj["URL"].toString());

    s.urlShortenerDeletionLink =
        settingsObj["DeletionURL"].isString()
            ? parseUrl(settingsObj["DeletionURL"].toString())
            : QString{};

    QStringList headers;

    if (settingsObj.contains("Headers") && settingsObj["Headers"].isObject())
    {
        headers = parseHeaders(settingsObj["Headers"].toObject());
    }

    s.urlShortenerHeaders = headers.join(';');
    s.urlShortenerEnabled = true;

    return true;
}

ExpectedStr<QJsonObject> validateImportJson(const QString &clipboardText)
{
    if (clipboardText.isEmpty())
    {
        return makeUnexpected("Clipboard must not be empty");
    }

    QJsonParseError parseError;
    QJsonDocument doc =
        QJsonDocument::fromJson(clipboardText.toUtf8(), &parseError);

    if (parseError.error != QJsonParseError::NoError)
    {
        return makeUnexpected("Clipboard did not contain valid JSON");
    }

    if (!doc.isObject())
    {
        return makeUnexpected("JSON must be an object");
    }

    auto settingsObj = doc.object();

    if (!settingsObj.contains("Version"))
    {
        return makeUnexpected("JSON must contain the 'Version' key");
    }

    if (!settingsObj.contains("RequestURL"))
    {
        return makeUnexpected("JSON must contain the 'RequestURL' key");
    }

    return settingsObj;
}

}