/*
 * This file is part of the mod-forge module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 * Formerly core patches 0001-item-affixes and 0006-item-affix-spells (wowproject).
 */

#include "ForgeMgr.h"
#include "Bag.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "Timer.h"
#include "Tokenize.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <set>
#include <unordered_set>

namespace
{
    // Blizzard's RandPropPoints slot groups (see GenerateEnchSuffixFactor).
    float GetSlotFactor(uint32 inventoryType)
    {
        switch (inventoryType)
        {
            case INVTYPE_HEAD:
            case INVTYPE_CHEST:
            case INVTYPE_ROBE:
            case INVTYPE_LEGS:
            case INVTYPE_2HWEAPON:
                return 1.0f;
            case INVTYPE_SHOULDERS:
            case INVTYPE_WAIST:
            case INVTYPE_FEET:
            case INVTYPE_HANDS:
            case INVTYPE_TRINKET:
                return 0.74f;
            case INVTYPE_NECK:
            case INVTYPE_WRISTS:
            case INVTYPE_FINGER:
            case INVTYPE_CLOAK:
            case INVTYPE_SHIELD:
            case INVTYPE_HOLDABLE:
                return 0.56f;
            case INVTYPE_WEAPON:
            case INVTYPE_WEAPONMAINHAND:
            case INVTYPE_WEAPONOFFHAND:
                return 0.42f;
            case INVTYPE_RANGED:
            case INVTYPE_THROWN:
            case INVTYPE_RANGEDRIGHT:
                return 0.32f;
            default:
                return 0.5f;
        }
    }

    bool IsPercentEffect(ItemAffixEffect effect)
    {
        return effect == ITEM_AFFIX_EFFECT_ARMOR_PCT || effect == ITEM_AFFIX_EFFECT_WEAPON_DMG_PCT ||
            effect == ITEM_AFFIX_EFFECT_ATTACK_SPEED_PCT || effect == ITEM_AFFIX_EFFECT_BLOCK;
    }

    bool IsSpellEffect(ItemAffixEffect effect)
    {
        return effect == ITEM_AFFIX_EFFECT_EQUIP_SPELL || effect == ITEM_AFFIX_EFFECT_HIT_SPELL;
    }

    // Spell affix on a slot it doesn't fit (run speed off boots, fishing off hats and gloves).
    bool FitsSlot(ItemAffix const& affix, ItemTemplate const* proto)
    {
        if (affix.Effect != ITEM_AFFIX_EFFECT_EQUIP_SPELL || !affix.EffectArg)
            return true;
        uint32 type = proto->InventoryType == INVTYPE_ROBE ? INVTYPE_CHEST : proto->InventoryType;
        return type < 32 && (uint32(affix.EffectArg) & (1u << type));
    }

    // Puts spellId in the template's first free spell slot.
    void AddItemSpell(ItemTemplate& t, ItemAffixEffect effect, int32 arg, int32 spellId)
    {
        for (_Spell& spell : t.Spells)
        {
            if (spell.SpellId)
                continue;
            spell.SpellId = spellId;
            spell.SpellTrigger = effect == ITEM_AFFIX_EFFECT_HIT_SPELL ? ITEM_SPELLTRIGGER_CHANCE_ON_HIT : ITEM_SPELLTRIGGER_ON_EQUIP;
            spell.SpellPPMRate = effect == ITEM_AFFIX_EFFECT_HIT_SPELL ? arg / 10.0f : 0.0f;
            return;
        }
    }

    std::vector<uint32> ParseWeights(std::string const& value, std::vector<uint32> const& fallback)
    {
        std::vector<uint32> weights;
        for (std::string_view token : Acore::Tokenize(value, ',', false))
            weights.push_back(Acore::StringTo<uint32>(token).value_or(0));
        weights.resize(3, 0);
        if (weights[0] + weights[1] + weights[2] == 0)
            return fallback;
        return weights;
    }

    // Returns 1..weights.size().
    uint32 PickCount(std::vector<uint32> const& weights)
    {
        uint32 total = 0;
        for (uint32 w : weights)
            total += w;
        uint32 roll = urand(1, total);
        for (std::size_t i = 0; i < weights.size(); ++i)
        {
            if (roll <= weights[i])
                return i + 1;
            roll -= weights[i];
        }
        return 1;
    }

    // Stat costs for the vanilla-style budget (docs/loot-vanilla-plan.md, audit 2026-10-13).
    float StatCost(uint32 mod)
    {
        switch (mod)
        {
            case ITEM_MOD_MANA:                   return 0.067f;
            case ITEM_MOD_HEALTH:                 return 0.1f;
            case ITEM_MOD_ATTACK_POWER:
            case ITEM_MOD_RANGED_ATTACK_POWER:    return 0.5f;
            case ITEM_MOD_SPELL_HEALING_DONE:     return 0.45f;
            case ITEM_MOD_SPELL_DAMAGE_DONE:
            case ITEM_MOD_SPELL_POWER:            return 0.86f;
            case ITEM_MOD_MANA_REGENERATION:
            case ITEM_MOD_HEALTH_REGEN:           return 2.5f;
            case ITEM_MOD_BLOCK_VALUE:            return 0.65f;
            default:                              return 1.0f; // primary stats, stamina, ratings
        }
    }

    constexpr float RESIST_COST = 1.0f;
    constexpr float ARMOR_COST = 0.07f;

    // Rolls whose value comes from the budget (stats, resistances, flat armor); percent effects keep tier values.
    float BudgetCost(ItemAffix const& affix)
    {
        switch (affix.Effect)
        {
            case ITEM_AFFIX_EFFECT_STAT:   return StatCost(uint32(affix.EffectArg));
            case ITEM_AFFIX_EFFECT_RESIST: return RESIST_COST;
            case ITEM_AFFIX_EFFECT_ARMOR:  return ARMOR_COST;
            default:                       return 0.0f;
        }
    }

    // Affix grades: the share of a green item's stat power (vanilla budget formula, summed as power^1.5) that one
    // affix of each grade gets, and how often each grade rolls. The mean share is about 2/3, so the quality
    // ladder's affix counts (green 1-2 ... Legendary 4-6) come out at vanilla's quality ratios on average
    // (green 1.0, blue 1.42, epic 1.79, Legendary 2.27; simulated 2026-10-21), with PoE-like spread.
    struct GradeBand
    {
        float Min;
        float Max;
        uint32 Weight;
    };
    constexpr GradeBand GRADE_BANDS[ITEM_AFFIX_GRADES] = {
        { 0.95f, 1.15f, 10 },
        { 0.75f, 0.95f, 25 },
        { 0.55f, 0.75f, 35 },
        { 0.40f, 0.55f, 30 },
    };

    uint8 PickGrade()
    {
        uint32 total = 0;
        for (GradeBand const& band : GRADE_BANDS)
            total += band.Weight;
        uint32 roll = urand(1, total);
        for (uint8 grade = 1; grade <= ITEM_AFFIX_GRADES; ++grade)
        {
            if (roll <= GRADE_BANDS[grade - 1].Weight)
                return grade;
            roll -= GRADE_BANDS[grade - 1].Weight;
        }
        return ITEM_AFFIX_GRADES;
    }

    // "lo-hi" or "n" -> {lo, hi}, each 0..6.
    std::pair<uint8, uint8> ParseRange(std::string const& value, std::pair<uint8, uint8> fallback)
    {
        std::vector<std::string_view> parts = Acore::Tokenize(value, '-', false);
        if (parts.empty() || parts.size() > 2)
            return fallback;
        Optional<uint8> lo = Acore::StringTo<uint8>(parts[0]);
        Optional<uint8> hi = Acore::StringTo<uint8>(parts.back());
        if (!lo || !hi || *lo > *hi || *hi > 6)
            return fallback;
        return { *lo, *hi };
    }

