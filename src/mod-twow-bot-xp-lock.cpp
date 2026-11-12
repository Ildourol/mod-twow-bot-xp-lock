#include "ScriptObjects.h"
#include "Log.h"

extern void AddBotXpLockScripts();

void Addmod_twow_bot_xp_lockScripts()
{
    AddBotXpLockScripts();
    sLog.outString("[mod-twow-bot-xp-lock] All scripts registered successfully.");
}
