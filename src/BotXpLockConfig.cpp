#include "BotXpLockConfig.h"
#include "Config/Config.h"
#include "Log.h"

void BotXpLockConfig::Load()
{
    m_enabled = sConfig.GetBoolDefault("BotXpLock.Enable", true);
    m_partyOnly = sConfig.GetBoolDefault("BotXpLock.PartyOnly", false);
    m_allowCustomCap = sConfig.GetBoolDefault("BotXpLock.AllowCustomCap", true);
    m_clampAltSyncDing = sConfig.GetBoolDefault("BotXpLock.ClampAltSyncDing", true);
    m_notifyOnLock = sConfig.GetBoolDefault("BotXpLock.NotifyOnLock", true);
    m_debug = sConfig.GetBoolDefault("BotXpLock.Debug", true);

    sLog.outString("[BotXpLock] Configuration loaded: Enable=%u, PartyOnly=%u, AllowCustomCap=%u, ClampAltSyncDing=%u, Debug=%u",
        m_enabled ? 1 : 0, m_partyOnly ? 1 : 0, m_allowCustomCap ? 1 : 0, m_clampAltSyncDing ? 1 : 0, m_debug ? 1 : 0);
}
