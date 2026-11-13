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
        "SELECT l.guid, l.locked, l.lock_level, l.locked_at_level, l.xp_rate, l.locked_by_guid, c.name, c.level "
        "FROM `bot_xp_locks` l "
        "LEFT JOIN `characters` c ON c.guid = l.guid"
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

        char const* name = fields[6].GetString();
        if (name && *name)
            entry.botName = name;
        else
            entry.botName = "Bot#" + std::to_string(guid);

        uint8 charLevel = fields[7].GetUInt8();
        if (charLevel > 0)
            entry.lockedAtLevel = charLevel;

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
    if (m_locks.empty())
        return false;

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
    return (it->second.xpRate <= 0.0f);
}

float BotXpLockMgr::GetBotXpRate(Player* player) const
{
    if (!sBotXpLockConfig.IsEnabled() || !player)
        return 1.0f;

    uint32 lowGuid = player->GetGUIDLow();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_locks.empty())
        return 1.0f;

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
    if (amount < 1 || !player || !sBotXpLockConfig.IsEnabled())
        return;

    uint32 lowGuid = player->GetGUIDLow();
    float rate = 1.0f;
    bool locked = false;

    // Single-pass lock and rate evaluation under a single mutex lock
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_locks.empty())
            return;

        auto it = m_locks.find(lowGuid);
        if (it == m_locks.end())
            return; // Not in lock registry; normal 1.0x XP

        auto const& entry = it->second;
        if (entry.locked)
        {
            if (entry.lockLevel == 0 || player->GetLevel() >= entry.lockLevel)
                locked = true;
        }

        if (entry.xpRate <= 0.0f)
            locked = true;
        else
            rate = entry.xpRate;
    }

    if (locked)
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

std::string BotXpLockMgr::GetBotName(uint32 botGuid) const
{
    if (botGuid == 0)
        return "";

    Player* bot = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, botGuid));
    if (bot)
        return bot->GetName();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_locks.find(botGuid);
        if (it != m_locks.end() && !it->second.botName.empty() && it->second.botName.find("Bot#") != 0)
            return it->second.botName;
    }

    std::unique_ptr<QueryResult> res(CharacterDatabase.PQuery(
        "SELECT `name` FROM `characters` WHERE `guid` = %u", botGuid));
    if (res)
    {
        Field* fields = res->Fetch();
        char const* name = fields[0].GetString();
        if (name && *name)
            return name;
    }

    return "Bot#" + std::to_string(botGuid);
}

uint8 BotXpLockMgr::GetBotLevel(uint32 botGuid) const
{
    if (botGuid == 0)
        return 0;

    Player* bot = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, botGuid));
    if (bot)
        return bot->GetLevel();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_locks.find(botGuid);
        if (it != m_locks.end() && it->second.lockedAtLevel > 0)
            return it->second.lockedAtLevel;
    }

    std::unique_ptr<QueryResult> res(CharacterDatabase.PQuery(
        "SELECT `level` FROM `characters` WHERE `guid` = %u", botGuid));
    if (res)
    {
        Field* fields = res->Fetch();
        uint8 lvl = fields[0].GetUInt8();
        if (lvl > 0)
            return lvl;
    }

    return 0;
}

