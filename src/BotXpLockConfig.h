#ifndef BOT_XP_LOCK_CONFIG_H
#define BOT_XP_LOCK_CONFIG_H

#include "Common.h"

class BotXpLockConfig
{
public:
    static BotXpLockConfig& Instance()
    {
        static BotXpLockConfig instance;
        return instance;
    }

    void Load();

    bool IsEnabled() const { return m_enabled; }
    bool IsPartyOnly() const { return m_partyOnly; }
    bool IsAllowCustomCap() const { return m_allowCustomCap; }
    bool IsClampAltSyncDing() const { return m_clampAltSyncDing; }
    bool IsNotifyOnLock() const { return m_notifyOnLock; }

private:
    BotXpLockConfig() = default;

    bool m_enabled = true;
    bool m_partyOnly = false;
    bool m_allowCustomCap = true;
    bool m_clampAltSyncDing = true;
    bool m_notifyOnLock = true;
};

#define sBotXpLockConfig BotXpLockConfig::Instance()

#endif // BOT_XP_LOCK_CONFIG_H
