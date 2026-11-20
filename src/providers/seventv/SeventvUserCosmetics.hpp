#pragma once

class QString;

namespace chatterino {

/// 7TV only sends a user's paint and badge while their 7TV client reports them
/// as active, so chatters from loaded history or from before a restart stay
/// plain until that happens again. This looks those users up once per session,
/// in batches, and assigns what 7TV returns. Safe to call from any thread.
void lookUpSeventvCosmetics(const QString &twitchID, const QString &login);

}  // namespace chatterino