bool BotXpLockMgr::ResolveBot(Player* sender, std::string const& identifier, uint32& outGuid, std::string& outName, uint8& outLevel, Player*& outPlayer) const
{
    outGuid = 0;
    outName.clear();
    outLevel = 0;
    outPlayer = nullptr;

    if (!identifier.empty())
    {
        // 1. Try numeric low GUID
        char* endPtr = nullptr;
        unsigned long parsedGuid = std::strtoul(identifier.c_str(), &endPtr, 10);
        if (endPtr != identifier.c_str() && *endPtr == '\0' && parsedGuid > 0)
        {
            outGuid = static_cast<uint32>(parsedGuid);
            outPlayer = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, outGuid));
            if (outPlayer)
            {
                outName = outPlayer->GetName();
                outLevel = outPlayer->GetLevel();
                return true;
            }

            outName = GetBotName(outGuid);
            outLevel = GetBotLevel(outGuid);
            return true;
        }

        // 2. Try character name
        outPlayer = sObjectMgr.GetPlayer(identifier.c_str());
        if (outPlayer)
        {
            outGuid = outPlayer->GetGUIDLow();
            outName = outPlayer->GetName();
            outLevel = outPlayer->GetLevel();
            return true;
        }

        std::string escapedName = identifier;
        CharacterDatabase.escape_string(escapedName);
        std::unique_ptr<QueryResult> res(CharacterDatabase.PQuery(
            "SELECT `guid`, `name`, `level` FROM `characters` WHERE `name` = '%s'", escapedName.c_str()));
        if (res)
        {
            Field* fields = res->Fetch();
            outGuid = fields[0].GetUInt32();
            char const* name = fields[1].GetString();
            outName = (name && *name) ? name : identifier;
            outLevel = fields[2].GetUInt8();
            return true;
        }

        // Check in-memory locks by case-insensitive name
        auto StrCaseEqual = [](std::string const& a, std::string const& b)
        {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            }
            return true;
        };

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto const& pair : m_locks)
            {
                if (StrCaseEqual(pair.second.botName, identifier))
                {
                    outGuid = pair.first;
                    outName = pair.second.botName;
                    outLevel = pair.second.lockedAtLevel;
                    return true;
                }
            }
        }

        return false;
    }

    // 3. Fallback to sender's target if identifier is empty
    if (sender && sender->GetSelectionGuid() && sender->GetSelectionGuid().IsPlayer())
    {
        outPlayer = sObjectMgr.GetPlayer(sender->GetSelectionGuid());
        if (outPlayer)
        {
            outGuid = outPlayer->GetGUIDLow();
            outName = outPlayer->GetName();
            outLevel = outPlayer->GetLevel();
            return true;
        }
        else
        {
            outGuid = sender->GetSelectionGuid().GetCounter();
            outName = GetBotName(outGuid);
            outLevel = GetBotLevel(outGuid);
            return true;
        }
    }

    return false;
}

bool BotXpLockMgr::SetLock(uint32 botGuid, bool locked, uint8 lockLevel, uint32 operatorGuid, std::string* outMsg)
{
    if (botGuid == 0)
        return false;

    std::string botName = GetBotName(botGuid);
    uint8 botLevel = GetBotLevel(botGuid);
    float currentRate = 1.0f;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        BotXpLock::LockEntry& entry = m_locks[botGuid];
        entry.guid = botGuid;
        entry.locked = locked;
        entry.lockLevel = lockLevel;
        if (botLevel > 0)
            entry.lockedAtLevel = botLevel;
        else if (entry.lockedAtLevel == 0)
            entry.lockedAtLevel = 1;
        entry.lockedByGuid = operatorGuid;
        entry.botName = botName;
        if (!locked && entry.xpRate <= 0.0f)
            entry.xpRate = 1.0f;
        currentRate = entry.xpRate;
    }

    CharacterDatabase.PExecute(
        "REPLACE INTO `bot_xp_locks` (`guid`, `locked`, `lock_level`, `locked_at_level`, `xp_rate`, `locked_by_guid`, `locked_at`) "
        "VALUES (%u, %u, %u, %u, %f, %u, NOW())",
        botGuid, locked ? 1 : 0, lockLevel, botLevel > 0 ? botLevel : 1, currentRate, operatorGuid
    );

    Player* bot = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, botGuid));
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
            if (lockLevel > 0)
            {
                *outMsg = botName + "'s XP is capped at Level " + std::to_string(lockLevel) +
                          " (currently Level " + std::to_string(botLevel) + ").";
            }
            else
            {
                *outMsg = botName + "'s XP is now |cffff2020LOCKED|r at Level " +
                          std::to_string(botLevel > 0 ? botLevel : 1) + " (0x XP).";
            }
        }
        else
        {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(2) << currentRate;
            *outMsg = botName + "'s XP is now |cff00ff00UNLOCKED|r (rate: " + ss.str() + "x).";
        }
    }

    return true;
}

