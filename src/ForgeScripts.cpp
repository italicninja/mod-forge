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
}

void AddForgeScripts()
{
    new ForgeWorldScript();
    new ForgeMiscScript();
}