    void SetResistance(ItemTemplate& t, int32 school, int32 value)
    {
        switch (school)
        {
            case SPELL_SCHOOL_HOLY:   t.HolyRes += value; break;
            case SPELL_SCHOOL_FIRE:   t.FireRes += value; break;
            case SPELL_SCHOOL_NATURE: t.NatureRes += value; break;
            case SPELL_SCHOOL_FROST:  t.FrostRes += value; break;
            case SPELL_SCHOOL_SHADOW: t.ShadowRes += value; break;
            case SPELL_SCHOOL_ARCANE: t.ArcaneRes += value; break;
            default: break;
        }
    }
}

ItemAffixTier const* ItemAffix::GetTier(uint8 tier) const
{
    for (ItemAffixTier const& t : Tiers)
        if (t.Tier == tier)
            return &t;
    return nullptr;
}

ForgeMgr* ForgeMgr::instance()
{
    static ForgeMgr instance;
    return &instance;
}

void ForgeMgr::Load()
{
    uint32 oldMSTime = getMSTime();

    LoadConfig();
    LoadAffixes();
    LoadPools();
    LoadUniqueDefs();
    LoadGenerated();
    FinishUniques();

    LOG_INFO("server.loading", ">> Loaded {} item affixes, {} entry pools, {} generated items, {} uniques in {} ms ({})",
        _affixes.size(), _pools.size(), _generated.size(), _uniqueByEntry.size(), GetMSTimeDiffToNow(oldMSTime),
        _enabled ? "generation enabled" : "generation disabled");
    LOG_INFO("server.loading", " ");
}

void ForgeMgr::LoadConfig()
{
    _enabled = sConfigMgr->GetOption<bool>("Forge.Enable", true);
    _chance = sConfigMgr->GetOption<float>("Forge.Chance", 100.0f);
    _minQuality = sConfigMgr->GetOption<uint32>("Forge.MinQuality", ITEM_QUALITY_UNCOMMON);
    _maxQuality = sConfigMgr->GetOption<uint32>("Forge.MaxQuality", ITEM_QUALITY_RARE);
    _prefixWeights = ParseWeights(sConfigMgr->GetOption<std::string>("Forge.PrefixWeights", "50,35,15"), { 50, 35, 15 });
    _suffixWeights = ParseWeights(sConfigMgr->GetOption<std::string>("Forge.SuffixWeights", "50,35,15"), { 50, 35, 15 });

    _budgetMode = sConfigMgr->GetOption<bool>("Forge.BudgetMode", true);
    _affixGrades = sConfigMgr->GetOption<bool>("Forge.AffixGrades", true);
    static char const* const countKeys[] = { "Poor", "Normal", "Uncommon", "Rare", "Epic", "Legendary" };
    static constexpr std::pair<uint8, uint8> countDefaults[] = { { 0, 1 }, { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 4 }, { 4, 6 } };
    for (uint8 q = ITEM_QUALITY_POOR; q <= ITEM_QUALITY_LEGENDARY; ++q)
    {
        auto [lo, hi] = ParseRange(sConfigMgr->GetOption<std::string>(std::string("Forge.Count.") + countKeys[q], ""), countDefaults[q]);
        _countMin[q] = lo;
        _countMax[q] = hi;
    }
    _legendaryChance = sConfigMgr->GetOption<float>("Forge.LegendaryChance", 0.5f);
    _legendaryBudget = sConfigMgr->GetOption<float>("Forge.LegendaryBudget", 1.3f);
    _budgetPerLevel = sConfigMgr->GetOption<float>("Forge.BudgetPerLevel", 0.45f);
    _budgetOffset = sConfigMgr->GetOption<float>("Forge.BudgetOffset", -3.0f);
    _recycleIntervalMs = sConfigMgr->GetOption<uint32>("Forge.RecycleIntervalMinutes", 30) * MINUTE * IN_MILLISECONDS;
    _recycleGraceMinutes = std::max<uint32>(30, sConfigMgr->GetOption<uint32>("Forge.RecycleGraceMinutes", 120));
}

void ForgeMgr::Update(uint32 diff)
{
    if (!_enabled || !_recycleIntervalMs)
        return;

    _recycleTimer += diff;
    if (_recycleTimer < _recycleIntervalMs)
        return;
    _recycleTimer = 0;

    uint32 oldMSTime = getMSTime();
    QueryResult result = CharacterDatabase.Query("SELECT g.entry FROM item_generated g LEFT JOIN item_instance i ON i.itemEntry = g.entry "
        "WHERE i.guid IS NULL AND g.created < NOW() - INTERVAL {} MINUTE", _recycleGraceMinutes);
    if (!result)
        return;

    // item_instance doesn't cover everything a player holds: a sold item sits in a buyback slot after its
    // row is deleted at the next save, and saving it again with a freed entry crashed in
    // Player::_SaveInventory. Skip entries any online player (or bot) still holds.
    std::unordered_set<uint32> held = CollectHeldEntries();

    std::vector<uint32> freed;
    {
        std::unique_lock lock(_lock);
        do
        {
            uint32 entry = result->Fetch()[0].Get<uint32>();
            if (held.count(entry))
                continue;
            auto itr = _generated.find(entry);
            if (itr == _generated.end() || _uniqueByEntry.count(entry))
                continue;
            ItemAffixPool* pool = FindPool(entry);
            if (!pool)
                continue;
            _retired.push_back(std::move(itr->second->Template));
            _generated.erase(itr);
            pool->Free.push_back(entry);
            freed.push_back(entry);
        } while (result->NextRow());
    }

    if (freed.empty())
        return;

    std::string list;
    for (uint32 entry : freed)
        list += (list.empty() ? "" : ",") + std::to_string(entry);
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    trans->Append("DELETE FROM item_generated_affix WHERE entry IN ({})", list);
    trans->Append("DELETE FROM item_generated WHERE entry IN ({})", list);
    CharacterDatabase.CommitTransaction(trans);
    LOG_INFO("module.forge", "Recycled {} unused generated entries in {} ms ({} retired templates kept).",
        freed.size(), GetMSTimeDiffToNow(oldMSTime), _retired.size());
}

// Generated entries in online players' equipment, bags, bank, buyback and keyring. Called from Update on the
// world thread, outside the map update.
std::unordered_set<uint32> ForgeMgr::CollectHeldEntries() const
{
    std::unordered_set<uint32> held;
    auto note = [&](Item const* item)
    {
        if (item && item->GetEntry() >= _minEntry && item->GetEntry() < _maxEntry)
            held.insert(item->GetEntry());
    };

    std::shared_lock lock(*HashMapHolder<Player>::GetLock());
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
    {
        for (uint8 slot = PLAYER_SLOT_START; slot < PLAYER_SLOT_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            note(item);
            if (Bag* bag = item ? item->ToBag() : nullptr)
                for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                    note(bag->GetItemByPos(uint8(i)));
        }
    }
    return held;
}

float ForgeMgr::BaseBudget(ItemTemplate const* base) const
{
    float sum = 0.0f;
    for (uint32 i = 0; i < base->StatsCount && i < MAX_ITEM_PROTO_STATS; ++i)
        if (base->ItemStat[i].ItemStatValue > 0)
            sum += std::pow(base->ItemStat[i].ItemStatValue * StatCost(base->ItemStat[i].ItemStatType), 1.5f);
    for (int32 res : { base->HolyRes, base->FireRes, base->NatureRes, base->FrostRes, base->ShadowRes, base->ArcaneRes })
        if (res > 0)
            sum += std::pow(res * RESIST_COST, 1.5f);
    return sum > 0.0f ? std::pow(sum, 2.0f / 3.0f) : 0.0f;
}

