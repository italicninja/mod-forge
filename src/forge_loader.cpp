/*
 * This file is part of the mod-forge module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include "Chat.h"
#include "Player.h"
#include "PlayerScript.h"
#include "WorldSession.h"

void AddForgeScripts();
void AddSC_forge_commandscript();
void AddSC_forge_currency();

// The version this source is tagged with in https://github.com/italicninja/mod-forge (bump it with each tag).
// addon/pack.sh stamps it into the ForgeTalents addon's .toc too.
constexpr char const* FORGE_VERSION = "v0.1.0";

namespace
{
    // Tells each real player at login that the realm runs this module, like mod-playerbots does. GPL-2.0: the link
    // must lead to the module's public source.
    class ForgeLoginScript : public PlayerScript
    {
    public:
        ForgeLoginScript() : PlayerScript("ForgeLoginScript", {PLAYERHOOK_ON_LOGIN}) { }

        void OnPlayerLogin(Player* player) override
        {
            if (player->GetSession()->IsHeadless())
                return;
            std::string line = "|cff00ff00This server runs with |cff00ccffmod-forge|r |cffffd100";
            line += FORGE_VERSION;
            line += "|r |cffcccccchttps://github.com/italicninja/mod-forge|r";
            ChatHandler(player->GetSession()).SendSysMessage(line);
        }
    };
}

// Called by the core's module loader (Add<module name with '-' as '_'>Scripts).
void Addmod_forgeScripts()
{
    AddForgeScripts();
    AddSC_forge_commandscript();
    AddSC_forge_currency();
    new ForgeLoginScript();
}
