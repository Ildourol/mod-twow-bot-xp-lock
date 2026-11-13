#ifndef BOT_XP_LOCK_MGR_H
#define BOT_XP_LOCK_MGR_H

#include "Common.h"
#include "BotXpLockCommon.h"
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>

class Player;
class ChatHandler;

class BotXpLockMgr
{
public:
    static BotXpLockMgr& Instance()
    {
        static BotXpLockMgr instance;
        return instance;
    }

    void Initialize();
    void LoadFromDB();

    bool IsXpLocked(Player* player) const;
    float GetBotXpRate(Player* player) const;
    void ModifyXP(Player* player, uint32& amount);

    void OnLevelChanged(Player* player, uint8 oldLevel);
    void OnBotLogin(Player* player);

    bool SetLock(uint32 botGuid, bool locked, uint8 lockLevel, uint32 operatorGuid, std::string* outMsg = nullptr);
    bool SetRate(uint32 botGuid, float rate, uint32 operatorGuid, std::string* outMsg = nullptr);
    bool DeleteLock(uint32 botGuid, uint32 operatorGuid, std::string* outMsg = nullptr);
    bool GetLockEntry(uint32 botGuid, BotXpLock::LockEntry& outEntry) const;
    std::vector<BotXpLock::LockEntry> GetAllLockedBots() const;

    // Query / identification helpers
    std::string GetBotName(uint32 botGuid) const;
    uint8 GetBotLevel(uint32 botGuid) const;
    bool ResolveBot(Player* sender, std::string const& identifier, uint32& outGuid, std::string& outName, uint8& outLevel, Player*& outPlayer) const;

    // Ingress communication handlers
    void HandleAddonMessage(Player* sender, std::string const& payload);
    void HandleCommand(ChatHandler* handler, Player* sender, std::string const& args);

    // Egress communication helpers
    void SendStatusAddonMessage(Player* recipient, uint32 botGuid);
    void SendRosterAddonMessage(Player* recipient);
    void SendAllLockedBotsAddonMessage(Player* recipient);

    // Bot detection helper
    static bool IsPlayerBot(Player const* player);

private:
    BotXpLockMgr() = default;

    mutable std::mutex m_mutex;
    std::unordered_map<uint32, BotXpLock::LockEntry> m_locks; // Key: bot low GUID
};

#define sBotXpLockMgr BotXpLockMgr::Instance()

#endif // BOT_XP_LOCK_MGR_H
