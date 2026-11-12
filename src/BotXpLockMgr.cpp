#include "BotXpLockMgr.h"
#include "BotXpLockConfig.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "Chat/Chat.h"
#include "Database/DatabaseEnv.h"
#include "WorldSession.h"
#include "Group.h"
#include "Log.h"

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cstdlib>

#if defined(HAVE_TORTOISEBOTS)
#include "PlayerbotAIStorage.h"
#endif

void BotXpLockMgr::Initialize()
{
    // Ensure table exists in tw_char
    CharacterDatabase.Execute(
        "CREATE TABLE IF NOT EXISTS `bot_xp_locks` ("
        "  `guid` INT UNSIGNED NOT NULL COMMENT 'Character low GUID of the bot',"
        "  `locked` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '1 = locked, 0 = unlocked',"
        "  `lock_level` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = lock at current level; >0 = cap at this level',"
        "  `locked_at_level` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Level when lock was applied',"
        "  `xp_rate` FLOAT NOT NULL DEFAULT 1.0 COMMENT 'Individual bot XP rate multiplier (e.g. 0.5, 1.0, 2.0)',"
        "  `locked_by_guid` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Player low GUID who applied the lock',"
        "  `locked_at` DATETIME NULL COMMENT 'Timestamp when lock was last modified',"
        "  PRIMARY KEY (`guid`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Turtle WoW Bot XP Lock & Rate State Registry';"
    );

    // Idempotent migration for existing tables
    CharacterDatabase.Execute(
        "ALTER TABLE `bot_xp_locks` ADD COLUMN IF NOT EXISTS `xp_rate` FLOAT NOT NULL DEFAULT 1.0 AFTER `locked_at_level`;"
    );
}

void BotXpLockMgr::LoadFromDB()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_locks.clear();

    std::unique_ptr<QueryResult> result(CharacterDatabase.Query(
        "SELECT guid, locked, lock_level, locked_at_level, xp_rate, locked_by_guid FROM `bot_xp_locks`"
    ));

    if (!result)
    {
        sLog.outString("[BotXpLock] Loaded 0 bot XP locks/rates from database.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();
        uint32 guid = fields[0].GetUInt32();

        BotXpLock::LockEntry entry;
        entry.guid = guid;
        entry.locked = fields[1].GetBool();
        entry.lockLevel = fields[2].GetUInt8();
        entry.lockedAtLevel = fields[3].GetUInt8();
        entry.xpRate = fields[4].GetFloat();
        entry.lockedByGuid = fields[5].GetUInt32();

        m_locks[guid] = entry;
        ++count;
    } while (result->NextRow());

    sLog.outString("[BotXpLock] Successfully loaded %u bot XP lock/rate record(s) from database.", count);
}

bool BotXpLockMgr::IsPlayerBot(Player const* player)
{
    if (!player)
        return false;

    // Headless WorldSession is the authoritative core-native seam for all bots
    if (player->GetSession() && player->GetSession()->IsHeadless())
        return true;

#if defined(HAVE_TORTOISEBOTS)
    if (PlayerbotAIStorage::Instance().GetAI(const_cast<Player*>(player)) != nullptr)
        return true;
#endif

    return false;
}

bool BotXpLockMgr::IsXpLocked(Player* player) const
{
    if (!sBotXpLockConfig.IsEnabled() || !player)
        return false;

    uint32 lowGuid = player->GetGUIDLow();
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_locks.find(lowGuid);
    if (it == m_locks.end())
        return false;

    if (it->second.locked)
    {
        // LockLevel 0: locked at whatever current level the bot is
        if (it->second.lockLevel == 0)
            return true;

        // Custom level cap: locked once bot level is at or above the target cap
        if (player->GetLevel() >= it->second.lockLevel)
            return true;
    }

    // If rate is set to 0.0x, it is also treated as locked
    if (it->second.xpRate <= 0.0f)
        return true;

    return false;
}

float BotXpLockMgr::GetBotXpRate(Player* player) const
{
    if (!sBotXpLockConfig.IsEnabled() || !player)
        return 1.0f;

    uint32 lowGuid = player->GetGUIDLow();
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_locks.find(lowGuid);
    if (it == m_locks.end())
        return 1.0f;

    if (it->second.locked)
    {
        if (it->second.lockLevel == 0 || player->GetLevel() >= it->second.lockLevel)
            return 0.0f;
    }

    return it->second.xpRate;
}