float ForgeMgr::EstimateBudget(uint32 itemLevel, uint32 inventoryType, uint32 quality) const
{
    float green = std::max(1.0f, _budgetPerLevel * float(itemLevel) + _budgetOffset);
    return green * GetSlotFactor(inventoryType) * _qualityBudget[std::min<uint32>(quality, MAX_ITEM_QUALITY - 1)];
}

float ForgeMgr::ItemBudget(ItemTemplate const* base, uint8 quality) const
{
    // The base item's own stats set the budget; items without template stats (white/grey, random-suffix
    // greens) use the estimate for their level, slot and quality.
    float budget = BaseBudget(base);
    if (budget <= 0.0f)
        budget = EstimateBudget(base->ItemLevel, base->InventoryType, base->Quality);
    if (quality != base->Quality && base->Quality < MAX_ITEM_QUALITY && quality < MAX_ITEM_QUALITY)
    {
        if (quality == ITEM_QUALITY_LEGENDARY && base->Quality == ITEM_QUALITY_EPIC)
            budget *= _legendaryBudget;
        else
            budget *= _qualityBudget[quality] / std::max(0.01f, _qualityBudget[base->Quality]);
    }
    return budget;
}

float ForgeMgr::GreenBudget(ItemTemplate const* base) const
{
    float budget = BaseBudget(base);
    uint32 quality = std::min<uint32>(base->Quality, MAX_ITEM_QUALITY - 1);
    if (budget > 0.0f)
        return budget / std::max(0.01f, _qualityBudget[quality]);
    return EstimateBudget(base->ItemLevel, base->InventoryType, ITEM_QUALITY_UNCOMMON);
}

void ForgeMgr::SetGradeValue(ItemAffixRoll& roll, ItemAffix const& affix, ItemAffixTier const& tier, float greenBudget) const
{
    if (IsSpellEffect(affix.Effect))
    {
        roll.Grade = 0;
        roll.Value = tier.MinValue;
        return;
    }
    if (!roll.Grade || roll.Grade > ITEM_AFFIX_GRADES)
        roll.Grade = PickGrade();
    float cost = BudgetCost(affix);
    if (cost <= 0.0f)
    {
        // Percent effects keep their tier's range; the grade picked the tier.
        roll.Value = irand(tier.MinValue, tier.MaxValue);
        return;
    }
    GradeBand const& band = GRADE_BANDS[roll.Grade - 1];
    float value = std::pow(frand(band.Min, band.Max), 2.0f / 3.0f) * greenBudget / cost;
    roll.Value = std::max<int32>(1, int32(std::lround(value)));
}

void ForgeMgr::SplitBudget(std::vector<ItemAffixRoll>& rolls, float budget) const
{
    // Random shares (60-140% before normalising), so items of one base differ; each share is turned back
    // into a stat value with that stat's cost: value = share^(2/3) * budget / cost.
    std::vector<std::pair<ItemAffixRoll*, float>> budgeted;
    float total = 0.0f;
    for (ItemAffixRoll& roll : rolls)
    {
        ItemAffix const* affix = GetAffix(roll.AffixId);
        if (!affix)
            continue;
        // A spell affix takes an average share of the budget and keeps its spell.
        if (IsSpellEffect(affix->Effect))
        {
            total += 1.0f;
            continue;
        }
        if (BudgetCost(*affix) <= 0.0f)
            continue;
        float weight = frand(0.6f, 1.4f);
        budgeted.emplace_back(&roll, weight);
        total += weight;
    }
    for (auto& [roll, weight] : budgeted)
    {
        float cost = BudgetCost(*GetAffix(roll->AffixId));
        float value = std::pow(weight / total, 2.0f / 3.0f) * budget / cost;
        roll->Value = std::max<int32>(1, int32(std::lround(value)));
    }
}

void ForgeMgr::LoadAffixes()
{
    _affixes.clear();

    //                                                   0     1     2     3       4           5          6
    QueryResult result = WorldDatabase.Query("SELECT id, kind, grp, effect, effect_arg, item_mask, weight FROM item_affix");
    if (!result)
    {
        LOG_WARN("server.loading", ">> Loaded 0 item affixes. DB table `item_affix` is empty.");
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        ItemAffix affix;
        affix.Id = fields[0].Get<uint32>();
        uint8 kind = fields[1].Get<uint8>();
        uint8 effect = fields[3].Get<uint8>();
        if (kind > ITEM_AFFIX_SUFFIX || effect >= MAX_ITEM_AFFIX_EFFECT)
        {
            LOG_ERROR("sql.sql", "Table `item_affix`: affix {} has invalid kind {} or effect {}, skipped.", affix.Id, kind, effect);
            continue;
        }
        affix.Kind = ItemAffixKind(kind);
        affix.Group = fields[2].Get<uint32>();
        affix.Effect = ItemAffixEffect(effect);
        affix.EffectArg = fields[4].Get<int32>();
        affix.ItemMask = fields[5].Get<uint32>();
        affix.Weight = fields[6].Get<uint32>();
        if (affix.Effect == ITEM_AFFIX_EFFECT_STAT && (affix.EffectArg < 0 || affix.EffectArg >= MAX_ITEM_MOD))
        {
            LOG_ERROR("sql.sql", "Table `item_affix`: affix {} has invalid stat type {}, skipped.", affix.Id, affix.EffectArg);
            continue;
        }
        _affixes[affix.Id] = std::move(affix);
    } while (result->NextRow());

    //                                    0         1     2         3          4          5          6       7
    result = WorldDatabase.Query("SELECT affix_id, tier, min_ilvl, req_level, min_value, max_value, weight, name "
        "FROM item_affix_tier ORDER BY affix_id, min_ilvl, tier");
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 affixId = fields[0].Get<uint32>();
            auto itr = _affixes.find(affixId);
            if (itr == _affixes.end())
            {
                LOG_ERROR("sql.sql", "Table `item_affix_tier`: unknown affix {}, skipped.", affixId);
                continue;
            }
            ItemAffixTier tier;
            tier.Tier = fields[1].Get<uint8>();
            tier.MinItemLevel = fields[2].Get<uint8>();
            tier.RequiredLevel = fields[3].Get<uint8>();
            tier.MinValue = fields[4].Get<int32>();
            tier.MaxValue = std::max(tier.MinValue, fields[5].Get<int32>());
            tier.Weight = fields[6].Get<uint32>();
            tier.Name = fields[7].Get<std::string>();
            itr->second.Tiers.push_back(std::move(tier));
        } while (result->NextRow());
    }

    for (auto itr = _affixes.begin(); itr != _affixes.end();)
    {
        ItemAffix const& affix = itr->second;
        if (affix.Tiers.empty())
        {
            LOG_ERROR("sql.sql", "Table `item_affix`: affix {} has no tiers, skipped.", itr->first);
            itr = _affixes.erase(itr);
            continue;
        }
        bool badSpell = false;
        if (IsSpellEffect(affix.Effect))
            for (ItemAffixTier const& tier : affix.Tiers)
                if (!sSpellMgr->GetSpellInfo(uint32(tier.MinValue)))
                {
                    LOG_ERROR("sql.sql", "Table `item_affix_tier`: affix {} tier {} has unknown spell {}, affix skipped.",
                        affix.Id, tier.Tier, tier.MinValue);
                    badSpell = true;
                }
        if (badSpell)
            itr = _affixes.erase(itr);
        else
            ++itr;
    }
}

