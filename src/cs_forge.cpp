/*
 * This file is part of the mod-forge module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 * Formerly core patches 0001-item-affixes and 0006-item-affix-spells (wowproject).
 */

#include "Chat.h"
#include "CommandScript.h"
#include "ForgeMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "RBAC.h"

using namespace Acore::ChatCommands;

// GM tools for PoE-style generated items (wowproject docs/loot-affixes.md): .forge, or .affix.
class forge_commandscript : public CommandScript
{
public:
    forge_commandscript() : CommandScript("forge_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable affixCommandTable =
        {
            { "roll",      HandleAffixRollCommand,      rbac::RBAC_PERM_COMMAND_ADDITEM, Console::No },
            { "legendary", HandleAffixLegendaryCommand, rbac::RBAC_PERM_COMMAND_ADDITEM, Console::No },
            { "unique",    HandleAffixUniqueCommand,    rbac::RBAC_PERM_COMMAND_ADDITEM, Console::No },
            { "info",      HandleAffixInfoCommand,      rbac::RBAC_PERM_COMMAND_ADDITEM, Console::Yes },
            { "reroll",    HandleAffixRerollCommand,    rbac::RBAC_PERM_COMMAND_ADDITEM, Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "forge", affixCommandTable },
            { "affix", affixCommandTable }, // the name before mod-forge
        };
        return commandTable;
    }

    // .forge roll <base entry> [item level] [prefixes] [suffixes]: roll a generated item into your
    // bags. Prefix/suffix counts (1-3) are random when omitted or 0.
    static bool HandleAffixRollCommand(ChatHandler* handler, uint32 baseEntry, Optional<uint8> itemLevel,
        Optional<uint8> prefixes, Optional<uint8> suffixes)
    {
        Player* player = handler->GetSession()->GetPlayer();
        ItemTemplate const* base = sObjectMgr->GetItemTemplate(baseEntry);
        if (!base || !ForgeMgr::GetItemMask(base))
        {
            handler->SendErrorMessage("Item {} is not a weapon or armor piece that can roll affixes.", baseEntry);
            return false;
        }

        if (prefixes.value_or(0) > 3 || suffixes.value_or(0) > 3)
        {
            handler->SendErrorMessage("Prefix and suffix counts go up to 3.");
            return false;
        }

        uint32 entry = sForgeMgr->Generate(base, itemLevel.value_or(player->GetLevel()), true,
            prefixes.value_or(0), suffixes.value_or(0));
        if (!entry)
        {
            handler->SendErrorMessage("Nothing rolled for item {}: no matching Item.dbc pool or no affixes at that level.", baseEntry);
            return false;
        }

        if (!player->StoreNewItemInBestSlots(entry, 1))
        {
            handler->SendErrorMessage("Rolled entry {} but your bags are full.", entry);
            return false;
        }

        handler->PSendSysMessage("Rolled entry {}: {}", entry, sObjectMgr->GetItemTemplate(entry)->Name1);
        return true;
    }

    static bool GiveItem(ChatHandler* handler, Player* player, uint32 entry)
    {
        if (!player->StoreNewItemInBestSlots(entry, 1))
        {
            handler->SendErrorMessage("Item {} is ready but your bags are full (or you already carry this Unique).", entry);
            return false;
        }
        handler->PSendSysMessage("Got entry {}: {}", entry, sObjectMgr->GetItemTemplate(entry)->Name1);
        return true;
    }

    // .forge legendary <base entry> [item level]: roll a Legendary (orange, 4-6 affixes, epic budget x
    // Forge.LegendaryBudget) from any affixable base.
    static bool HandleAffixLegendaryCommand(ChatHandler* handler, uint32 baseEntry, Optional<uint8> itemLevel)
    {
        Player* player = handler->GetSession()->GetPlayer();
        ItemTemplate const* base = sObjectMgr->GetItemTemplate(baseEntry);
        if (!base || !ForgeMgr::GetItemMask(base))
        {
            handler->SendErrorMessage("Item {} is not a weapon or armor piece that can roll affixes.", baseEntry);
            return false;
        }

        uint32 entry = sForgeMgr->Generate(base, itemLevel.value_or(player->GetLevel()), true, 0, 0, ITEM_QUALITY_LEGENDARY);
        if (!entry)
        {
            handler->SendErrorMessage("Nothing rolled for item {}: no matching Item.dbc pool or no affixes at that level.", baseEntry);
            return false;
        }
        return GiveItem(handler, player, entry);
    }

    // .forge unique [key]: list the Uniques, or give yourself one.
    static bool HandleAffixUniqueCommand(ChatHandler* handler, Optional<std::string> key)
    {
        if (!key)
        {
            for (UniqueItem const* unique : sForgeMgr->GetUniques())
                handler->PSendSysMessage("  {} = {} (entry {}, drops from creature {} at {}%)", unique->Key, unique->Name,
                    unique->Entry, unique->CreatureEntry, unique->Chance);
            return true;
        }

        UniqueItem const* unique = sForgeMgr->GetUnique(*key);
        if (!unique)
        {
            handler->SendErrorMessage("No Unique '{}'. Use .forge unique to list them.", *key);
            return false;
        }
        return GiveItem(handler, handler->GetSession()->GetPlayer(), unique->Entry);
    }

    // .forge reroll <entry>: what the Orb of Upheaval does, without the orb or its quality rule, on a generated item
    // you carry or wear.
    static bool HandleAffixRerollCommand(ChatHandler* handler, uint32 entry)
    {
        Player* player = handler->GetSession()->GetPlayer();
        Item* item = player->GetItemByEntry(entry);
        if (!item)
        {
            handler->SendErrorMessage("You carry no item {}.", entry);
            return false;
        }
        std::string before = item->GetTemplate()->Name1;
        if (!sForgeMgr->RerollItem(player, item))
        {
            handler->SendErrorMessage("Item {} is not a generated item, or nothing rolled.", entry);
            return false;
        }
        handler->PSendSysMessage("Rerolled {}: {} -> {}", entry, before, item->GetTemplate()->Name1);
        return true;
    }

    // .forge info <entry>: show what a generated item rolled.
    static bool HandleAffixInfoCommand(ChatHandler* handler, uint32 entry)
    {
        GeneratedItem const* item = sForgeMgr->GetGenerated(entry);
        if (!item)
        {
            handler->SendErrorMessage("Entry {} is not a generated item.", entry);
            return false;
        }

        handler->PSendSysMessage("{}: base {}, quality {}, item level {} (dropped at {}), {} affixes", item->Template->Name1,
            item->BaseEntry, item->Template->Quality, item->Template->ItemLevel, item->ItemLevel, item->Rolls.size());
        for (ItemAffixRoll const& roll : item->Rolls)
        {
            ItemAffix const* affix = sForgeMgr->GetAffix(roll.AffixId);
            ItemAffixTier const* tier = affix ? affix->GetTier(roll.Tier) : nullptr;
            handler->PSendSysMessage("  {} {} T{} = {}{}", affix && affix->Kind == ITEM_AFFIX_PREFIX ? "prefix" : "suffix",
                tier ? tier->Name : "?", roll.Tier, roll.Value, roll.Grade ? " (grade " + std::to_string(roll.Grade) + ")" : "");
        }
        return true;
    }
};

void AddSC_forge_commandscript()
{
    new forge_commandscript();
}