void BotXpLockMgr::ModifyXP(Player* player, uint32& amount)
{
    if (amount < 1 || !player)
        return;

    if (IsXpLocked(player))
    {
        amount = 0;
        return;
    }

    float rate = GetBotXpRate(player);
    if (rate <= 0.0f)
    {
        amount = 0;
        return;
    }

    if (rate != 1.0f)
    {
        uint32 scaled = static_cast<uint32>(amount * rate);
        if (scaled == 0 && amount > 0 && rate > 0.0f)
            scaled = 1;
        amount = scaled;
    }
}

void BotXpLockMgr::OnLevelChanged(Player* player, uint8 oldLevel)
{
    if (!sBotXpLockConfig.IsEnabled() || !player)
        return;

    uint32 lowGuid = player->GetGUIDLow();
    BotXpLock::LockEntry entry;
    bool hasEntry = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_locks.find(lowGuid);
        if (it != m_locks.end() && it->second.locked)
        {
            entry = it->second;
            hasEntry = true;
        }
    }

    if (!hasEntry)
        return;

    uint8 currentLevel = player->GetLevel();

    // 1. Anti-Sync Ding Clamp: Prevent external alt level sync from exceeding target level
    uint8 targetCap = entry.lockLevel > 0 ? entry.lockLevel : entry.lockedAtLevel;
    if (sBotXpLockConfig.IsClampAltSyncDing() && targetCap > 0 && currentLevel > targetCap)
    {
        player->SetLevel(targetCap);
        player->InitStatsForLevel(false);
        player->SetUInt32Value(PLAYER_XP, 0);
        player->SetUInt32Value(PLAYER_NEXT_LEVEL_XP, sObjectMgr.GetXPForLevel(targetCap));
        player->SetXPGain(false);

        sLog.outBasic("[BotXpLock] Bot %s (GUID %u) dinged to level %u beyond cap %u; clamped back to %u.",
            player->GetName(), lowGuid, currentLevel, targetCap, targetCap);
        return;
    }

    // 2. Target level reached notification and native XP suppression
    if (entry.lockLevel > 0 && currentLevel >= entry.lockLevel)
    {
        player->SetXPGain(false);
        player->SetUInt32Value(PLAYER_XP, 0);

        if (sBotXpLockConfig.IsNotifyOnLock() && entry.lockedByGuid > 0)
        {
            Player* master = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, entry.lockedByGuid));
            if (master && master->IsInWorld())
            {
                std::string msg = "|cff00ff00[BotXpLock]|r Bot |cffffd100" + std::string(player->GetName()) +
                                  "|r reached Level " + std::to_string(currentLevel) +
                                  "! XP is now automatically |cffff2020LOCKED|r.";
                ChatHandler(master).SendSysMessage(msg.c_str());
                SendStatusAddonMessage(master, lowGuid);
            }
        }
    }
}

void BotXpLockMgr::OnBotLogin(Player* player)
{
    if (!sBotXpLockConfig.IsEnabled() || !player)
        return;

    if (IsXpLocked(player))
    {
        player->SetXPGain(false);
        player->SetUInt32Value(PLAYER_XP, 0);
    }
}

bool BotXpLockMgr::SetLock(uint32 botGuid, bool locked, uint8 lockLevel, uint32 operatorGuid, std::string* outMsg)
{
    Player* bot = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, botGuid));
    std::string botName = bot ? bot->GetName() : ("Bot#" + std::to_string(botGuid));
    uint8 botLevel = bot ? bot->GetLevel() : 0;
    float currentRate = 1.0f;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        BotXpLock::LockEntry& entry = m_locks[botGuid];
        entry.guid = botGuid;
        entry.locked = locked;
        entry.lockLevel = lockLevel;
        if (botLevel > 0)
            entry.lockedAtLevel = botLevel;
        entry.lockedByGuid = operatorGuid;
        entry.botName = botName;
        currentRate = entry.xpRate;
    }

    CharacterDatabase.PExecute(
        "REPLACE INTO `bot_xp_locks` (`guid`, `locked`, `lock_level`, `locked_at_level`, `xp_rate`, `locked_by_guid`, `locked_at`) "
        "VALUES (%u, %u, %u, %u, %f, %u, NOW())",
        botGuid, locked ? 1 : 0, lockLevel, botLevel, currentRate, operatorGuid
    );

    if (bot && bot->IsInWorld())
    {
        if (locked)
        {
            if (lockLevel == 0 || bot->GetLevel() >= lockLevel)
            {
                bot->SetXPGain(false);
                bot->SetUInt32Value(PLAYER_XP, 0);
            }
            else
            {
                bot->SetXPGain(true);
            }
        }
        else
        {
            bot->SetXPGain(true);
        }
    }

    if (outMsg)
    {
        if (locked)
        {
            if (lockLevel == 0 || (botLevel > 0 && botLevel >= lockLevel))
            {
                *outMsg = botName + "'s XP is now |cffff2020LOCKED|r at Level " +
                          std::to_string(botLevel > 0 ? botLevel : lockLevel) + ".";
            }
            else
            {
                *outMsg = botName + "'s XP is capped at Level " + std::to_string(lockLevel) +
                          " (currently Level " + std::to_string(botLevel) + ").";
            }
        }
        else
        {
            *outMsg = botName + "'s XP is now |cff00ff00UNLOCKED|r (rate: " +
                      std::to_string(currentRate).substr(0, 4) + "x).";
        }
    }

    return true;
}