void ForgeMgr::LoadPools()
{
    _pools.clear();
    _poolsByKey.clear();

    uint32 staticMax = uint32(sObjectMgr->GetItemTemplateStoreFast()->size());

    //                                                   0   1            2      3      4         5      6         7        8               9
    QueryResult result = WorldDatabase.Query("SELECT id, first_entry, count, class, subclass, sound, material, display, inventory_type, sheath "
        "FROM item_affix_pool ORDER BY first_entry");
    if (!result)
    {
        LOG_WARN("server.loading", ">> Loaded 0 item affix pools. DB table `item_affix_pool` is empty; no items will be generated.");
        return;
    }

    uint32 lastEnd = 0;
    do
    {
        Field* fields = result->Fetch();
        ItemAffixPool pool;
        pool.Id = fields[0].Get<uint32>();
        pool.FirstEntry = fields[1].Get<uint32>();
        pool.Count = fields[2].Get<uint32>();
        pool.DisplayId = fields[7].Get<uint32>();
        pool.NextUnused = pool.FirstEntry;
        if (!pool.Count || pool.FirstEntry < staticMax || pool.FirstEntry < lastEnd)
        {
            LOG_ERROR("sql.sql", "Table `item_affix_pool`: pool {} ({}+{}) is empty, overlaps item_template or another pool; skipped.",
                pool.Id, pool.FirstEntry, pool.Count);
            continue;
        }
        lastEnd = pool.FirstEntry + pool.Count;

        PoolKey key { fields[3].Get<uint32>(), fields[4].Get<uint32>(), fields[5].Get<int32>(),
            fields[6].Get<int32>(), fields[8].Get<uint32>(), fields[9].Get<uint32>() };
        _poolsByKey[key].push_back(_pools.size());
        _pools.push_back(std::move(pool));
    } while (result->NextRow());

    if (QueryResult state = CharacterDatabase.Query("SELECT pool_id, next_unused FROM item_affix_pool_state"))
    {
        do
        {
            Field* fields = state->Fetch();
            uint32 poolId = fields[0].Get<uint32>();
            for (ItemAffixPool& pool : _pools)
                if (pool.Id == poolId)
                    pool.NextUnused = std::clamp(fields[1].Get<uint32>(), pool.FirstEntry, pool.FirstEntry + pool.Count);
        } while (state->NextRow());
    }
}

void ForgeMgr::LoadGenerated()
{
    _generated.clear();

    // Drop rolls nothing holds any more: loot nobody picked up, sold or destroyed items.
    // item_instance covers bags, bank, mail, auctions and guild banks.
    CharacterDatabase.DirectExecute("DELETE g FROM item_generated g LEFT JOIN item_instance i ON i.itemEntry = g.entry WHERE i.guid IS NULL");
    CharacterDatabase.DirectExecute("DELETE a FROM item_generated_affix a LEFT JOIN item_generated g ON g.entry = a.entry WHERE g.entry IS NULL");

    std::unordered_map<uint32, std::vector<ItemAffixRoll>> rolls;
    if (QueryResult result = CharacterDatabase.Query("SELECT entry, affix_id, tier, value, grade FROM item_generated_affix ORDER BY entry, idx"))
    {
        do
        {
            Field* fields = result->Fetch();
            rolls[fields[0].Get<uint32>()].push_back({ fields[1].Get<uint32>(), fields[2].Get<uint8>(), fields[3].Get<int32>(),
                fields[4].Get<uint8>() });
        } while (result->NextRow());
    }

    uint32 regenerated = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT entry, base_entry, item_level, quality FROM item_generated"))
    {
        do
        {
            Field* fields = result->Fetch();
            auto item = std::make_unique<GeneratedItem>();
            item->Entry = fields[0].Get<uint32>();
            item->BaseEntry = fields[1].Get<uint32>();
            item->ItemLevel = fields[2].Get<uint8>();
            item->Quality = fields[3].IsNull() ? ITEM_AFFIX_QUALITY_UNSET : fields[3].Get<uint8>();

            ItemTemplate const* base = sObjectMgr->GetItemTemplate(item->BaseEntry);
            if (!base || item->Entry < sObjectMgr->GetItemTemplateStoreFast()->size())
            {
                LOG_ERROR("sql.sql", "Table `item_generated`: entry {} has a missing base {} or collides with item_template; "
                    "items using it will be deleted at login.", item->Entry, item->BaseEntry);
                continue;
            }

            item->Rolls = std::move(rolls[item->Entry]);
            ItemAffixPool* pool = FindPool(item->Entry);
            // Regenerate rows from before the quality ladder: keep the affixes and tiers (and so the name),
            // take the base's quality and re-split its budget.
            if (_budgetMode && item->Quality >= MAX_ITEM_QUALITY)
            {
                item->Quality = uint8(std::min<uint32>(base->Quality, ITEM_QUALITY_LEGENDARY));
                SplitBudget(item->Rolls, ItemBudget(base, item->Quality));
                if (pool)
                    SaveGenerated(*item, *pool);
                ++regenerated;
            }
            item->Template = BuildTemplate(item->Entry, *base, item->ItemLevel, item->Rolls, pool ? pool->DisplayId : 0,
                item->Quality);
            if (pool && pool->NextUnused <= item->Entry)
                pool->NextUnused = item->Entry + 1;
            _generated[item->Entry] = std::move(item);
        } while (result->NextRow());
    }

    if (regenerated)
        LOG_INFO("server.loading", ">> Regenerated {} generated items under the quality ladder (quality from base, stat budget).",
            regenerated);

    _minEntry = UINT32_MAX;
    _maxEntry = 0;
    for (ItemAffixPool const& pool : _pools)
    {
        _minEntry = std::min(_minEntry, pool.FirstEntry);
        _maxEntry = std::max(_maxEntry, pool.FirstEntry + pool.Count);
    }
    for (auto const& [entry, item] : _generated)
    {
        _minEntry = std::min(_minEntry, entry);
        _maxEntry = std::max(_maxEntry, entry + 1);
    }

    std::set<uint32> uniqueEntries;
    for (auto const& unique : _uniques)
        if (unique->Entry)
        {
            uniqueEntries.insert(unique->Entry);
            if (ItemAffixPool* pool = FindPool(unique->Entry); pool && pool->NextUnused <= unique->Entry)
                pool->NextUnused = unique->Entry + 1;
        }

    // Freed entries get reused only once a pool has handed out every fresh entry.
    for (ItemAffixPool& pool : _pools)
    {
        pool.Free.clear();
        for (uint32 entry = pool.FirstEntry; entry < pool.NextUnused; ++entry)
            if (!_generated.count(entry) && !uniqueEntries.count(entry))
                pool.Free.push_back(entry);
    }
}

