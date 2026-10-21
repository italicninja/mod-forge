/*
 * This file is part of the mod-forge module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include "Chat.h"
#include "DatabaseEnv.h"
#include "ForgeMgr.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "SpellMgr.h"
#include "SpellScriptLoader.h"
#include <algorithm>
#include <set>

// Crafting orbs (wowproject docs/forge-currency.md). Each orb is an item whose "Use:" spell is a copy of a stock
// enchanting-scroll spell (item-target cursor, enchant sparkle) with enchant 0: the stock effect does nothing, so
// spell_forge_orb does the work, by the orb's kind in forge_currency. Items, spells and forge_currency rows are
// generated from wowproject tools/client-patch/forge-currency.json (client patch and
// sql/world/2026_10_21_01_forge_currency.sql).

namespace
{
    ForgeOrbKind ParseOrbKind(std::string const& kind)
    {
        static char const* const names[MAX_FORGE_ORB] = { "transmute", "alteration", "augment", "scour", "alchemy",
            "chance", "chaos", "regal", "annul", "vaal", "divine", "exalt", "essence" };
        for (uint8 i = 0; i < MAX_FORGE_ORB; ++i)
            if (kind == names[i])
                return ForgeOrbKind(i);
        return MAX_FORGE_ORB;
    }

    bool IsSpellAffix(ItemAffix const* affix)
    {
        return affix && (affix->Effect == ITEM_AFFIX_EFFECT_EQUIP_SPELL || affix->Effect == ITEM_AFFIX_EFFECT_HIT_SPELL);
    }

    // A Unique's (or Unique copy's) stat values, rerolled by factor (spells kept).
    int32 ScaleValue(int32 value, float factor)
    {
        return std::max<int32>(1, int32(std::lround(value * factor)));
    }

    constexpr uint32 CHANCE_UNIQUE_PCT = 2;    // Gambler's Coin: the base's Unique, if it has one
    constexpr uint32 CHANCE_LEGENDARY_PCT = 3;
    constexpr uint32 CHANCE_EPIC_PCT = 15;
    constexpr uint32 CHANCE_RARE_PCT = 30;
    constexpr float UNIQUE_REROLL_MIN = 0.8f;  // Tear of the Naaru on a Unique: each value x 0.8-1.2
    constexpr float UNIQUE_REROLL_MAX = 1.2f;
}

void ForgeMgr::LoadCurrency()
{
    _orbs.clear();
    //                                                   0         1           2     3    4
    QueryResult result = WorldDatabase.Query("SELECT spell_id, item_entry, kind, arg, drop_weight FROM forge_currency");
    if (!result)
        return;
    do
    {
        Field* fields = result->Fetch();
        ForgeOrb orb;
        orb.SpellId = fields[0].Get<uint32>();
        orb.ItemEntry = fields[1].Get<uint32>();
        orb.Kind = ParseOrbKind(fields[2].Get<std::string>());
        orb.Arg = fields[3].Get<int32>();
        orb.DropWeight = fields[4].Get<uint32>();
        if (orb.Kind == MAX_FORGE_ORB || !sObjectMgr->GetItemTemplate(orb.ItemEntry) || !sSpellMgr->GetSpellInfo(orb.SpellId) ||
            (orb.Kind == FORGE_ORB_ESSENCE && !GetAffix(uint32(orb.Arg))))
        {
            LOG_ERROR("sql.sql", "Table `forge_currency`: spell {} (item {}) has an unknown kind '{}', item, spell or affix; skipped.",
                orb.SpellId, orb.ItemEntry, fields[2].Get<std::string>());
            continue;
        }
        _orbs[orb.SpellId] = orb;
    } while (result->NextRow());
}

ForgeOrb const* ForgeMgr::GetOrb(uint32 spellId) const
{
    auto itr = _orbs.find(spellId);
    return itr != _orbs.end() ? &itr->second : nullptr;
}

uint32 ForgeMgr::PickOrbEntry() const
{
    uint32 total = 0;
    for (auto const& [spellId, orb] : _orbs)
        total += orb.DropWeight;
    if (!total)
        return 0;
    uint32 roll = urand(1, total);
    for (auto const& [spellId, orb] : _orbs)
    {
        if (roll <= orb.DropWeight)
            return orb.ItemEntry;
        roll -= orb.DropWeight;
    }
    return 0;
}

void ForgeMgr::RebuildLocked(GeneratedItem& item)
{
    ItemTemplate const* base = sObjectMgr->GetItemTemplate(item.BaseEntry);
    ItemAffixPool* pool = FindPool(item.Entry);
    if (!base || !pool)
        return;
    // Keep the old template like a recycled one: pointers to it may still be held
    _retired.push_back(std::move(item.Template));
    item.Template = MakeTemplate(item, *base, pool->DisplayId);
    SaveGenerated(item, *pool);
}

std::vector<ItemAffix const*> ForgeMgr::AffixCandidates(std::vector<ItemAffixRoll> const& rolls, ItemTemplate const* base,
    uint8 itemLevel, uint32 onlyAffix) const
{
    uint32 kindCount[2] = { 0, 0 };
    std::set<uint32> usedGroups;
    for (ItemAffixRoll const& roll : rolls)
        if (ItemAffix const* affix = GetAffix(roll.AffixId))
        {
            ++kindCount[affix->Kind];
            usedGroups.insert(affix->Group);
        }

    uint32 mask = GetItemMask(base);
    std::vector<ItemAffix const*> out;
    for (auto const& [id, affix] : _affixes)
    {
        if (onlyAffix && id != onlyAffix)
            continue;
        if (kindCount[affix.Kind] >= 3 || !(affix.ItemMask & mask) || (!onlyAffix && !affix.Weight) ||
            usedGroups.count(affix.Group) || affix.Tiers.front().MinItemLevel > itemLevel)
            continue;
        if (affix.Effect == ITEM_AFFIX_EFFECT_EQUIP_SPELL && affix.EffectArg)
        {
            uint32 type = base->InventoryType == INVTYPE_ROBE ? INVTYPE_CHEST : base->InventoryType;
            if (type >= 32 || !(uint32(affix.EffectArg) & (1u << type)))
                continue;
        }
        out.push_back(&affix);
    }
    return out;
}

bool ForgeMgr::AddRoll(std::vector<ItemAffixRoll>& rolls, ItemTemplate const* base, uint8 itemLevel, uint32 onlyAffix,
    uint8 grade) const
{
    std::vector<ItemAffix const*> candidates = AffixCandidates(rolls, base, itemLevel, onlyAffix);
    if (candidates.empty())
        return false;

    ItemAffix const* affix = candidates.front();
    if (!onlyAffix)
    {
        uint32 total = 0;
        for (ItemAffix const* c : candidates)
            total += c->Weight;
        uint32 pick = urand(1, total);
        for (ItemAffix const* c : candidates)
        {
            if (pick <= c->Weight)
            {
                affix = c;
                break;
            }
            pick -= c->Weight;
        }
    }

    // Same naming rule as Roll in grade mode: grade 1 = the highest tier the item level unlocks.
    std::vector<ItemAffixTier const*> unlocked;
    for (ItemAffixTier const& t : affix->Tiers)
        if (t.MinItemLevel <= itemLevel)
            unlocked.push_back(&t);
    ItemAffixRoll roll { affix->Id, 0, 0, uint8(IsSpellAffix(affix) ? 0 : grade ? grade : RollGrade()) };
    std::size_t below = roll.Grade ? std::min<std::size_t>(roll.Grade - 1, unlocked.size() - 1) : 0;
    ItemAffixTier const* tier = unlocked[unlocked.size() - 1 - below];
    roll.Tier = tier->Tier;
    SetGradeValue(roll, *affix, *tier, GreenBudget(base));
    rolls.push_back(roll);
    return true;
}

std::vector<ItemAffixRoll> ForgeMgr::RollCount(ItemTemplate const* base, uint8 itemLevel, uint32 count, uint32 onlyAffix) const
{
    std::vector<ItemAffixRoll> rolls;
    if (onlyAffix && count)
    {
        AddRoll(rolls, base, itemLevel, onlyAffix, 1);
        --count;
    }
    for (uint32 n = 0; n < count; ++n)
        if (!AddRoll(rolls, base, itemLevel))
            break;
    return rolls;
}

void ForgeMgr::RerollValue(ItemAffixRoll& roll, ItemTemplate const* base) const
{
    ItemAffix const* affix = GetAffix(roll.AffixId);
    ItemAffixTier const* tier = affix ? affix->GetTier(roll.Tier) : nullptr;
    if (!tier || IsSpellAffix(affix))
        return;
    // A roll from before grades gets one now; SetGradeValue keeps an existing grade.
    SetGradeValue(roll, *affix, *tier, GreenBudget(base));
}

std::string ForgeMgr::CheckOrb(ForgeOrb const& orb, Player* player, Item* item) const
{
    if (!item || item->GetOwnerGUID() != player->GetGUID())
        return "You can only reshape your own items.";

    uint32 entry = item->GetEntry();
    GeneratedItem const* generated = GetGenerated(entry);
    if (generated && generated->Preview)
        generated = nullptr;
    auto shared = _uniqueByEntry.find(entry);
    bool sharedUnique = shared != _uniqueByEntry.end();
    bool uniqueCopy = generated && !generated->UniqueKey.empty();
    bool forged = generated && !uniqueCopy;
    ItemTemplate const* proto = item->GetTemplate();
    bool plain = !generated && !sharedUnique && IsEligible(proto);

    if (generated && (generated->Flags & GENERATED_ITEM_FLAG_CORRUPTED))
        return "Corrupted items cannot be changed.";

    uint8 quality = proto->Quality;
    auto hasRoom = [&](uint8 forQuality)
    {
        ItemTemplate const* base = sObjectMgr->GetItemTemplate(generated->BaseEntry);
        return base && generated->Rolls.size() < _countMax[std::min<uint8>(forQuality, ITEM_QUALITY_LEGENDARY)] &&
            !AffixCandidates(generated->Rolls, base, generated->ItemLevel).empty();
    };

    switch (orb.Kind)
    {
        case FORGE_ORB_TRANSMUTE:
        case FORGE_ORB_ALCHEMY:
        case FORGE_ORB_CHANCE:
            return plain ? "" : "Only a plain weapon or piece of armor can be awakened.";
        case FORGE_ORB_ESSENCE:
        {
            if (!plain && !forged)
                return "Only a plain or forged weapon or piece of armor can take an essence.";
            ItemAffix const* affix = GetAffix(uint32(orb.Arg));
            if (!affix || !(affix->ItemMask & GetItemMask(proto)))
                return "That essence cannot bind to this item.";
            return "";
        }
        case FORGE_ORB_ALTERATION:
            if (!forged)
                return "Only forged items can be reshaped.";
            return quality <= ITEM_QUALITY_UNCOMMON ? "" : "Only an Uncommon forged item can be shifted.";
        case FORGE_ORB_AUGMENT:
            if (!forged)
                return "Only forged items can be reshaped.";
            if (quality > ITEM_QUALITY_UNCOMMON)
                return "Only an Uncommon forged item can be augmented.";
            return hasRoom(quality) ? "" : "That item has no room for another affix.";
        case FORGE_ORB_CHAOS:
            if (!forged)
                return "Only forged items can be reshaped.";
            return quality >= ITEM_QUALITY_RARE ? "" : "Only a Rare, Epic or Legendary forged item can be upheaved.";
        case FORGE_ORB_REGAL:
            if (!forged)
                return "Only forged items can be reshaped.";
            if (quality != ITEM_QUALITY_UNCOMMON && quality != ITEM_QUALITY_RARE)
                return "Only an Uncommon or Rare forged item can be raised.";
            return hasRoom(quality + 1) ? "" : "That item has no room for another affix.";
        case FORGE_ORB_EXALT:
            if (!forged)
                return "Only forged items can be reshaped.";
            if (quality < ITEM_QUALITY_RARE)
                return "Only a Rare, Epic or Legendary forged item can be exalted.";
            return hasRoom(quality) ? "" : "That item has no room for another affix.";
        case FORGE_ORB_ANNUL:
            if (!forged)
                return "Only forged items can be reshaped.";
            return generated->Rolls.empty() ? "That item has no affix left to remove." : "";
        case FORGE_ORB_SCOUR:
            return forged ? "" : "Only forged items can be scoured.";
        case FORGE_ORB_DIVINE:
            if (sharedUnique || uniqueCopy)
                return "";
            if (!forged)
                return "Only forged items and Uniques can be reweighed.";
            for (ItemAffixRoll const& roll : generated->Rolls)
                if (!IsSpellAffix(GetAffix(roll.AffixId)))
                    return "";
            return "That item has no values to reweigh.";
        case FORGE_ORB_VAAL:
            return forged || uniqueCopy || sharedUnique ? "" : "Only forged items and Uniques can be corrupted.";
        default:
            return "This orb does nothing.";
    }
}

namespace
{
    // Replace player's item (in bags or equipped) with a new item of entry in the same place, keeping enchants, gems,
    // durability and binding. Returns the new item, or nullptr.
    Item* ReplaceItem(Player* player, Item* old, uint32 entry)
    {
        uint8 bag = old->GetBagSlot();
        uint8 slot = old->GetSlot();
        uint16 pos = old->GetPos();
        bool equipped = old->IsEquipped();
        bool bound = old->IsSoulBound();
        uint32 durability = old->GetUInt32Value(ITEM_FIELD_DURABILITY);
        struct Enchant { uint32 Id; uint32 Duration; uint32 Charges; };
        Enchant enchants[MAX_ENCHANTMENT_SLOT] = {};
        for (uint8 e = PERM_ENCHANTMENT_SLOT; e < MAX_INSPECTED_ENCHANTMENT_SLOT; ++e)
            enchants[e] = { old->GetEnchantmentId(EnchantmentSlot(e)), old->GetEnchantmentDuration(EnchantmentSlot(e)),
                old->GetEnchantmentCharges(EnchantmentSlot(e)) };

        player->DestroyItem(bag, slot, true);

        Item* item = nullptr;
        if (equipped)
            item = player->EquipNewItem(pos, entry, true);
        else
        {
            ItemPosCountVec dest;
            if (player->CanStoreNewItem(bag, slot, dest, entry, 1) == EQUIP_ERR_OK ||
                player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, 1) == EQUIP_ERR_OK)
                item = player->StoreNewItem(dest, entry, true);
        }
        if (!item)
        {
            LOG_ERROR("module.forge", "Orb: could not put item {} back for {}; it is lost.", entry, player->GetName());
            return nullptr;
        }

        for (uint8 e = PERM_ENCHANTMENT_SLOT; e < MAX_INSPECTED_ENCHANTMENT_SLOT; ++e)
        {
            if (!enchants[e].Id)
                continue;
            item->SetEnchantment(EnchantmentSlot(e), enchants[e].Id, enchants[e].Duration, enchants[e].Charges);
            if (equipped)
                player->ApplyEnchantment(item, EnchantmentSlot(e), true);
            if (e == TEMP_ENCHANTMENT_SLOT && enchants[e].Duration)
                player->AddEnchantmentDuration(item, EnchantmentSlot(e), enchants[e].Duration);
        }
        item->SetUInt32Value(ITEM_FIELD_DURABILITY, std::min(durability, item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY)));
        if (bound)
            item->SetBinding(true);
        item->SetState(ITEM_CHANGED, player);
        return item;
    }
}

void ForgeMgr::UseOrb(ForgeOrb const& orb, Player* player, Item* item)
{
    uint32 entry = item->GetEntry();
    std::string before = item->GetTemplate()->Name1;
    ChatHandler chat(player->GetSession());

    // Orbs that give the item a new entry (plain <-> forged, a Unique's own copy) replace it after the cast, on the
    // player's next update, so the spell never holds a destroyed item.
    auto replaceLater = [player, guid = item->GetGUID(), before](uint32 newEntry, std::string message)
    {
        ObjectGuid playerGuid = player->GetGUID();
        player->m_Events.AddEventAtOffset([playerGuid, guid, newEntry, before, message]()
        {
            Player* owner = ObjectAccessor::FindPlayer(playerGuid);
            Item* target = owner ? owner->GetItemByGuid(guid) : nullptr;
            if (!target)
                return;
            if (Item* item = ReplaceItem(owner, target, newEntry))
            {
                sForgeMgr->SendTemplate(owner, newEntry);
                ChatHandler(owner->GetSession()).PSendSysMessage(message.c_str(), before, item->GetTemplate()->Name1);
            }
        }, 0ms);
    };

    // In-place changes to a forged item or a Unique copy: an equipped item loses the old stats and gains the new.
    auto changeInPlace = [&](auto&& change) -> bool
    {
        bool equipped = item->IsEquipped();
        if (equipped)
            player->_ApplyItemMods(item, item->GetSlot(), false);
        bool changed = false;
        {
            std::unique_lock lock(_lock);
            auto itr = _generated.find(entry);
            if (itr != _generated.end())
            {
                changed = change(*itr->second);
                if (changed)
                    RebuildLocked(*itr->second);
            }
        }
        if (equipped)
            player->_ApplyItemMods(item, item->GetSlot(), true);
        if (changed)
            SendTemplate(player, entry);
        return changed;
    };

    auto qualityCount = [&](uint8 quality, bool most) -> uint32
    {
        quality = std::min<uint8>(quality, ITEM_QUALITY_LEGENDARY);
        return std::max<uint32>(1, most ? _countMax[quality] : _countMin[quality]);
    };

    // A new forged item from a plain one.
    auto forge = [&](uint8 quality, uint32 count, uint32 onlyAffix, char const* message)
    {
        ItemTemplate const* base = item->GetTemplate();
        uint8 itemLevel = uint8(std::min<uint32>(std::max<uint32>(base->ItemLevel, 1), 255));
        std::vector<ItemAffixRoll> rolls = RollCount(base, itemLevel, count, onlyAffix);
        uint32 newEntry = rolls.empty() ? 0 : CreateGenerated(base, itemLevel, quality, std::move(rolls));
        if (!newEntry)
        {
            chat.SendNotification("Nothing could be forged from that item.");
            return;
        }
        replaceLater(newEntry, message);
    };

    std::string message;
    switch (orb.Kind)
    {
        case FORGE_ORB_TRANSMUTE:
        case FORGE_ORB_ALCHEMY:
        {
            uint8 quality = uint8(std::min<uint32>(item->GetTemplate()->Quality, ITEM_QUALITY_LEGENDARY));
            forge(quality, qualityCount(quality, orb.Kind == FORGE_ORB_ALCHEMY), 0, "Your {} awakens as {}.");
            return;
        }
        case FORGE_ORB_CHANCE:
        {
            ItemTemplate const* base = item->GetTemplate();
            uint32 roll = urand(1, 100);
            for (auto const& unique : _uniques)
                if (unique->Template && unique->BaseEntry == base->ItemId && roll <= CHANCE_UNIQUE_PCT &&
                    !player->HasItemCount(unique->Entry, 1, true))
                {
                    replaceLater(unique->Entry, "Fortune smiles: your {} becomes {}!");
                    return;
                }
            uint8 quality = roll <= CHANCE_LEGENDARY_PCT ? ITEM_QUALITY_LEGENDARY
                : roll <= CHANCE_EPIC_PCT ? ITEM_QUALITY_EPIC
                : roll <= CHANCE_RARE_PCT ? ITEM_QUALITY_RARE
                : uint8(std::clamp<uint32>(base->Quality, ITEM_QUALITY_UNCOMMON, ITEM_QUALITY_EPIC));
            quality = std::max<uint8>(quality, uint8(std::min<uint32>(base->Quality, ITEM_QUALITY_LEGENDARY)));
            forge(quality, urand(qualityCount(quality, false), qualityCount(quality, true)), 0, "The coin lands: your {} becomes {}.");
            return;
        }
        case FORGE_ORB_ESSENCE:
        {
            uint32 affixId = uint32(orb.Arg);
            if (!GetGenerated(entry))
            {
                uint8 quality = uint8(std::min<uint32>(item->GetTemplate()->Quality, ITEM_QUALITY_LEGENDARY));
                forge(quality, urand(qualityCount(quality, false), qualityCount(quality, true)), affixId,
                    "The essence binds: your {} becomes {}.");
                return;
            }
            changeInPlace([&](GeneratedItem& g)
            {
                ItemTemplate const* base = sObjectMgr->GetItemTemplate(g.BaseEntry);
                uint8 quality = std::min<uint8>(g.Quality, ITEM_QUALITY_LEGENDARY);
                std::vector<ItemAffixRoll> rolls = RollCount(base, g.ItemLevel,
                    urand(qualityCount(quality, false), qualityCount(quality, true)), affixId);
                if (rolls.empty())
                    return false;
                g.Rolls = std::move(rolls);
                return true;
            });
            message = "The essence binds: your {} becomes {}.";
            break;
        }
        case FORGE_ORB_ALTERATION:
        case FORGE_ORB_CHAOS:
        {
            bool equipped = item->IsEquipped();
            if (equipped)
                player->_ApplyItemMods(item, item->GetSlot(), false);
            bool rerolled = Reroll(entry);
            if (equipped)
                player->_ApplyItemMods(item, item->GetSlot(), true);
            if (rerolled)
                SendTemplate(player, entry);
            message = "Your {} is reforged into {}.";
            break;
        }
        case FORGE_ORB_AUGMENT:
        case FORGE_ORB_EXALT:
            changeInPlace([&](GeneratedItem& g)
            {
                return AddRoll(g.Rolls, sObjectMgr->GetItemTemplate(g.BaseEntry), g.ItemLevel);
            });
            message = "Your {} grows into {}.";
            break;
        case FORGE_ORB_REGAL:
            changeInPlace([&](GeneratedItem& g)
            {
                g.Quality = uint8(std::min<uint32>(g.Quality, ITEM_QUALITY_EPIC - 1) + 1);
                AddRoll(g.Rolls, sObjectMgr->GetItemTemplate(g.BaseEntry), g.ItemLevel);
                return true;
            });
            message = "Your {} is raised to {}.";
            break;
        case FORGE_ORB_ANNUL:
            changeInPlace([&](GeneratedItem& g)
            {
                if (g.Rolls.empty())
                    return false;
                g.Rolls.erase(g.Rolls.begin() + urand(0, g.Rolls.size() - 1));
                return true;
            });
            message = "The Void takes from your {}. It is now {}.";
            break;
        case FORGE_ORB_SCOUR:
            if (GeneratedItem const* g = GetGenerated(entry))
                replaceLater(g->BaseEntry, "Your {} is scoured back to {}.");
            return;
        case FORGE_ORB_DIVINE:
        case FORGE_ORB_VAAL:
        {
            bool vaal = orb.Kind == FORGE_ORB_VAAL;
            uint32 outcome = vaal ? urand(0, 3) : 1;

            // A Unique at its shared entry gets its own copy first.
            if (auto shared = _uniqueByEntry.find(entry); shared != _uniqueByEntry.end())
            {
                UniqueItem const& unique = *shared->second;
                ItemTemplate const* base = sObjectMgr->GetItemTemplate(unique.BaseEntry);
                std::vector<ItemAffixRoll> rolls;
                for (std::size_t i = 0; i < unique.Stats.size(); ++i)
                    rolls.push_back({ 0, uint8(i), std::get<2>(unique.Stats[i]) });
                std::vector<std::size_t> stats;
                for (std::size_t i = 0; i < unique.Stats.size(); ++i)
                    if (std::get<0>(unique.Stats[i]) != ITEM_AFFIX_EFFECT_EQUIP_SPELL && std::get<0>(unique.Stats[i]) != ITEM_AFFIX_EFFECT_HIT_SPELL)
                        stats.push_back(i);
                if (outcome == 1)
                    for (std::size_t i : stats)
                        rolls[i].Value = ScaleValue(rolls[i].Value, frand(UNIQUE_REROLL_MIN, UNIQUE_REROLL_MAX));
                else if ((outcome == 2 || outcome == 3) && !stats.empty())
                {
                    std::size_t i = stats[urand(0, stats.size() - 1)];
                    rolls[i].Value = ScaleValue(rolls[i].Value, outcome == 2 ? 1.25f : 0.75f);
                }
                uint32 newEntry = base ? CreateGenerated(base, unique.ItemLevel, ITEM_QUALITY_ARTIFACT, std::move(rolls), unique.Key,
                    vaal ? GENERATED_ITEM_FLAG_CORRUPTED : 0) : 0;
                if (!newEntry)
                {
                    chat.SendNotification("Nothing happens.");
                    return;
                }
                replaceLater(newEntry, vaal ? "Felfire consumes your {}. It is corrupted: {}." : "The Light weighs your {} anew: {}.");
                return;
            }

            changeInPlace([&](GeneratedItem& g)
            {
                ItemTemplate const* base = sObjectMgr->GetItemTemplate(g.BaseEntry);
                if (vaal)
                    g.Flags |= GENERATED_ITEM_FLAG_CORRUPTED;
                if (!g.UniqueKey.empty())
                {
                    UniqueItem const* unique = FindUniqueDef(g.UniqueKey);
                    std::vector<ItemAffixRoll*> stats;
                    for (ItemAffixRoll& roll : g.Rolls)
                        if (unique && roll.Tier < unique->Stats.size() &&
                            std::get<0>(unique->Stats[roll.Tier]) != ITEM_AFFIX_EFFECT_EQUIP_SPELL &&
                            std::get<0>(unique->Stats[roll.Tier]) != ITEM_AFFIX_EFFECT_HIT_SPELL)
                            stats.push_back(&roll);
                    if (outcome == 1)
                        for (ItemAffixRoll* roll : stats)
                            roll->Value = ScaleValue(std::get<2>(unique->Stats[roll->Tier]), frand(UNIQUE_REROLL_MIN, UNIQUE_REROLL_MAX));
                    else if ((outcome == 2 || outcome == 3) && !stats.empty())
                    {
                        ItemAffixRoll* roll = stats[urand(0, stats.size() - 1)];
                        roll->Value = ScaleValue(roll->Value, outcome == 2 ? 1.25f : 0.75f);
                    }
                    return true;
                }
                switch (outcome)
                {
                    case 1: // Divine, or Felfire's reweigh
                        for (ItemAffixRoll& roll : g.Rolls)
                            RerollValue(roll, base);
                        break;
                    case 2: // an affix beyond the quality's count; if nothing fits, the weakest affix becomes grade 1
                        if (!AddRoll(g.Rolls, base, g.ItemLevel))
                        {
                            ItemAffixRoll* weakest = nullptr;
                            for (ItemAffixRoll& roll : g.Rolls)
                                if (roll.Grade && (!weakest || roll.Grade > weakest->Grade))
                                    weakest = &roll;
                            if (weakest)
                            {
                                weakest->Grade = 1;
                                RerollValue(*weakest, base);
                            }
                        }
                        break;
                    case 3: // lose an affix
                        if (g.Rolls.size() > 1)
                            g.Rolls.erase(g.Rolls.begin() + urand(0, g.Rolls.size() - 1));
                        break;
                    default: // nothing but the corruption
                        break;
                }
                return true;
            });
            if (!vaal)
                message = "The Light weighs your {} anew: {}.";
            else
            {
                static char const* const outcomes[4] = { "Felfire washes over your {}, and leaves it as it was: {}.",
                    "Felfire reshapes your {}: {}.", "Felfire empowers your {}: {}.", "Felfire scars your {}: {}." };
                message = outcomes[outcome];
            }
            break;
        }
        default:
            return;
    }

    if (!message.empty())
        chat.PSendSysMessage(message.c_str(), before, item->GetTemplate()->Name1);
}

// Every orb's Use: spell (forge_currency); the orb's kind decides what it does.
class spell_forge_orb : public SpellScript
{
    PrepareSpellScript(spell_forge_orb);

    SpellCastResult CheckCast()
    {
        Player* player = GetCaster()->ToPlayer();
        ForgeOrb const* orb = sForgeMgr->GetOrb(GetSpellInfo()->Id);
        if (!player || !orb)
            return SPELL_FAILED_BAD_TARGETS;
        std::string why = sForgeMgr->CheckOrb(*orb, player, GetExplTargetItem());
        if (why.empty())
            return SPELL_CAST_OK;
        ChatHandler(player->GetSession()).SendNotification(why);
        return SPELL_FAILED_DONT_REPORT;
    }

    void HandleHit(SpellEffIndex effIndex)
    {
        PreventHitDefaultEffect(effIndex);
        Player* player = GetCaster()->ToPlayer();
        Item* item = GetHitItem();
        ForgeOrb const* orb = sForgeMgr->GetOrb(GetSpellInfo()->Id);
        if (player && item && orb)
            sForgeMgr->UseOrb(*orb, player, item);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_forge_orb::CheckCast);
        OnEffectHitTarget += SpellEffectFn(spell_forge_orb::HandleHit, EFFECT_0, SPELL_EFFECT_ENCHANT_ITEM);
    }
};

void AddSC_forge_currency()
{
    RegisterSpellScript(spell_forge_orb);
}