bool BotXpLockMgr::SetRate(uint32 botGuid, float rate, uint32 operatorGuid, std::string* outMsg)
{
    if (rate < 0.0f)
        rate = 0.0f;
    if (rate > 10.0f)
        rate = 10.0f;

    if (rate <= 0.0f)
    {
        return SetLock(botGuid, true, 0, operatorGuid, outMsg);
    }

    Player* bot = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, botGuid));
    std::string botName = bot ? bot->GetName() : ("Bot#" + std::to_string(botGuid));
    uint8 botLevel = bot ? bot->GetLevel() : 0;
    uint8 lockLevel = 0;
    bool locked = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        BotXpLock::LockEntry& entry = m_locks[botGuid];
        entry.guid = botGuid;
        entry.xpRate = rate;
        // Setting a positive rate unfreezes the current-level lock unless a future cap is set
        if (entry.locked && entry.lockLevel == 0)
        {
            entry.locked = false;
        }
        locked = entry.locked;
        lockLevel = entry.lockLevel;
        if (botLevel > 0)
            entry.lockedAtLevel = botLevel;
        entry.lockedByGuid = operatorGuid;
        entry.botName = botName;
    }

    CharacterDatabase.PExecute(
        "REPLACE INTO `bot_xp_locks` (`guid`, `locked`, `lock_level`, `locked_at_level`, `xp_rate`, `locked_by_guid`, `locked_at`) "
        "VALUES (%u, %u, %u, %u, %f, %u, NOW())",
        botGuid, locked ? 1 : 0, lockLevel, botLevel, rate, operatorGuid
    );

    if (bot && bot->IsInWorld())
    {
        bot->SetXPGain(true);
    }

    if (outMsg)
    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2) << rate;
        *outMsg = botName + "'s XP rate set to |cffffd100" + ss.str() + "x|r.";
    }

    return true;
}

bool BotXpLockMgr::GetLockEntry(uint32 botGuid, BotXpLock::LockEntry& outEntry) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_locks.find(botGuid);
    if (it != m_locks.end())
    {
        outEntry = it->second;
        return true;
    }
    return false;
}

std::vector<BotXpLock::LockEntry> BotXpLockMgr::GetAllLockedBots() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<BotXpLock::LockEntry> list;
    for (auto const& pair : m_locks)
    {
        if (pair.second.locked || pair.second.xpRate != 1.0f)
            list.push_back(pair.second);
    }
    return list;
}

void BotXpLockMgr::SendStatusAddonMessage(Player* recipient, uint32 botGuid)
{
    if (!recipient || !recipient->GetSession() || recipient->GetSession()->IsHeadless())
        return;

    Player* bot = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, botGuid));
    std::string botName = bot ? bot->GetName() : ("Bot#" + std::to_string(botGuid));
    uint8 botLevel = bot ? bot->GetLevel() : 0;
    uint32 curXP = bot ? bot->GetUInt32Value(PLAYER_XP) : 0;
    uint32 nextXP = bot ? bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP) : 0;

    bool isLocked = false;
    uint8 lockLevel = 0;
    float xpRate = 1.0f;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_locks.find(botGuid);
        if (it != m_locks.end())
        {
            isLocked = it->second.locked;
            lockLevel = it->second.lockLevel;
            xpRate = it->second.xpRate;
            if (botLevel == 0 && it->second.lockedAtLevel > 0)
                botLevel = it->second.lockedAtLevel;
        }
    }

    std::ostringstream rateStream;
    rateStream << std::fixed << std::setprecision(2) << xpRate;

    // Format: STATUS\t<guid>\t<name>\t<level>\t<curXP>\t<nextXP>\t<isLocked>\t<lockLevel>\t<xpRate>
    std::string payload = "STATUS\t" + std::to_string(botGuid) + "\t" +
                          botName + "\t" +
                          std::to_string(botLevel) + "\t" +
                          std::to_string(curXP) + "\t" +
                          std::to_string(nextXP) + "\t" +
                          (isLocked ? "1" : "0") + "\t" +
                          std::to_string(lockLevel) + "\t" +
                          rateStream.str();

    recipient->SendAddonMessage(BotXpLock::ADDON_PREFIX, payload);
}