void ForgeMgr::LoadUniqueDefs()
{
    _uniques.clear();
    _uniqueByEntry.clear();
    _uniqueByCreature.clear();

    //                                                   0    1      2           3           4               5            6             7
    QueryResult result = WorldDatabase.Query("SELECT `key`, name, base_entry, item_level, required_level, description, creature_entry, chance "
        "FROM item_unique");
    if (!result)
        return;

    std::unordered_map<std::string, UniqueItem*> byKey;
    do
    {
        Field* fields = result->Fetch();
        auto unique = std::make_unique<UniqueItem>();
        unique->Key = fields[0].Get<std::string>();
        unique->Name = fields[1].Get<std::string>();
        unique->BaseEntry = fields[2].Get<uint32>();
        unique->ItemLevel = fields[3].Get<uint8>();
        unique->RequiredLevel = fields[4].Get<uint8>();
        unique->Description = fields[5].Get<std::string>();
        unique->CreatureEntry = fields[6].Get<uint32>();
        unique->Chance = fields[7].Get<float>();
        byKey[unique->Key] = unique.get();
        _uniques.push_back(std::move(unique));
    } while (result->NextRow());

    //                                                 0            1    2       3           4
    if (QueryResult stats = WorldDatabase.Query("SELECT unique_key, idx, effect, effect_arg, value FROM item_unique_stat ORDER BY unique_key, idx"))
    {
        do
        {
            Field* fields = stats->Fetch();
            auto itr = byKey.find(fields[0].Get<std::string>());
            uint8 effect = fields[2].Get<uint8>();
            if (itr == byKey.end() || effect >= MAX_ITEM_AFFIX_EFFECT)
                continue;
            itr->second->Stats.emplace_back(ItemAffixEffect(effect), fields[3].Get<int32>(), fields[4].Get<int32>());
        } while (stats->NextRow());
    }

    if (QueryResult state = CharacterDatabase.Query("SELECT unique_key, entry FROM item_unique_state"))
    {
        do
        {
            Field* fields = state->Fetch();
            auto itr = byKey.find(fields[0].Get<std::string>());
            if (itr != byKey.end() && FindPool(fields[1].Get<uint32>()))
                itr->second->Entry = fields[1].Get<uint32>();
        } while (state->NextRow());
    }
}

void ForgeMgr::FinishUniques()
{
    std::unique_lock lock(_lock);
    for (auto& unique : _uniques)
    {
        ItemTemplate const* base = sObjectMgr->GetItemTemplate(unique->BaseEntry);
        if (!base || unique->BaseEntry >= sObjectMgr->GetItemTemplateStoreFast()->size() || !GetItemMask(base))
        {
            LOG_ERROR("sql.sql", "Table `item_unique`: '{}' has missing or unusable base item {}, skipped.", unique->Key, unique->BaseEntry);
            continue;
        }

        if (!unique->Entry)
        {
            bool reused = false;
            auto [entry, pool] = AllocateEntry(base, reused);
            if (!entry)
            {
                LOG_ERROR("sql.sql", "Table `item_unique`: no free Item.dbc entry for '{}' (base {}), skipped.", unique->Key, unique->BaseEntry);
                continue;
            }
            unique->Entry = entry;
            CharacterDatabase.DirectExecute("REPLACE INTO item_unique_state (unique_key, entry) VALUES ('{}', {})", unique->Key, entry);
            CharacterDatabase.DirectExecute("REPLACE INTO item_affix_pool_state (pool_id, next_unused) VALUES ({}, {})", pool->Id, pool->NextUnused);
        }

        ItemAffixPool* pool = FindPool(unique->Entry);
        std::unique_ptr<ItemTemplate> t = BuildTemplate(unique->Entry, *base, unique->ItemLevel, {}, pool ? pool->DisplayId : 0,
            ITEM_QUALITY_ARTIFACT);
        t->Name1 = unique->Name;
        t->Description = unique->Description;
        t->ItemLevel = std::max<uint32>(unique->ItemLevel, 1);
        t->RequiredLevel = std::min<uint32>(unique->RequiredLevel, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
        t->Bonding = BIND_WHEN_PICKED_UP;
        t->MaxCount = 1; // "Unique": one per character
        for (auto const& [effect, arg, value] : unique->Stats)
        {
            switch (effect)
            {
                case ITEM_AFFIX_EFFECT_STAT:
                    if (t->StatsCount < MAX_ITEM_PROTO_STATS)
                    {
                        t->ItemStat[t->StatsCount].ItemStatType = arg;
                        t->ItemStat[t->StatsCount].ItemStatValue = value;
                        ++t->StatsCount;
                    }
                    break;
                case ITEM_AFFIX_EFFECT_RESIST: SetResistance(*t, arg, value); break;
                case ITEM_AFFIX_EFFECT_ARMOR:  t->Armor += value; break;
                case ITEM_AFFIX_EFFECT_EQUIP_SPELL:
                case ITEM_AFFIX_EFFECT_HIT_SPELL:
                    if (sSpellMgr->GetSpellInfo(uint32(value)))
                        AddItemSpell(*t, effect, arg, value);
                    else
                        LOG_ERROR("sql.sql", "Table `item_unique_stat`: '{}' has unknown spell {}, skipped.", unique->Key, value);
                    break;
                default: break;
            }
        }
        t->SellPrice = std::max<uint32>(1, base->SellPrice * 4);
        unique->Template = std::move(t);
        _uniqueByEntry[unique->Entry] = unique.get();
        if (unique->CreatureEntry)
            _uniqueByCreature.emplace(unique->CreatureEntry, unique.get());
        _minEntry = std::min(_minEntry, unique->Entry);
        _maxEntry = std::max(_maxEntry, unique->Entry + 1);
    }
}

UniqueItem const* ForgeMgr::GetUnique(std::string const& key) const
{
    for (auto const& unique : _uniques)
        if (unique->Key == key && unique->Template)
            return unique.get();
    return nullptr;
}

std::vector<UniqueItem const*> ForgeMgr::GetUniques() const
{
    std::vector<UniqueItem const*> out;
    for (auto const& unique : _uniques)
        if (unique->Template)
            out.push_back(unique.get());
    return out;
}

ItemTemplate const* ForgeMgr::GetTemplate(uint32 entry) const
{
    if (entry < _minEntry || entry >= _maxEntry)
        return nullptr;

    if (auto unique = _uniqueByEntry.find(entry); unique != _uniqueByEntry.end())
        return unique->second->Template.get();

    std::shared_lock lock(_lock);
    auto itr = _generated.find(entry);
    return itr != _generated.end() ? itr->second->Template.get() : nullptr;
}

bool ForgeMgr::IsGenerated(uint32 entry) const
{
    return GetTemplate(entry) != nullptr;
}

GeneratedItem const* ForgeMgr::GetGenerated(uint32 entry) const
{
    if (entry < _minEntry || entry >= _maxEntry)
        return nullptr;

    std::shared_lock lock(_lock);
    auto itr = _generated.find(entry);
    return itr != _generated.end() ? itr->second.get() : nullptr;
}

ItemAffix const* ForgeMgr::GetAffix(uint32 id) const
{
    auto itr = _affixes.find(id);
    return itr != _affixes.end() ? &itr->second : nullptr;
}

uint32 ForgeMgr::GetItemMask(ItemTemplate const* proto)
{
    if (proto->Class == ITEM_CLASS_WEAPON)
    {
        switch (proto->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_GUN:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            case ITEM_SUBCLASS_WEAPON_THROWN:
            case ITEM_SUBCLASS_WEAPON_WAND:
                return ITEM_AFFIX_MASK_RANGED;
            case ITEM_SUBCLASS_WEAPON_obsolete:
            case ITEM_SUBCLASS_WEAPON_EXOTIC:
            case ITEM_SUBCLASS_WEAPON_EXOTIC2:
            case ITEM_SUBCLASS_WEAPON_MISC:
            case ITEM_SUBCLASS_WEAPON_SPEAR:
            case ITEM_SUBCLASS_WEAPON_FISHING_POLE:
                return 0;
            default:
                break;
        }
        switch (proto->InventoryType)
        {
            case INVTYPE_2HWEAPON:
                return ITEM_AFFIX_MASK_WEAPON_2H;
            case INVTYPE_WEAPON:
            case INVTYPE_WEAPONMAINHAND:
            case INVTYPE_WEAPONOFFHAND:
                return ITEM_AFFIX_MASK_WEAPON_1H;
            default:
                return 0;
        }
    }

    if (proto->Class != ITEM_CLASS_ARMOR)
        return 0;

    switch (proto->InventoryType)
    {
        case INVTYPE_NECK:
        case INVTYPE_FINGER:
        case INVTYPE_TRINKET:
            return ITEM_AFFIX_MASK_JEWELRY;
        case INVTYPE_CLOAK:
            return ITEM_AFFIX_MASK_CLOAK;
        case INVTYPE_HOLDABLE:
            return ITEM_AFFIX_MASK_HOLDABLE;
        case INVTYPE_SHIELD:
            return ITEM_AFFIX_MASK_SHIELD;
        case INVTYPE_BODY:
        case INVTYPE_TABARD:
        case INVTYPE_RELIC:
        case INVTYPE_NON_EQUIP:
        case INVTYPE_BAG:
        case INVTYPE_QUIVER:
        case INVTYPE_AMMO:
            return 0;
        default:
            break;
    }

    switch (proto->SubClass)
    {
        case ITEM_SUBCLASS_ARMOR_CLOTH:   return ITEM_AFFIX_MASK_CLOTH;
        case ITEM_SUBCLASS_ARMOR_LEATHER: return ITEM_AFFIX_MASK_LEATHER;
        case ITEM_SUBCLASS_ARMOR_MAIL:    return ITEM_AFFIX_MASK_MAIL;
        case ITEM_SUBCLASS_ARMOR_PLATE:   return ITEM_AFFIX_MASK_PLATE;
        default:                          return 0;
    }
}

ForgeMgr::PoolKey ForgeMgr::MakePoolKey(ItemTemplate const* proto)
{
    return { proto->Class, proto->SubClass, proto->SoundOverrideSubclass, proto->Material, proto->InventoryType, proto->Sheath };
}

bool ForgeMgr::IsEligible(ItemTemplate const* base) const
{
    // Generated entries sit above every item_template entry; they never re-roll.
    if (!base || base->ItemId >= sObjectMgr->GetItemTemplateStoreFast()->size())
        return false;
    if (base->Quality < _minQuality || base->Quality > _maxQuality)
        return false;
    // BoP greens and blues are hand-made dungeon/quest loot: PoE "uniques".
    if (base->Bonding == BIND_WHEN_PICKED_UP || base->Bonding == BIND_QUEST_ITEM || base->Bonding == BIND_QUEST_ITEM1)
        return false;
    if (base->ItemSet || base->StartQuest || base->MaxCount > 0 || base->HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE))
        return false;
    if (!GetItemMask(base))
        return false;
    return _poolsByKey.count(MakePoolKey(base)) > 0;
}