bool BotXpLockMgr::SetRate(uint32 botGuid, float rate, uint32 operatorGuid, std::string* outMsg)
{
    if (botGuid == 0)
        return false;

    if (rate < 0.0f)
        rate = 0.0f;
    if (rate > 10.0f)
        rate = 10.0f;

    if (rate <= 0.0f)
    {
        return SetLock(botGuid, true, 0, operatorGuid, outMsg);
    }

    std::string botName = GetBotName(botGuid);
    uint8 botLevel = GetBotLevel(botGuid);
    uint8 lockLevel = 0;
    bool locked = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        BotXpLock::LockEntry& entry = m_locks[botGuid];
        entry.guid = botGuid;
        entry.xpRate = rate;
        // Setting a positive rate unfreezes current level lock unless a future cap is set
        if (entry.locked && entry.lockLevel == 0)
        {
            entry.locked = false;
        }
        locked = entry.locked;
        lockLevel = entry.lockLevel;
        if (botLevel > 0)
            entry.lockedAtLevel = botLevel;
        else if (entry.lockedAtLevel == 0)
            entry.lockedAtLevel = 1;
        entry.lockedByGuid = operatorGuid;
        entry.botName = botName;
    }

    CharacterDatabase.PExecute(
        "REPLACE INTO `bot_xp_locks` (`guid`, `locked`, `lock_level`, `locked_at_level`, `xp_rate`, `locked_by_guid`, `locked_at`) "
        "VALUES (%u, %u, %u, %u, %f, %u, NOW())",
        botGuid, locked ? 1 : 0, lockLevel, botLevel > 0 ? botLevel : 1, rate, operatorGuid
    );

    Player* bot = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, botGuid));
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

bool BotXpLockMgr::DeleteLock(uint32 botGuid, uint32 operatorGuid, std::string* outMsg)
{
    if (botGuid == 0)
        return false;

    std::string botName = GetBotName(botGuid);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_locks.erase(botGuid);
    }

    CharacterDatabase.PExecute(
        "DELETE FROM `bot_xp_locks` WHERE `guid` = %u",
        botGuid
    );

    Player* bot = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, botGuid));
    if (bot && bot->IsInWorld())
    {
        bot->SetXPGain(true);
    }

    if (outMsg)
    {
        *outMsg = botName + " has been removed from SQL database and reset to default XP gain.";
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
        list.push_back(pair.second);
    }
    return list;
}