void BotXpLockMgr::SendRosterAddonMessage(Player* recipient)
{
    if (!recipient || !recipient->GetSession() || recipient->GetSession()->IsHeadless())
        return;

    Group* grp = recipient->GetGroup();
    if (!grp)
        return;

    for (GroupReference* itr = grp->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (member && IsPlayerBot(member))
        {
            SendStatusAddonMessage(recipient, member->GetGUIDLow());
        }
    }
}

void BotXpLockMgr::SendAllLockedBotsAddonMessage(Player* recipient)
{
    if (!recipient || !recipient->GetSession() || recipient->GetSession()->IsHeadless())
        return;

    auto list = GetAllLockedBots();
    recipient->SendAddonMessage(BotXpLock::ADDON_PREFIX, "LIST_START\t" + std::to_string(list.size()));

    for (auto const& entry : list)
    {
        Player* bot = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, entry.guid));
        std::string name = bot ? bot->GetName() : entry.botName;
        if (name.empty())
            name = "Bot#" + std::to_string(entry.guid);
        uint8 curLvl = bot ? bot->GetLevel() : (entry.lockedAtLevel > 0 ? entry.lockedAtLevel : 0);

        std::ostringstream rateStream;
        rateStream << std::fixed << std::setprecision(2) << entry.xpRate;

        // Format: LIST_ITEM\t<guid>\t<name>\t<level>\t<isLocked>\t<lockLevel>\t<xpRate>
        std::string payload = "LIST_ITEM\t" + std::to_string(entry.guid) + "\t" +
                              name + "\t" +
                              std::to_string(curLvl) + "\t" +
                              (entry.locked ? "1" : "0") + "\t" +
                              std::to_string(entry.lockLevel) + "\t" +
                              rateStream.str();
        recipient->SendAddonMessage(BotXpLock::ADDON_PREFIX, payload);
    }

    recipient->SendAddonMessage(BotXpLock::ADDON_PREFIX, "LIST_END");
}