std::vector<ItemAffixRoll> ForgeMgr::Roll(ItemTemplate const* base, uint8 itemLevel, uint8 prefixes, uint8 suffixes,
    uint8 quality) const
{
    uint32 mask = GetItemMask(base);
    float slotFactor = GetSlotFactor(base->InventoryType);
    bool grades = _budgetMode && _affixGrades;
    float greenBudget = grades ? GreenBudget(base) : 0.0f;
    std::set<uint32> usedGroups;
    std::vector<ItemAffixRoll> rolls;

    // Budget mode: the affix count comes from the quality ladder (Forge.Count.*), split at random
    // between prefixes and suffixes (3 of each at most). Forced counts (.forge roll) still win.
    uint32 counts[2] = { prefixes, suffixes };
    bool byQuality = _budgetMode && !prefixes && !suffixes && quality < MAX_ITEM_QUALITY;
    if (byQuality)
    {
        uint32 total = urand(_countMin[quality], _countMax[quality]);
        if (!total)
            return rolls;
        counts[0] = urand(total > 3 ? total - 3 : 0, std::min<uint32>(total, 3));
        counts[1] = total - counts[0];
    }

    for (ItemAffixKind kind : { ITEM_AFFIX_PREFIX, ITEM_AFFIX_SUFFIX })
    {
        uint8 forced = kind == ITEM_AFFIX_PREFIX ? prefixes : suffixes;
        uint32 count = byQuality ? counts[kind]
            : forced ? std::min<uint32>(forced, 3) : PickCount(kind == ITEM_AFFIX_PREFIX ? _prefixWeights : _suffixWeights);
        for (uint32 n = 0; n < count; ++n)
        {
            std::vector<ItemAffix const*> candidates;
            uint32 totalWeight = 0;
            for (auto const& [id, affix] : _affixes)
            {
                if (affix.Kind != kind || !(affix.ItemMask & mask) || !affix.Weight || usedGroups.count(affix.Group))
                    continue;
                if (!FitsSlot(affix, base))
                    continue;
                if (affix.Tiers.front().MinItemLevel > itemLevel)
                    continue;
                candidates.push_back(&affix);
                totalWeight += affix.Weight;
            }
            if (!totalWeight)
                break;

            uint32 pick = urand(1, totalWeight);
            ItemAffix const* affix = candidates.back();
            for (ItemAffix const* candidate : candidates)
            {
                if (pick <= candidate->Weight)
                {
                    affix = candidate;
                    break;
                }
                pick -= candidate->Weight;
            }

            if (grades)
            {
                // The grade names the affix: grade 1 takes the highest tier the item level unlocks, each grade
                // below one tier lower. A spell affix takes the highest.
                std::vector<ItemAffixTier const*> unlocked;
                for (ItemAffixTier const& t : affix->Tiers)
                    if (t.MinItemLevel <= itemLevel)
                        unlocked.push_back(&t);
                ItemAffixRoll roll { affix->Id, 0, 0, IsSpellEffect(affix->Effect) ? uint8(0) : PickGrade() };
                std::size_t below = roll.Grade ? std::min<std::size_t>(roll.Grade - 1, unlocked.size() - 1) : 0;
                ItemAffixTier const* tier = unlocked[unlocked.size() - 1 - below];
                roll.Tier = tier->Tier;
                SetGradeValue(roll, *affix, *tier, greenBudget);
                rolls.push_back(roll);
                usedGroups.insert(affix->Group);
                continue;
            }

            uint32 tierWeight = 0;
            for (ItemAffixTier const& tier : affix->Tiers)
                if (tier.MinItemLevel <= itemLevel)
                    tierWeight += std::max<uint32>(tier.Weight, 1);
            uint32 tierPick = urand(1, tierWeight);
            ItemAffixTier const* tier = &affix->Tiers.front();
            for (ItemAffixTier const& t : affix->Tiers)
            {
                if (t.MinItemLevel > itemLevel)
                    break;
                // A spell's numbers are fixed, so a spell affix takes the best tier for the item level.
                if (IsSpellEffect(affix->Effect))
                {
                    tier = &t;
                    continue;
                }
                uint32 w = std::max<uint32>(t.Weight, 1);
                if (tierPick <= w)
                {
                    tier = &t;
                    break;
                }
                tierPick -= w;
            }

            int32 value = irand(tier->MinValue, tier->MaxValue);
            if (IsSpellEffect(affix->Effect))
                value = tier->MinValue;
            else if (!IsPercentEffect(affix->Effect))
                value = std::max<int32>(1, int32(std::lround(value * slotFactor)));

            rolls.push_back({ affix->Id, tier->Tier, value });
            usedGroups.insert(affix->Group);
        }
    }

    if (_budgetMode && !grades && !rolls.empty())
        SplitBudget(rolls, ItemBudget(base, quality < MAX_ITEM_QUALITY ? quality : base->Quality));
    return rolls;
}

