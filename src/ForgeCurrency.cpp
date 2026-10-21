/*
 * This file is part of the mod-forge module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include "Chat.h"
#include "ForgeMgr.h"
#include "Item.h"
#include "Player.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

// Crafting orbs (wowproject docs/forge-currency.md). Each orb is an item whose "Use:" spell is a copy of a stock
// enchanting-scroll spell (item-target cursor, enchant sparkle) with enchant 0: the stock effect does nothing, so the
// script does the work. Spells and items: tools/client-patch/forge-currency.json (client) and
// sql/world/2026_10_21_01_forge_currency.sql (server).

namespace
{
    // Generated item in the caster's own bags or equipment (not a trade slot), of at least minQuality.
    SpellCastResult CheckForgedTarget(Player* player, Item* item, uint8 minQuality)
    {
        if (!item || item->GetOwnerGUID() != player->GetGUID())
            return SPELL_FAILED_BAD_TARGETS;
        GeneratedItem const* generated = sForgeMgr->GetGenerated(item->GetEntry());
        if (!generated || generated->Preview)
        {
            ChatHandler(player->GetSession()).SendNotification("Only forged items can be reshaped.");
            return SPELL_FAILED_DONT_REPORT;
        }
        if (item->GetTemplate()->Quality < minQuality)
        {
            ChatHandler(player->GetSession()).SendNotification("That item is not powerful enough.");
            return SPELL_FAILED_DONT_REPORT;
        }
        return SPELL_CAST_OK;
    }
}

// Orb of Upheaval (PoE Chaos Orb): reroll every affix of a Rare, Epic or Legendary forged item.
class spell_forge_orb_upheaval : public SpellScript
{
    PrepareSpellScript(spell_forge_orb_upheaval);

    SpellCastResult CheckCast()
    {
        Player* player = GetCaster()->ToPlayer();
        if (!player)
            return SPELL_FAILED_BAD_TARGETS;
        return CheckForgedTarget(player, GetExplTargetItem(), ITEM_QUALITY_RARE);
    }

    void HandleReroll(SpellEffIndex effIndex)
    {
        PreventHitDefaultEffect(effIndex);
        Player* player = GetCaster()->ToPlayer();
        Item* item = GetHitItem();
        if (!player || !item)
            return;

        std::string before = item->GetTemplate()->Name1;
        if (sForgeMgr->RerollItem(player, item))
            ChatHandler(player->GetSession()).PSendSysMessage("Your {} is reforged into {}.", before, item->GetTemplate()->Name1);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_forge_orb_upheaval::CheckCast);
        OnEffectHitTarget += SpellEffectFn(spell_forge_orb_upheaval::HandleReroll, EFFECT_0, SPELL_EFFECT_ENCHANT_ITEM);
    }
};

void AddSC_forge_currency()
{
    RegisterSpellScript(spell_forge_orb_upheaval);
}