void BotXpLockMgr::HandleAddonMessage(Player* sender, std::string const& payload)
{
    if (!sBotXpLockConfig.IsEnabled() || !sender)
        return;

    std::stringstream ss(payload);
    std::string action;
    std::getline(ss, action, '\t');

    if (action == "QUERY")
    {
        std::string guidStr;
        if (std::getline(ss, guidStr, '\t'))
        {
            uint32 botGuid = static_cast<uint32>(std::strtoul(guidStr.c_str(), nullptr, 10));
            SendStatusAddonMessage(sender, botGuid);
        }
    }
    else if (action == "LOCK")
    {
        std::string guidStr;
        std::string levelStr;
        if (std::getline(ss, guidStr, '\t'))
        {
            uint32 botGuid = static_cast<uint32>(std::strtoul(guidStr.c_str(), nullptr, 10));
            uint8 targetLevel = 0;
            if (std::getline(ss, levelStr, '\t'))
                targetLevel = static_cast<uint8>(std::strtoul(levelStr.c_str(), nullptr, 10));

            if (sBotXpLockConfig.IsPartyOnly())
            {
                Group* grp = sender->GetGroup();
                if (!grp || !grp->IsMember(ObjectGuid(HIGHGUID_PLAYER, botGuid)))
                {
                    ChatHandler(sender).SendSysMessage("|cffff2020[BotXpLock] You can only lock XP for bots in your party.|r");
                    return;
                }
            }

            std::string feedback;
            SetLock(botGuid, true, targetLevel, sender->GetGUIDLow(), &feedback);

            if (sBotXpLockConfig.IsNotifyOnLock() && !feedback.empty())
                ChatHandler(sender).SendSysMessage(("|cff00ff00[BotXpLock]|r " + feedback).c_str());

            SendStatusAddonMessage(sender, botGuid);
        }
    }
    else if (action == "UNLOCK")
    {
        std::string guidStr;
        if (std::getline(ss, guidStr, '\t'))
        {
            uint32 botGuid = static_cast<uint32>(std::strtoul(guidStr.c_str(), nullptr, 10));

            if (sBotXpLockConfig.IsPartyOnly())
            {
                Group* grp = sender->GetGroup();
                if (!grp || !grp->IsMember(ObjectGuid(HIGHGUID_PLAYER, botGuid)))
                {
                    ChatHandler(sender).SendSysMessage("|cffff2020[BotXpLock] You can only unlock XP for bots in your party.|r");
                    return;
                }
            }

            std::string feedback;
            SetLock(botGuid, false, 0, sender->GetGUIDLow(), &feedback);

            if (sBotXpLockConfig.IsNotifyOnLock() && !feedback.empty())
                ChatHandler(sender).SendSysMessage(("|cff00ff00[BotXpLock]|r " + feedback).c_str());

            SendStatusAddonMessage(sender, botGuid);
        }
    }
    else if (action == "RATE")
    {
        std::string guidStr;
        std::string rateStr;
        if (std::getline(ss, guidStr, '\t') && std::getline(ss, rateStr, '\t'))
        {
            uint32 botGuid = static_cast<uint32>(std::strtoul(guidStr.c_str(), nullptr, 10));
            float rate = static_cast<float>(std::atof(rateStr.c_str()));

            if (sBotXpLockConfig.IsPartyOnly())
            {
                Group* grp = sender->GetGroup();
                if (!grp || !grp->IsMember(ObjectGuid(HIGHGUID_PLAYER, botGuid)))
                {
                    ChatHandler(sender).SendSysMessage("|cffff2020[BotXpLock] You can only set XP rates for bots in your party.|r");
                    return;
                }
            }

            std::string feedback;
            SetRate(botGuid, rate, sender->GetGUIDLow(), &feedback);

            if (sBotXpLockConfig.IsNotifyOnLock() && !feedback.empty())
                ChatHandler(sender).SendSysMessage(("|cff00ff00[BotXpLock]|r " + feedback).c_str());

            SendStatusAddonMessage(sender, botGuid);
        }
    }
    else if (action == "ROSTER_QUERY")
    {
        SendRosterAddonMessage(sender);
    }
    else if (action == "LIST_QUERY")
    {
        SendAllLockedBotsAddonMessage(sender);
    }
}

