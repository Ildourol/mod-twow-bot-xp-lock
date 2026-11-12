#ifndef BOT_XP_LOCK_COMMON_H
#define BOT_XP_LOCK_COMMON_H

#include "Common.h"
#include <string>

namespace BotXpLock
{
    static constexpr char const* ADDON_PREFIX = "TW_BOT_XPLOCK";

    struct LockEntry
    {
        uint32 guid = 0;
        bool locked = false;
        uint8 lockLevel = 0;      // 0 = lock at current level; >0 = target cap level
        uint8 lockedAtLevel = 0;  // Level when lock was applied
        float xpRate = 1.0f;      // Individual bot XP rate multiplier (e.g. 0.5f, 2.0f, 0.0f)
        uint32 lockedByGuid = 0;
        std::string botName;
    };
}

#endif // BOT_XP_LOCK_COMMON_H