std::unique_ptr<ItemTemplate> ForgeMgr::BuildTemplate(uint32 entry, ItemTemplate const& base, uint8 itemLevel,
    std::vector<ItemAffixRoll> const& rolls, uint32 displayOverride, uint8 quality) const
{
    auto t = std::make_unique<ItemTemplate>(base);
    t->ItemId = entry;
    t->ScriptId = 0;
    t->Description.clear();

    // Strip to the base type: armor, damage, speed, block and sockets stay.
    t->StatsCount = 0;
    for (_ItemStat& stat : t->ItemStat)
    {
        stat.ItemStatType = 0;
        stat.ItemStatValue = 0;
    }
    t->HolyRes = t->FireRes = t->NatureRes = t->FrostRes = t->ShadowRes = t->ArcaneRes = 0;
    for (_Spell& spell : t->Spells)
    {
        spell = _Spell();
        spell.SpellCooldown = -1;
        spell.SpellCategoryCooldown = -1;
    }
    t->RandomProperty = 0;
    t->RandomSuffix = 0;
    t->ItemSet = 0;
    t->ScalingStatDistribution = 0;
    t->ScalingStatValue = 0;
    if (displayOverride)
        t->DisplayInfoID = displayOverride;

    int32 armorPct = 0, armorFlat = 0, damagePct = 0, speedPct = 0;
    uint8 requiredLevel = 0;
    ItemAffixTier const* bestTier[2] = { nullptr, nullptr };
    bool bestIsSpell[2] = { false, false };

    for (ItemAffixRoll const& roll : rolls)
    {
        ItemAffix const* affix = GetAffix(roll.AffixId);
        ItemAffixTier const* tier = affix ? affix->GetTier(roll.Tier) : nullptr;
        if (!affix || !tier)
        {
            LOG_ERROR("server.loading", "Generated item {}: unknown affix {} tier {}, ignored.", entry, roll.AffixId, roll.Tier);
            continue;
        }
        requiredLevel = std::max(requiredLevel, tier->RequiredLevel);
        // The name shows the spell affix (at most one per item), else the highest tier, so a player can tell
        // a proc or equip effect from the name.
        bool spell = IsSpellEffect(affix->Effect);
        if (!bestTier[affix->Kind] || spell || (!bestIsSpell[affix->Kind] && tier->Tier > bestTier[affix->Kind]->Tier))
        {
            bestTier[affix->Kind] = tier;
            bestIsSpell[affix->Kind] = spell;
        }

        switch (affix->Effect)
        {
            case ITEM_AFFIX_EFFECT_STAT:
            {
                uint32 i = 0;
                while (i < t->StatsCount && t->ItemStat[i].ItemStatType != uint32(affix->EffectArg))
                    ++i;
                if (i == MAX_ITEM_PROTO_STATS)
                    break;
                if (i == t->StatsCount)
                {
                    t->ItemStat[i].ItemStatType = affix->EffectArg;
                    ++t->StatsCount;
                }
                t->ItemStat[i].ItemStatValue += roll.Value;
                break;
            }
            case ITEM_AFFIX_EFFECT_ARMOR:            armorFlat += roll.Value; break;
            case ITEM_AFFIX_EFFECT_ARMOR_PCT:        armorPct += roll.Value; break;
            case ITEM_AFFIX_EFFECT_WEAPON_DMG_PCT:   damagePct += roll.Value; break;
            case ITEM_AFFIX_EFFECT_ATTACK_SPEED_PCT: speedPct += roll.Value; break;
            case ITEM_AFFIX_EFFECT_RESIST:           SetResistance(*t, affix->EffectArg, roll.Value); break;
            case ITEM_AFFIX_EFFECT_BLOCK:            t->Block = base.Block * (100 + roll.Value) / 100; break;
            case ITEM_AFFIX_EFFECT_EQUIP_SPELL:
            case ITEM_AFFIX_EFFECT_HIT_SPELL:        AddItemSpell(*t, affix->Effect, affix->EffectArg, tier->MinValue); break;
            default: break;
        }
    }

    t->Armor = base.Armor * (100 + armorPct) / 100 + armorFlat;
    if (damagePct)
        for (_Damage& damage : t->Damage)
        {
            damage.DamageMin = std::floor(damage.DamageMin * (100 + damagePct) / 100.0f);
            damage.DamageMax = std::ceil(damage.DamageMax * (100 + damagePct) / 100.0f);
        }
    if (speedPct && t->Delay)
        t->Delay = std::max<uint32>(1000, (t->Delay * 100 / (100 + speedPct)) / 10 * 10);

    std::string name = base.Name1;
    if (bestTier[ITEM_AFFIX_PREFIX])
        name = bestTier[ITEM_AFFIX_PREFIX]->Name + " " + name;
    if (bestTier[ITEM_AFFIX_SUFFIX])
        name += " " + bestTier[ITEM_AFFIX_SUFFIX]->Name;
    t->Name1 = name;

    std::size_t count = rolls.size();
    if (quality < MAX_ITEM_QUALITY)
    {
        // Quality follows the base (or a promotion); power comes from its budget, so the item keeps the
        // base's item level and level requirement. Tiers only name the affixes.
        t->Quality = quality;
        t->ItemLevel = std::max<uint32>(base.ItemLevel, 1);
        t->RequiredLevel = std::min<uint32>(base.RequiredLevel, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
    }
    else
    {
        // Pre-budget rows: quality from the affix count, level from the mob and tiers.
        t->Quality = count <= 2 ? ITEM_QUALITY_UNCOMMON : count <= 4 ? ITEM_QUALITY_RARE : ITEM_QUALITY_EPIC;
        t->ItemLevel = std::max<uint32>(itemLevel, 1);
        t->RequiredLevel = std::min<uint32>(std::max<uint32>(base.RequiredLevel, requiredLevel), sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
    }
    // Bots destroy items worth nothing first, so a generated item is always worth something.
    t->SellPrice = std::max<uint32>(1, base.SellPrice * (4 + count) / 4);
    t->BuyPrice = std::max<int32>(1, base.BuyPrice * int32(4 + count) / 4);
    return t;
}

ItemAffixPool* ForgeMgr::FindPool(uint32 entry)
{
    for (ItemAffixPool& pool : _pools)
        if (entry >= pool.FirstEntry && entry < pool.FirstEntry + pool.Count)
            return &pool;
    return nullptr;
}

std::pair<uint32, ItemAffixPool*> ForgeMgr::AllocateEntry(ItemTemplate const* base, bool& reused)
{
    auto itr = _poolsByKey.find(MakePoolKey(base));
    if (itr == _poolsByKey.end())
        return { 0, nullptr };

    // Prefer rows drawn with the base's own model, then rows without a model, then any.
    std::vector<ItemAffixPool*> order;
    for (int pass = 0; pass < 3; ++pass)
        for (std::size_t index : itr->second)
        {
            ItemAffixPool* pool = &_pools[index];
            bool match = pass == 0 ? pool->DisplayId == base->DisplayInfoID
                : pass == 1 ? pool->DisplayId == 0
                : pool->DisplayId != 0 && pool->DisplayId != base->DisplayInfoID;
            if (match)
                order.push_back(pool);
        }

    for (ItemAffixPool* pool : order)
        if (pool->NextUnused < pool->FirstEntry + pool->Count)
        {
            reused = false;
            return { pool->NextUnused++, pool };
        }
    for (ItemAffixPool* pool : order)
        if (!pool->Free.empty())
        {
            uint32 entry = pool->Free.front();
            pool->Free.pop_front();
            reused = true;
            return { entry, pool };
        }
    return { 0, nullptr };
}

uint32 ForgeMgr::Generate(ItemTemplate const* base, uint8 itemLevel, bool force, uint8 prefixes, uint8 suffixes,
    uint8 quality)
{
    if (!base || (!force && !IsEligible(base)) || !GetItemMask(base))
        return 0;

    if (quality >= MAX_ITEM_QUALITY)
        quality = _budgetMode ? uint8(base->Quality) : ITEM_AFFIX_QUALITY_UNSET;
    std::vector<ItemAffixRoll> rolls = Roll(base, itemLevel, prefixes, suffixes, quality);
    if (rolls.empty())
        return 0;

    std::unique_lock lock(_lock);
    bool reused = false;
    auto [entry, pool] = AllocateEntry(base, reused);
    if (!entry)
    {
        if (!_warnedNoPool)
        {
            LOG_WARN("module.forge", "No free Item.dbc entry for base item {} (pools exhausted or missing). "
                "Grow item_affix_pool and rebuild the client patch.", base->ItemId);
            _warnedNoPool = true;
        }
        return 0;
    }

    auto item = std::make_unique<GeneratedItem>();
    item->Entry = entry;
    item->BaseEntry = base->ItemId;
    item->ItemLevel = itemLevel;
    item->Quality = quality;
    item->Rolls = std::move(rolls);
    item->Template = BuildTemplate(entry, *base, itemLevel, item->Rolls, pool->DisplayId, quality);
    SaveGenerated(*item, *pool);
    _generated[entry] = std::move(item);

    if (reused)
        LOG_DEBUG("module.forge", "Reused entry {} for base {}", entry, base->ItemId);
    return entry;
}

uint32 ForgeMgr::CreatePreview(ItemTemplate const* base, uint8 itemLevel, std::string const& note)
{
    if (!_enabled || !base || !GetItemMask(base))
        return 0;

    std::unique_lock lock(_lock);
    bool reused = false;
    auto [entry, pool] = AllocateEntry(base, reused);
    if (!entry)
        return 0;

    auto item = std::make_unique<GeneratedItem>();
    item->Entry = entry;
    item->BaseEntry = base->ItemId;
    item->ItemLevel = itemLevel;
    item->Quality = uint8(std::min<uint32>(base->Quality, ITEM_QUALITY_LEGENDARY));
    item->Preview = true;
    item->Template = BuildTemplate(entry, *base, itemLevel, {}, pool->DisplayId, item->Quality);
    // The note is the only hint: the client showed no "<Random enchantment>" line for a template random property
    // or suffix group on a quest reward (tried both, 2026-10-14)
    item->Template->Description = note;
    _generated[entry] = std::move(item);

    // Only the pool's counter is saved, so the entry isn't handed out twice while this server runs
    CharacterDatabase.Execute("REPLACE INTO item_affix_pool_state (pool_id, next_unused) VALUES ({}, {})", pool->Id, pool->NextUnused);
    return entry;
}

void ForgeMgr::ReleasePreview(uint32 entry)
{
    std::unique_lock lock(_lock);
    auto itr = _generated.find(entry);
    if (itr == _generated.end() || !itr->second->Preview)
        return;

    // Keep the template like any retired one: a pointer to it may still be held
    _retired.push_back(std::move(itr->second->Template));
    _generated.erase(itr);
    if (ItemAffixPool* pool = FindPool(entry))
        pool->Free.push_back(entry);
}

void ForgeMgr::SaveGenerated(GeneratedItem const& item, ItemAffixPool const& pool) const
{
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    trans->Append("DELETE FROM item_generated_affix WHERE entry = {}", item.Entry);
    if (item.Quality < MAX_ITEM_QUALITY)
        trans->Append("REPLACE INTO item_generated (entry, base_entry, item_level, quality) VALUES ({}, {}, {}, {})",
            item.Entry, item.BaseEntry, item.ItemLevel, item.Quality);
    else
        trans->Append("REPLACE INTO item_generated (entry, base_entry, item_level) VALUES ({}, {}, {})",
            item.Entry, item.BaseEntry, item.ItemLevel);
    for (std::size_t i = 0; i < item.Rolls.size(); ++i)
        trans->Append("INSERT INTO item_generated_affix (entry, idx, affix_id, tier, value, grade) VALUES ({}, {}, {}, {}, {}, {})",
            item.Entry, i, item.Rolls[i].AffixId, item.Rolls[i].Tier, item.Rolls[i].Value, item.Rolls[i].Grade);
    trans->Append("REPLACE INTO item_affix_pool_state (pool_id, next_unused) VALUES ({}, {})", pool.Id, pool.NextUnused);
    CharacterDatabase.CommitTransaction(trans);
}

void ForgeMgr::SendTemplate(Player* player, uint32 entry) const
{
    // A reused entry may sit stale in the client's ItemCache.wdb: answer the query unasked.
    WorldPacket query(CMSG_ITEM_QUERY_SINGLE, 4);
    query << entry;
    player->GetSession()->HandleItemQuerySingleOpcode(query);
}

void ForgeMgr::OnLootFilled(Loot& loot, LootStore const& store, Player* lootOwner, WorldObject* lootSource)
{
    if (!_enabled || !lootOwner || _pools.empty())
        return;

    uint8 itemLevel;
    if (&store == &LootTemplates_Creature && lootSource && lootSource->IsCreature())
        itemLevel = lootSource->ToCreature()->GetLevel();
    else if (&store == &LootTemplates_Gameobject)
        itemLevel = lootOwner->GetLevel();
    else
        return;

    std::vector<uint32> created;
    for (LootItem& item : loot.items)
    {
        if (item.needs_quest)
            continue;
        ItemTemplate const* base = sObjectMgr->GetItemTemplate(item.itemid);
        if (!IsEligible(base) || !roll_chance_f(_chance))
            continue;
        uint8 quality = ITEM_AFFIX_QUALITY_UNSET;
        if (_budgetMode && base->Quality == ITEM_QUALITY_EPIC && roll_chance_f(_legendaryChance))
            quality = ITEM_QUALITY_LEGENDARY;
        if (uint32 entry = Generate(base, itemLevel, false, 0, 0, quality))
        {
            item.itemid = entry;
            item.randomPropertyId = 0;
            item.randomSuffix = 0;
            created.push_back(entry);
        }
    }

    // Uniques: a separate roll per kill of their creature, added to the normal loot.
    if (&store == &LootTemplates_Creature && lootSource && lootSource->IsCreature())
    {
        auto range = _uniqueByCreature.equal_range(lootSource->ToCreature()->GetEntry());
        for (auto itr = range.first; itr != range.second; ++itr)
            if (roll_chance_f(itr->second->Chance))
            {
                loot.AddItem(LootStoreItem(itr->second->Entry, 0, 100.0f, false, LOOT_MODE_DEFAULT, 0, 1, 1));
                created.push_back(itr->second->Entry);
            }
    }

    if (created.empty())
        return;

    // Push the new templates to everyone who can see this loot, so reused entries never show
    // a stale tooltip. New entries cost one packet each.
    std::vector<Player*> viewers { lootOwner };
    if (Group* group = lootOwner->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member != lootOwner && member->IsInMap(lootOwner))
                    viewers.push_back(member);
    for (Player* viewer : viewers)
        if (viewer->GetSession())
            for (uint32 entry : created)
                SendTemplate(viewer, entry);
}