void BotXpLockMgr::SendStatusAddonMessage(Player* recipient, uint32 botGuid)
{
    if (!recipient || !recipient->GetSession() || recipient->GetSession()->IsHeadless() || botGuid == 0)
        return;

    Player* bot = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, botGuid));
    std::string botName = bot ? bot->GetName() : GetBotName(botGuid);
    uint8 botLevel = bot ? bot->GetLevel() : GetBotLevel(botGuid);
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
        if (member && member != recipient)
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
        if (name.empty() || name.find("Bot#") == 0)
            name = GetBotName(entry.guid);
        uint8 curLvl = bot ? bot->GetLevel() : (entry.lockedAtLevel > 0 ? entry.lockedAtLevel : GetBotLevel(entry.guid));

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
        std::string levelStr;
        std::string nameStr;
        if (std::getline(ss, guidStr, '\t'))
        {
            uint32 botGuid = static_cast<uint32>(std::strtoul(guidStr.c_str(), nullptr, 10));
            std::getline(ss, levelStr, '\t');
            std::getline(ss, nameStr, '\t');

            if (botGuid == 0 && !nameStr.empty())
            {
                std::string botName;
                uint8 botLvl = 0;
                Player* botPlayer = nullptr;
                ResolveBot(sender, nameStr, botGuid, botName, botLvl, botPlayer);
            }

            if (botGuid == 0 && sender->GetSelectionGuid().IsPlayer())
                botGuid = sender->GetSelectionGuid().GetCounter();

            if (botGuid > 0)
                SendStatusAddonMessage(sender, botGuid);
        }
    }
    else if (action == "LOCK")
    {
        std::string guidStr;
        std::string levelStr;
        std::string nameStr;
        if (std::getline(ss, guidStr, '\t'))
        {
            uint32 botGuid = static_cast<uint32>(std::strtoul(guidStr.c_str(), nullptr, 10));
            uint8 targetLevel = 0;
            if (std::getline(ss, levelStr, '\t'))
                targetLevel = static_cast<uint8>(std::strtoul(levelStr.c_str(), nullptr, 10));
            std::getline(ss, nameStr, '\t');

            if (botGuid == 0 && !nameStr.empty())
            {
                std::string botName;
                uint8 botLvl = 0;
                Player* botPlayer = nullptr;
                ResolveBot(sender, nameStr, botGuid, botName, botLvl, botPlayer);
            }

            if (botGuid == 0 && sender->GetSelectionGuid().IsPlayer())
                botGuid = sender->GetSelectionGuid().GetCounter();

            if (botGuid > 0)
            {
                if (sBotXpLockConfig.IsPartyOnly() && (!sender->GetGroup() || !sender->GetGroup()->IsMember(ObjectGuid(HIGHGUID_PLAYER, botGuid))))
                {
                    ChatHandler(sender).SendSysMessage("|cffff2020[BotXpLock] You can only lock XP for bots in your party.|r");
                    return;
                }

                std::string feedback;
                SetLock(botGuid, true, targetLevel, sender->GetGUIDLow(), &feedback);

                if (sBotXpLockConfig.IsNotifyOnLock() && !feedback.empty())
                    ChatHandler(sender).SendSysMessage(("|cff00ff00[BotXpLock]|r " + feedback).c_str());

                SendStatusAddonMessage(sender, botGuid);
            }
        }
    }
    else if (action == "UNLOCK")
    {
        std::string guidStr;
        std::string nameStr;
        if (std::getline(ss, guidStr, '\t'))
        {
            uint32 botGuid = static_cast<uint32>(std::strtoul(guidStr.c_str(), nullptr, 10));
            std::getline(ss, nameStr, '\t');

            if (botGuid == 0 && !nameStr.empty())
            {
                std::string botName;
                uint8 botLvl = 0;
                Player* botPlayer = nullptr;
                ResolveBot(sender, nameStr, botGuid, botName, botLvl, botPlayer);
            }

            if (botGuid == 0 && sender->GetSelectionGuid().IsPlayer())
                botGuid = sender->GetSelectionGuid().GetCounter();

            if (botGuid > 0)
            {
                if (sBotXpLockConfig.IsPartyOnly() && (!sender->GetGroup() || !sender->GetGroup()->IsMember(ObjectGuid(HIGHGUID_PLAYER, botGuid))))
                {
                    ChatHandler(sender).SendSysMessage("|cffff2020[BotXpLock] You can only unlock XP for bots in your party.|r");
                    return;
                }

                std::string feedback;
                SetLock(botGuid, false, 0, sender->GetGUIDLow(), &feedback);

                if (sBotXpLockConfig.IsNotifyOnLock() && !feedback.empty())
                    ChatHandler(sender).SendSysMessage(("|cff00ff00[BotXpLock]|r " + feedback).c_str());

                SendStatusAddonMessage(sender, botGuid);
            }
        }
    }
    else if (action == "RATE")
    {
        std::string guidStr;
        std::string rateStr;
        std::string nameStr;
        if (std::getline(ss, guidStr, '\t') && std::getline(ss, rateStr, '\t'))
        {
            uint32 botGuid = static_cast<uint32>(std::strtoul(guidStr.c_str(), nullptr, 10));
            float rate = static_cast<float>(std::atof(rateStr.c_str()));
            std::getline(ss, nameStr, '\t');

            if (botGuid == 0 && !nameStr.empty())
            {
                std::string botName;
                uint8 botLvl = 0;
                Player* botPlayer = nullptr;
                ResolveBot(sender, nameStr, botGuid, botName, botLvl, botPlayer);
            }

            if (botGuid == 0 && sender->GetSelectionGuid().IsPlayer())
                botGuid = sender->GetSelectionGuid().GetCounter();

            if (botGuid > 0)
            {
                if (sBotXpLockConfig.IsPartyOnly() && (!sender->GetGroup() || !sender->GetGroup()->IsMember(ObjectGuid(HIGHGUID_PLAYER, botGuid))))
                {
                    ChatHandler(sender).SendSysMessage("|cffff2020[BotXpLock] You can only set XP rates for bots in your party.|r");
                    return;
                }

                std::string feedback;
                SetRate(botGuid, rate, sender->GetGUIDLow(), &feedback);

                if (sBotXpLockConfig.IsNotifyOnLock() && !feedback.empty())
                    ChatHandler(sender).SendSysMessage(("|cff00ff00[BotXpLock]|r " + feedback).c_str());

                SendStatusAddonMessage(sender, botGuid);
            }
        }
    }
    else if (action == "TOGGLE")
    {
        std::string guidStr;
        std::string nameStr;
        if (std::getline(ss, guidStr, '\t'))
        {
            uint32 botGuid = static_cast<uint32>(std::strtoul(guidStr.c_str(), nullptr, 10));
            std::getline(ss, nameStr, '\t');

            if (botGuid == 0 && !nameStr.empty())
            {
                std::string botName;
                uint8 botLvl = 0;
                Player* botPlayer = nullptr;
                ResolveBot(sender, nameStr, botGuid, botName, botLvl, botPlayer);
            }

            if (botGuid == 0 && sender->GetSelectionGuid().IsPlayer())
                botGuid = sender->GetSelectionGuid().GetCounter();

            if (botGuid > 0)
            {
                if (sBotXpLockConfig.IsPartyOnly() && (!sender->GetGroup() || !sender->GetGroup()->IsMember(ObjectGuid(HIGHGUID_PLAYER, botGuid))))
                {
                    ChatHandler(sender).SendSysMessage("|cffff2020[BotXpLock] You can only toggle XP for bots in your party.|r");
                    return;
                }

                bool currentlyLocked = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    auto it = m_locks.find(botGuid);
                    if (it != m_locks.end())
                    {
                        currentlyLocked = it->second.locked || (it->second.xpRate <= 0.0f);
                    }
                }

                std::string feedback;
                SetLock(botGuid, !currentlyLocked, 0, sender->GetGUIDLow(), &feedback);

                if (sBotXpLockConfig.IsNotifyOnLock() && !feedback.empty())
                    ChatHandler(sender).SendSysMessage(("|cff00ff00[BotXpLock]|r " + feedback).c_str());

                SendStatusAddonMessage(sender, botGuid);
            }
        }
    }
    else if (action == "DELETE")
    {
        std::string guidStr;
        std::string nameStr;
        if (std::getline(ss, guidStr, '\t'))
        {
            uint32 botGuid = static_cast<uint32>(std::strtoul(guidStr.c_str(), nullptr, 10));
            std::getline(ss, nameStr, '\t');

            if (botGuid == 0 && !nameStr.empty())
            {
                std::string botName;
                uint8 botLvl = 0;
                Player* botPlayer = nullptr;
                ResolveBot(sender, nameStr, botGuid, botName, botLvl, botPlayer);
            }

            if (botGuid == 0 && sender->GetSelectionGuid().IsPlayer())
                botGuid = sender->GetSelectionGuid().GetCounter();

            if (botGuid > 0)
            {
                std::string feedback;
                DeleteLock(botGuid, sender->GetGUIDLow(), &feedback);

                if (sBotXpLockConfig.IsNotifyOnLock() && !feedback.empty())
                    ChatHandler(sender).SendSysMessage(("|cff00ff00[BotXpLock]|r " + feedback).c_str());

                SendAllLockedBotsAddonMessage(sender);
                SendStatusAddonMessage(sender, botGuid);
            }
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

    if (subCmd == "list")
    {
        auto lockedList = GetAllLockedBots();
        handler->PSendSysMessage("|cff00ff00=== Currently Configured Bots (%zu) ===|r", lockedList.size());
        if (lockedList.empty())
        {
            handler->SendSysMessage("No bots currently have custom XP locks or rates.");
            SendAllLockedBotsAddonMessage(sender);
            return;
        }

        for (auto const& entry : lockedList)
        {
            Player* bot = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, entry.guid));
            std::string name = bot ? bot->GetName() : entry.botName;
            if (name.empty() || name.find("Bot#") == 0)
                name = GetBotName(entry.guid);
            uint8 curLvl = bot ? bot->GetLevel() : (entry.lockedAtLevel > 0 ? entry.lockedAtLevel : GetBotLevel(entry.guid));

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

        // Also deliver wire packets directly to caller so the client UI registry is populated immediately
        SendAllLockedBotsAddonMessage(sender);
        return;
    }

    if (subCmd.empty() || subCmd == "status" || subCmd == "info")
    {
        std::string botArg;
        ss >> botArg;

        uint32 botGuid = 0;
        std::string botName;
        uint8 botLvl = 0;
        Player* botPlayer = nullptr;

        if (!ResolveBot(sender, botArg, botGuid, botName, botLvl, botPlayer) || botGuid == 0)
        {
            handler->SendSysMessage("|cffff2020Usage:|r .botxplock <lock [cap]|unlock|rate <mult>|status|list|help> [name|guid]");
            handler->SendSysMessage("Target a companion bot first, or specify bot name/GUID.");
            return;
        }

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

        bool isBot = botPlayer ? IsPlayerBot(botPlayer) : true;
        handler->PSendSysMessage("|cff00ff00[BotXpLock]|r Bot: |cffffd100%s|r (Level %u%s)",
            botName.c_str(), botLvl, isBot ? ", Bot" : "");

        if (locked)
        {
            if (capLvl == 0 || botLvl >= capLvl)
                handler->PSendSysMessage("XP Status: |cffff2020LOCKED|r (Cannot gain XP, stays at Level %u)", botLvl);
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

    if (subCmd == "toggle")
    {
        std::string botArg;
        ss >> botArg;

        uint32 botGuid = 0;
        std::string botName;
        uint8 botLvl = 0;
        Player* botPlayer = nullptr;

        if (!ResolveBot(sender, botArg, botGuid, botName, botLvl, botPlayer) || botGuid == 0)
        {
            handler->SendSysMessage("|cffff2020Error:|r Target a bot to toggle XP lock, or specify bot GUID/name: .botxplock toggle <guid|name>.");
            return;
        }

        bool currentlyLocked = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_locks.find(botGuid);
            if (it != m_locks.end())
            {
                currentlyLocked = it->second.locked || (it->second.xpRate <= 0.0f);
            }
        }

        std::string feedback;
        SetLock(botGuid, !currentlyLocked, 0, sender->GetGUIDLow(), &feedback);

        handler->PSendSysMessage("|cff00ff00[BotXpLock]|r %s", feedback.c_str());
        SendStatusAddonMessage(sender, botGuid);
        return;
    }

    if (subCmd == "lock" || subCmd == "freeze")
    {
        std::string firstToken;
        std::string secondToken;
        ss >> firstToken;
        ss >> secondToken;

        uint32 botGuid = 0;
        std::string botName;
        uint8 botLvl = 0;
        Player* botPlayer = nullptr;
        uint8 capLvl = 0;

        if (!firstToken.empty() && !secondToken.empty())
        {
            // .botxplock lock <guid|name> <cap>
            if (!ResolveBot(sender, firstToken, botGuid, botName, botLvl, botPlayer) || botGuid == 0)
            {
                handler->PSendSysMessage("|cffff2020Error:|r Could not find bot '%s'.", firstToken.c_str());
                return;
            }
            capLvl = static_cast<uint8>(std::strtoul(secondToken.c_str(), nullptr, 10));
        }
        else if (!firstToken.empty() && secondToken.empty())
        {
            // Check if firstToken is a pure numeric cap 1..60 with active target
            char* endPtr = nullptr;
            unsigned long num = std::strtoul(firstToken.c_str(), &endPtr, 10);
            if (endPtr != firstToken.c_str() && *endPtr == '\0' && num >= 1 && num <= 60 && sender->GetSelectionGuid() && sender->GetSelectionGuid().IsPlayer())
            {
                // Capping current target at level num
                ResolveBot(sender, "", botGuid, botName, botLvl, botPlayer);
                capLvl = static_cast<uint8>(num);
            }
            else
            {
                // It's a bot identifier (name or guid > 60 or guid without target)
                if (!ResolveBot(sender, firstToken, botGuid, botName, botLvl, botPlayer) || botGuid == 0)
                {
                    handler->PSendSysMessage("|cffff2020Error:|r Could not find bot '%s'.", firstToken.c_str());
                    return;
                }
                capLvl = 0; // Freeze at current level
            }
        }
        else
        {
            // No arguments: lock current target at current level
            if (!ResolveBot(sender, "", botGuid, botName, botLvl, botPlayer) || botGuid == 0)
            {
                handler->SendSysMessage("|cffff2020Error:|r Target a bot to lock XP, or specify bot GUID/name: .botxplock lock <guid|name> [cap].");
                return;
            }
            capLvl = 0;
        }

        std::string feedback;
        SetLock(botGuid, true, capLvl, sender->GetGUIDLow(), &feedback);

        handler->PSendSysMessage("|cff00ff00[BotXpLock]|r %s", feedback.c_str());
        SendStatusAddonMessage(sender, botGuid);
        return;
    }

    if (subCmd == "unlock" || subCmd == "unfreeze")
    {
        std::string botArg;
        ss >> botArg;

        uint32 botGuid = 0;
        std::string botName;
        uint8 botLvl = 0;
        Player* botPlayer = nullptr;

        if (!ResolveBot(sender, botArg, botGuid, botName, botLvl, botPlayer) || botGuid == 0)
        {
            handler->SendSysMessage("|cffff2020Error:|r Target a bot to unlock XP, or specify bot GUID/name: .botxplock unlock <guid|name>.");
            return;
        }

        std::string feedback;
        SetLock(botGuid, false, 0, sender->GetGUIDLow(), &feedback);

        handler->PSendSysMessage("|cff00ff00[BotXpLock]|r %s", feedback.c_str());
        SendStatusAddonMessage(sender, botGuid);
        return;
    }

    if (subCmd == "delete" || subCmd == "remove" || subCmd == "reset")
    {
        std::string botArg;
        ss >> botArg;

        uint32 botGuid = 0;
        std::string botName;
        uint8 botLvl = 0;
        Player* botPlayer = nullptr;

        if (!ResolveBot(sender, botArg, botGuid, botName, botLvl, botPlayer) || botGuid == 0)
        {
            handler->SendSysMessage("|cffff2020Error:|r Target a bot or specify GUID/name (e.g. .botxplock delete <guid|name>).");
            return;
        }

        std::string feedback;
        DeleteLock(botGuid, sender->GetGUIDLow(), &feedback);

        handler->PSendSysMessage("|cff00ff00[BotXpLock]|r %s", feedback.c_str());
        SendAllLockedBotsAddonMessage(sender);
        SendStatusAddonMessage(sender, botGuid);
        return;
    }

    if (subCmd == "rate")
    {
        std::string firstToken;
        std::string secondToken;
        ss >> firstToken;
        ss >> secondToken;

        uint32 botGuid = 0;
        std::string botName;
        uint8 botLvl = 0;
        Player* botPlayer = nullptr;
        float rateArg = 1.0f;

        if (!firstToken.empty() && !secondToken.empty())
        {
            // .botxplock rate <guid|name> <rate>
            if (!ResolveBot(sender, firstToken, botGuid, botName, botLvl, botPlayer) || botGuid == 0)
            {
                handler->PSendSysMessage("|cffff2020Error:|r Could not find bot '%s'.", firstToken.c_str());
                return;
            }
            rateArg = static_cast<float>(std::atof(secondToken.c_str()));
        }
        else if (!firstToken.empty())
        {
            // .botxplock rate <rate> on current target
            if (!ResolveBot(sender, "", botGuid, botName, botLvl, botPlayer) || botGuid == 0)
            {
                handler->SendSysMessage("|cffff2020Error:|r Target a bot to set XP rate, or specify: .botxplock rate <guid|name> <multiplier>.");
                return;
            }
            rateArg = static_cast<float>(std::atof(firstToken.c_str()));
        }
        else
        {
            handler->SendSysMessage("|cffff2020Usage:|r .botxplock rate [guid|name] <multiplier> (e.g. 0.5, 1.0, 2.0)");
            return;
        }

        std::string feedback;
        SetRate(botGuid, rateArg, sender->GetGUIDLow(), &feedback);

        handler->PSendSysMessage("|cff00ff00[BotXpLock]|r %s", feedback.c_str());
        SendStatusAddonMessage(sender, botGuid);
        return;
    }

    if (subCmd == "help")
    {
        handler->SendSysMessage("|cff00ff00=== Bot XP Lock & Rate Commands ===|r");
        handler->SendSysMessage(" .botxplock status [guid|name]      - View XP lock and rate status");
        handler->SendSysMessage(" .botxplock toggle [guid|name]      - Toggle XP lock (freeze/unlock)");
        handler->SendSysMessage(" .botxplock lock [guid|name] [lvl]  - Lock XP immediately or set level cap");
        handler->SendSysMessage(" .botxplock unlock [guid|name]      - Unlock XP");
        handler->SendSysMessage(" .botxplock rate [guid|name] <mult> - Set individual XP rate (e.g. 0.5, 2.0)");
        handler->SendSysMessage(" .botxplock delete [guid|name]    - Remove bot from database and reset XP");
        handler->SendSysMessage(" .botxplock list                    - List all configured bots");
        return;
    }

    handler->PSendSysMessage("|cffff2020Unknown subcommand '%s'. Type .botxplock help.|r", subCmd.c_str());
}
