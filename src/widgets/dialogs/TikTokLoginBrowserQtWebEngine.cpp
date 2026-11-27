#include "TikTokLoginBrowser.hpp"

#include <QJsonValue>
#include <QMap>
#include <QPointer>
#include <QRegularExpression>
#include <QWebEngineCookieStore>
#include <QWebEngineDownloadRequest>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <QWidget>

#include <tuple>
#include <utility>

namespace chatterino {

namespace {

bool isTikTokDomain(const QString &domain)
{
    const auto host = domain.startsWith(u'.') ? domain.mid(1) : domain;
    return host == u"tiktok.com" || host.endsWith(u".tiktok.com");
}

class LoginPage final : public QWebEnginePage
{
public:
    using QWebEnginePage::QWebEnginePage;

protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType,
                                 bool isMainFrame) override
    {
        return !isMainFrame || TikTokLoginBrowser::allowedNavigation(url);
    }

    QWebEnginePage *createWindow(WebWindowType) override
    {
        return nullptr;
    }

    QStringList chooseFiles(FileSelectionMode, const QStringList &,
                            const QStringList &) override
    {
        return {};
    }
};

class QtWebEngineLoginBrowser final : public TikTokLoginBrowser
{
public:
    using TikTokLoginBrowser::TikTokLoginBrowser;
    ~QtWebEngineLoginBrowser() override
    {
        this->close();
    }

    void start(QWidget *surface, Callbacks callbacks) override
    {
        this->close();
        this->closed_ = false;
        this->surface_ = surface;
        this->callbacks_ = std::move(callbacks);

        // no storage name, so nothing is written to disk
        this->profile_ = new QWebEngineProfile(this);
        this->profile_->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
        this->profile_->setPersistentCookiesPolicy(
            QWebEngineProfile::NoPersistentCookies);
        // look like plain Chrome, like the Windows and macOS login windows
        // look like Edge and Safari
        this->profile_->setHttpUserAgent(this->profile_->httpUserAgent().remove(
            QRegularExpression(QStringLiteral(R"(QtWebEngine/\S+ )"))));
        QObject::connect(this->profile_, &QWebEngineProfile::downloadRequested,
                         this, [](QWebEngineDownloadRequest *download) {
                             download->cancel();
                         });

        auto *store = this->profile_->cookieStore();
        QObject::connect(store, &QWebEngineCookieStore::cookieAdded, this,
                         [this](const QNetworkCookie &cookie) {
                             if (isTikTokDomain(cookie.domain()) &&
                                 this->cookies_.size() < 512)
                             {
                                 this->cookies_.insert(key(cookie), cookie);
                             }
                         });
        QObject::connect(store, &QWebEngineCookieStore::cookieRemoved, this,
                         [this](const QNetworkCookie &cookie) {
                             this->cookies_.remove(key(cookie));
                         });
        store->loadAllCookies();

        auto *page = new LoginPage(this->profile_, this->profile_);
        page->settings()->setAttribute(
            QWebEngineSettings::JavascriptCanOpenWindows, false);
        page->settings()->setAttribute(
            QWebEngineSettings::PlaybackRequiresUserGesture, true);
        QObject::connect(page, &QWebEnginePage::permissionRequested, page,
                         [](QWebEnginePermission permission) {
                             permission.deny();
                         });
        QObject::connect(
            page, &QWebEnginePage::renderProcessTerminated, this,
            [this](QWebEnginePage::RenderProcessTerminationStatus status, int) {
                if (status == QWebEnginePage::NormalTerminationStatus)
                {
                    return;
                }
                const auto error = this->callbacks_.error;
                if (!this->closed_ && error)
                {
                    error(QStringLiteral("TikTok login stopped responding. "
                                         "Close this window and try again."));
                }
            });

        this->view_ = new QWebEngineView(surface);
        this->view_->setPage(page);
        this->view_->setContextMenuPolicy(Qt::NoContextMenu);
        this->view_->hide();
        this->resize();

        const auto ready = this->callbacks_.ready;
        if (ready)
        {
            ready();
        }
    }

    void navigate(const QUrl &url) override
    {
        if (!this->closed_ && this->view_ && allowedNavigation(url))
        {
            this->view_->load(url);
        }
    }

    void evaluate(const QString &script, ScriptCallback callback) override
    {
        if (this->closed_ || !this->view_)
        {
            callback({});
            return;
        }
        QPointer<QtWebEngineLoginBrowser> weak(this);
        this->view_->page()->runJavaScript(
            script, [weak, callback = std::move(callback)](const QVariant &v) {
                if (!weak || weak->closed_)
                {
                    return;
                }
                callback(QJsonValue::fromVariant(v).toObject());
            });
    }

    void cookies(CookiesCallback callback) override
    {
        if (this->closed_ || !this->view_)
        {
            callback({});
            return;
        }
        callback(this->cookies_.values());
    }

    void setVisible(bool visible) override
    {
        if (this->view_)
        {
            this->view_->setVisible(visible);
        }
    }

    void resize() override
    {
        if (this->view_ && this->surface_)
        {
            this->view_->setGeometry(this->surface_->rect());
        }
    }

    void close() override
    {
        if (std::exchange(this->closed_, true))
        {
            return;
        }
        this->callbacks_ = {};
        this->cookies_.clear();
        if (this->view_)
        {
            this->view_->stop();
            this->view_->deleteLater();
            this->view_ = nullptr;
        }
        if (this->profile_)
        {
            this->profile_->deleteLater();
            this->profile_ = nullptr;
        }
    }

private:
    using CookieKey = std::tuple<QByteArray, QString, QString>;

    static CookieKey key(const QNetworkCookie &cookie)
    {
        return {cookie.name(), cookie.domain(), cookie.path()};
    }

    QPointer<QWidget> surface_;
    QPointer<QWebEngineView> view_;
    QPointer<QWebEngineProfile> profile_;
    Callbacks callbacks_;
    QMap<CookieKey, QNetworkCookie> cookies_;
    bool closed_ = true;
};

}

std::unique_ptr<TikTokLoginBrowser> createPlatformTikTokLoginBrowser(
    QObject *parent)
{
    return std::make_unique<QtWebEngineLoginBrowser>(parent);
}

}
