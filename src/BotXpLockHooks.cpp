#include "BotXpLockMgr.h"
#include "BotXpLockConfig.h"
#include "ScriptObjects.h"
#include "Player.h"
#include "Chat/Chat.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <cstring>

namespace
{
class BotXpLockPlayerScript : public PlayerScript
{
public:
    BotXpLockPlayerScript()
        : PlayerScript("BotXpLockPlayerScript", {
            PLAYERHOOK_ON_GIVE_EXP,
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_LEVEL_CHANGED
        })
    {
    }

    void OnGiveXP(Player* player, uint32& amount, Unit* /*victim*/) override
    {
        sBotXpLockMgr.ModifyXP(player, amount);
    }

    void OnLogin(Player* player) override
    {
        sBotXpLockMgr.OnBotLogin(player);
    }

    void OnLevelChanged(Player* player, uint8 oldLevel) override
    {
        sBotXpLockMgr.OnLevelChanged(player, oldLevel);
    }
};

class BotXpLockWorldScript : public WorldScript
{
public:
    BotXpLockWorldScript()
        : WorldScript("BotXpLockWorldScript", {
            WORLDHOOK_ON_STARTUP,
            WORLDHOOK_ON_AFTER_CONFIG_LOAD
        })
    {
    }

    void OnStartup() override
    {
        sBotXpLockConfig.Load();
        sBotXpLockMgr.Initialize();
        sBotXpLockMgr.LoadFromDB();
    }

    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
        {
            sBotXpLockConfig.Load();
            sBotXpLockMgr.LoadFromDB();
        }
    }
};

class BotXpLockCommandScript : public AllCommandScript
{
public:
    BotXpLockCommandScript()
        : AllCommandScript("BotXpLockCommandScript")
    {
    }

    bool CanExecuteCommand(ChatHandler* handler, char const* command, char const* args) override
    {
        if (std::strcmp(command, "botxplock") != 0 && std::strcmp(command, "xplock") != 0)
            return true;

        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
        {
            handler->SendSysMessage("This command can only be used by in-game players.");
            return false;
        }

        sBotXpLockMgr.HandleCommand(handler, player, args ? args : "");
        return false;
    }
};

class BotXpLockServerScript : public ServerScript
{
public:
    BotXpLockServerScript()
        : ServerScript("BotXpLockServerScript", { SERVERHOOK_CAN_PACKET_RECEIVE })
    {
    }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        if (!sBotXpLockConfig.IsEnabled() || !session || !session->GetPlayer())
            return true;

        if (session->IsHeadless())
            return true;

        if (packet.GetOpcode() == CMSG_MESSAGECHAT)
        {
            try
            {
                WorldPacket copy = packet;
                copy.rpos(0);

                uint32 type = 0;
                uint32 language = 0;
                copy >> type >> language;

                if (language == LANG_ADDON)
                {
                    std::string msg;
                    if (type == CHAT_MSG_WHISPER)
                    {
                        std::string to;
                        copy >> to >> msg;
                    }
                    else if (type == CHAT_MSG_CHANNEL)
                    {
                        std::string channel;
                        copy >> channel >> msg;
                    }
                    else
                    {
                        copy >> msg;
                    }

                    std::string const prefixToken = std::string(BotXpLock::ADDON_PREFIX) + "\t";
                    if (msg.rfind(prefixToken, 0) == 0)
                    {
                        Player* sender = session->GetPlayer();
                        std::string payload = msg.substr(prefixToken.length());
                        sBotXpLockMgr.HandleAddonMessage(sender, payload);
                        return false; // Handled! Drop packet so chat handler does not error
                    }
                }
            }
            catch (...)
            {
            }
        }
        return true;
    }
};
} // anonymous namespace

void AddBotXpLockScripts()
{
    new BotXpLockPlayerScript();
    new BotXpLockWorldScript();
    new BotXpLockCommandScript();
    new BotXpLockServerScript();
}
