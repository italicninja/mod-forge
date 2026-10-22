/*
 * This file is part of the mod-forge module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include "Creature.h"
#include "ForgeMgr.h"
#include "LootMgr.h"
#include "MiscScript.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "WorldScript.h"

// Hooks into the core (generic ones from core patch 0001-generated-item-hooks); the logic is ForgeMgr's.
namespace
{
    ItemTemplate const* LookupGeneratedTemplate(uint32 entry) { return sForgeMgr->GetTemplate(entry); }

    class ForgeWorldScript : public WorldScript
    {
    public:
        ForgeWorldScript() : WorldScript("ForgeWorldScript", {WORLDHOOK_ON_AFTER_ITEM_TEMPLATES_LOAD, WORLDHOOK_ON_UPDATE}) { }

        // Right after item templates, before auctions, guilds and logins (they delete items whose template is missing)
        void OnAfterItemTemplatesLoad() override
        {
            sObjectMgr->SetGeneratedItemTemplateLookup(&LookupGeneratedTemplate);
            sForgeMgr->Load();
        }

        // World thread, after the (synchronous) map update: recycling
        void OnUpdate(uint32 diff) override { sForgeMgr->Update(diff); }
    };

    class ForgeMiscScript : public MiscScript
    {
    public:
        ForgeMiscScript() : MiscScript("ForgeMiscScript", {MISCHOOK_ON_AFTER_LOOT_TEMPLATE_PROCESS}) { }

        // Map threads, at the end of Loot::FillLoot's template pass. A creature's loot carries its GUID
        // (sourceWorldObjectGUID, set when the creature enters the world).
        void OnAfterLootTemplateProcess(Loot* loot, LootTemplate const* /*tab*/, LootStore const& store, Player* lootOwner,
            bool /*personal*/, bool /*noEmptyError*/, uint16 /*lootMode*/) override
        {
            if (!loot || !lootOwner)
                return;
            WorldObject* source = nullptr;
            if (&store == &LootTemplates_Creature && loot->sourceWorldObjectGUID.IsCreature())
                source = ObjectAccessor::GetCreature(*lootOwner, loot->sourceWorldObjectGUID);
            sForgeMgr->OnLootFilled(*loot, store, lootOwner, source);
        }
    };

    // Talent affixes: the bonuses follow the equipment. Every equip and unequip (also a destroyed or swapped item)
    // goes through SetVisibleItemSlot; loading the inventory happens outside the world, so login sets them all.
    class ForgePlayerScript : public PlayerScript
    {
    public:
        ForgePlayerScript() : PlayerScript("ForgePlayerScript", {PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_AFTER_SET_VISIBLE_ITEM_SLOT,
            PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT}) { }

        using PlayerScript::OnPlayerCanUseChat;

        void OnPlayerLogin(Player* player) override { sForgeMgr->UpdateTalentBonuses(player, 0xFF, nullptr, true); }

        // The ForgeTalents addon asks for the bonuses (after a /reload) with an addon whisper to the player itself.
        bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 language, std::string& msg, Player* receiver) override
        {
            if (language != LANG_ADDON || receiver != player || !msg.starts_with(std::string(FORGE_TALENT_PREFIX) + "\t"))
                return true;
            if (sForgeMgr->TalentAffixesEnabled())
                sForgeMgr->SendTalentBonuses(player);
            return false;
        }

        void OnPlayerAfterSetVisibleItemSlot(Player* player, uint8 slot, Item* item) override
        {
            if (sForgeMgr->TalentAffixesEnabled() || !player->GetTalentRankBonuses().empty())
                sForgeMgr->UpdateTalentBonuses(player, slot, item);
        }
    };
}

void AddForgeScripts()
{
    new ForgeWorldScript();
    new ForgeMiscScript();
    new ForgePlayerScript();
}