void BotXpLockMgr::HandleCommand(ChatHandler* handler, Player* sender, std::string const& args)
{
    if (!sBotXpLockConfig.IsEnabled())
    {
        handler->SendSysMessage("[BotXpLock] Module is currently disabled in configuration.");
        return;
    }

    std::stringstream ss(args);
    std::string subCmd;
    ss >> subCmd;
    std::transform(subCmd.begin(), subCmd.end(), subCmd.begin(), ::tolower);

    // Resolve target player/bot
    Player* target = nullptr;
    if (sender->GetSelectionGuid())
        target = sObjectMgr.GetPlayer(sender->GetSelectionGuid());

    if (subCmd == "list")
    {
        auto lockedList = GetAllLockedBots();
        handler->PSendSysMessage("|cff00ff00=== Currently Configured Bots (%zu) ===|r", lockedList.size());
        if (lockedList.empty())
        {
            handler->SendSysMessage("No bots currently have custom XP locks or rates.");
            return;
        }

        for (auto const& entry : lockedList)
        {
            Player* bot = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, entry.guid));
            std::string name = bot ? bot->GetName() : entry.botName;
            uint8 curLvl = bot ? bot->GetLevel() : entry.lockedAtLevel;
            if (entry.locked)
            {
                if (entry.lockLevel == 0)
                    handler->PSendSysMessage(" - |cffffd100%s|r (Lvl %u): |cffff2020LOCKED|r (0x XP)", name.c_str(), curLvl);
                else
                    handler->PSendSysMessage(" - |cffffd100%s|r (Lvl %u): |cffffa500CAPPED at %u|r (Rate: %.2fx)", name.c_str(), curLvl, entry.lockLevel, entry.xpRate);
            }
            else
            {
                handler->PSendSysMessage(" - |cffffd100%s|r (Lvl %u): |cff00ff00ACTIVE|r (Rate: %.2fx)", name.c_str(), curLvl, entry.xpRate);
            }
        }
        return;
    }

    if (subCmd.empty() || subCmd == "status" || subCmd == "info")
    {
        if (!target)
        {
            handler->SendSysMessage("|cffff2020Usage:|r .botxplock <lock [cap]|unlock|rate <mult>|status|list|help>");
            handler->SendSysMessage("Target a companion bot first, or type .botxplock help.");
            return;
        }

        uint32 botGuid = target->GetGUIDLow();
        bool locked = false;
        uint8 capLvl = 0;
        float rate = 1.0f;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_locks.find(botGuid);
            if (it != m_locks.end())
            {
                locked = it->second.locked;
                capLvl = it->second.lockLevel;
                rate = it->second.xpRate;
            }
        }

        handler->PSendSysMessage("|cff00ff00[BotXpLock]|r Target: |cffffd100%s|r (Level %u%s)",
            target->GetName(), target->GetLevel(), IsPlayerBot(target) ? ", Bot" : "");

        if (locked)
        {
            if (capLvl == 0 || target->GetLevel() >= capLvl)
                handler->PSendSysMessage("XP Status: |cffff2020LOCKED|r (Cannot gain XP, stays at Level %u)", target->GetLevel());
            else
                handler->PSendSysMessage("XP Status: |cffffa500CAPPED at Level %u|r (Currently leveling, stops at %u; Rate: %.2fx)", capLvl, capLvl, rate);
        }
        else
        {
            handler->PSendSysMessage("XP Status: |cff00ff00UNLOCKED|r (Active Rate: |cffffd100%.2fx|r)", rate);
        }

        SendStatusAddonMessage(sender, botGuid);
        return;
    }

    if (subCmd == "lock" || subCmd == "freeze")
    {
        if (!target)
        {
            handler->SendSysMessage("|cffff2020Error:|r Target a bot to lock XP, or type .botxplock help.");
            return;
        }

        uint32 capArg = 0;
        ss >> capArg;
        uint8 capLvl = static_cast<uint8>(capArg);

        std::string feedback;
        SetLock(target->GetGUIDLow(), true, capLvl, sender->GetGUIDLow(), &feedback);

        handler->PSendSysMessage("|cff00ff00[BotXpLock]|r %s", feedback.c_str());
        SendStatusAddonMessage(sender, target->GetGUIDLow());
        return;
    }

    if (subCmd == "unlock" || subCmd == "unfreeze")
    {
        if (!target)
        {
            handler->SendSysMessage("|cffff2020Error:|r Target a bot to unlock XP, or type .botxplock help.");
            return;
        }

        std::string feedback;
        SetLock(target->GetGUIDLow(), false, 0, sender->GetGUIDLow(), &feedback);

        handler->PSendSysMessage("|cff00ff00[BotXpLock]|r %s", feedback.c_str());
        SendStatusAddonMessage(sender, target->GetGUIDLow());
        return;
    }

    if (subCmd == "rate")
    {
        if (!target)
        {
            handler->SendSysMessage("|cffff2020Error:|r Target a bot to set XP rate (e.g. .botxplock rate 0.5).");
            return;
        }

        float rateArg = 1.0f;
        if (!(ss >> rateArg))
        {
            handler->SendSysMessage("|cffff2020Usage:|r .botxplock rate <multiplier> (e.g. 0.5, 1.0, 2.0)");
            return;
        }

        std::string feedback;
        SetRate(target->GetGUIDLow(), rateArg, sender->GetGUIDLow(), &feedback);

        handler->PSendSysMessage("|cff00ff00[BotXpLock]|r %s", feedback.c_str());
        SendStatusAddonMessage(sender, target->GetGUIDLow());
        return;
    }

    if (subCmd == "help")
    {
        handler->SendSysMessage("|cff00ff00=== Bot XP Lock & Rate Commands ===|r");
        handler->SendSysMessage(" .botxplock status          - View XP lock and rate status for targeted bot");
        handler->SendSysMessage(" .botxplock lock            - Lock XP immediately at targeted bot's current level");
        handler->SendSysMessage(" .botxplock lock <level>    - Cap targeted bot's XP at specified level (e.g. 19, 29)");
        handler->SendSysMessage(" .botxplock rate <mult>     - Set individual XP rate (e.g. 0.5 for half XP, 2.0 for double)");
        handler->SendSysMessage(" .botxplock unlock          - Unlock XP for targeted bot");
        handler->SendSysMessage(" .botxplock list            - List all bots with custom locks or rates");
        return;
    }

    handler->PSendSysMessage("|cffff2020Unknown subcommand '%s'. Type .botxplock help.|r", subCmd.c_str());
}
