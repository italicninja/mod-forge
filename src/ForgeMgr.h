/*
 * This file is part of the mod-forge module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 * Formerly core patches 0001-item-affixes and 0006-item-affix-spells (wowproject).
 */

#ifndef MOD_FORGE_MGR_H
#define MOD_FORGE_MGR_H

#include "Define.h"
#include "ItemTemplate.h"
#include <deque>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Item;
class Loot;
class LootStore;
class Player;
class WorldObject;

// PoE-style loot (wowproject docs/loot-affixes.md). An eligible drop becomes a new ItemTemplate: the base
// item stripped to armor/damage, plus 1-3 prefixes and 1-3 suffixes rolled by item level.
// Generated entries come from pools reserved in the client's Item.dbc (patch-enUS-4.MPQ).

enum ItemAffixKind : uint8
{
    ITEM_AFFIX_PREFIX = 0,
    ITEM_AFFIX_SUFFIX = 1,
};

enum ItemAffixEffect : uint8
{
    ITEM_AFFIX_EFFECT_STAT             = 0, // ItemStat of type arg (ItemModType)
    ITEM_AFFIX_EFFECT_ARMOR            = 1, // flat armor
    ITEM_AFFIX_EFFECT_ARMOR_PCT        = 2, // % of base armor
    ITEM_AFFIX_EFFECT_WEAPON_DMG_PCT   = 3, // % of base weapon damage
    ITEM_AFFIX_EFFECT_ATTACK_SPEED_PCT = 4, // % faster swing (lower Delay)
    ITEM_AFFIX_EFFECT_RESIST           = 5, // arg = SpellSchools
    ITEM_AFFIX_EFFECT_BLOCK            = 6, // shield block value
    // Spell affixes: the tier's min_value is a Spell.dbc id, so the client draws the tooltip line itself.
    // The highest tier the item level unlocks rolls; the affix takes a budget share without a value.
    ITEM_AFFIX_EFFECT_EQUIP_SPELL      = 7, // "Equip:"; arg = InventoryType bitmask (1 << type), 0 = any slot
    ITEM_AFFIX_EFFECT_HIT_SPELL        = 8, // "Chance on hit:"; arg = procs per minute x 10
    MAX_ITEM_AFFIX_EFFECT
};

// item_affix.item_mask
enum ItemAffixMask : uint32
{
    ITEM_AFFIX_MASK_WEAPON_1H = 0x001,
    ITEM_AFFIX_MASK_WEAPON_2H = 0x002,
    ITEM_AFFIX_MASK_RANGED    = 0x004, // bows, guns, crossbows, thrown, wands
    ITEM_AFFIX_MASK_SHIELD    = 0x008,
    ITEM_AFFIX_MASK_HOLDABLE  = 0x010, // off-hand frills
    ITEM_AFFIX_MASK_CLOTH     = 0x020,
    ITEM_AFFIX_MASK_LEATHER   = 0x040,
    ITEM_AFFIX_MASK_MAIL      = 0x080,
    ITEM_AFFIX_MASK_PLATE     = 0x100,
    ITEM_AFFIX_MASK_JEWELRY   = 0x200, // neck, finger, trinket
    ITEM_AFFIX_MASK_CLOAK     = 0x400,
};

struct ItemAffixTier
{
    uint8 Tier;
    uint8 MinItemLevel;
    uint8 RequiredLevel;
    int32 MinValue;
    int32 MaxValue;
    uint32 Weight;
    std::string Name;
};

struct ItemAffix
{
    uint32 Id;
    ItemAffixKind Kind;
    uint32 Group;
    ItemAffixEffect Effect;
    int32 EffectArg;
    uint32 ItemMask;
    uint32 Weight;
    std::vector<ItemAffixTier> Tiers; // ascending MinItemLevel

    ItemAffixTier const* GetTier(uint8 tier) const;
};

// Affix grades (Forge.AffixGrades, PoE-style): each affix rolls its own grade, 1 best .. 4 worst, and a stat affix
// gets that grade's share of a green item's stat power, so more affixes mean more power. 0: a roll from before
// grades (budget split or plain tier value) or a spell affix.
constexpr uint8 ITEM_AFFIX_GRADES = 4;

struct ItemAffixRoll
{
    uint32 AffixId;
    uint8 Tier;
    int32 Value;
    uint8 Grade = 0;
};

// A range of entries reserved in the client's Item.dbc. Every row in the range carries the same
// class/subclass/sound/material/inventory type/sheath (and display, if non-zero), so a
// generated item must come from a pool matching its base on those fields.
struct ItemAffixPool
{
    uint32 Id;
    uint32 FirstEntry;
    uint32 Count;
    uint32 DisplayId; // 0: rows copy no particular display
    uint32 NextUnused;
    std::deque<uint32> Free; // entries < NextUnused that no item uses (oldest first)
};

// item_generated.flags
enum GeneratedItemFlags : uint8
{
    GENERATED_ITEM_FLAG_CORRUPTED = 0x01, // Felfire Orb: no orb works on it any more
};

// Crafting orbs (docs/forge-currency.md), world table forge_currency.kind
enum ForgeOrbKind : uint8
{
    FORGE_ORB_TRANSMUTE,  // Shard of Awakening: plain item -> forged, fewest affixes for its quality
    FORGE_ORB_ALTERATION, // Shifting Prism: reroll an Uncommon (or lower) forged item
    FORGE_ORB_AUGMENT,    // Mote of Augmentation: add an affix to an Uncommon (or lower) forged item with room
    FORGE_ORB_SCOUR,      // Scouring Dust: forged -> plain base
    FORGE_ORB_ALCHEMY,    // Alchemist's Catalyst: plain item -> forged, most affixes for its quality
    FORGE_ORB_CHANCE,     // Gambler's Coin: plain item -> forged at a random quality, rarely Legendary or its Unique
    FORGE_ORB_CHAOS,      // Orb of Upheaval: reroll a Rare or better forged item
    FORGE_ORB_REGAL,      // Sovereign Orb: Uncommon -> Rare or Rare -> Epic, plus an affix
    FORGE_ORB_ANNUL,      // Void Shard: remove a random affix
    FORGE_ORB_VAAL,       // Felfire Orb: one of four outcomes, then Corrupted (forged items and Uniques)
    FORGE_ORB_DIVINE,     // Tear of the Naaru: same affixes, new values (forged items and Uniques)
    FORGE_ORB_EXALT,      // Eye of the Titans: add an affix to a Rare or better forged item with room
    FORGE_ORB_ESSENCE,    // Essence: reroll with affix arg guaranteed at grade 1 (plain or forged)
    MAX_FORGE_ORB
};

struct ForgeOrb
{
    uint32 SpellId;
    uint32 ItemEntry;
    ForgeOrbKind Kind;
    int32 Arg;
    uint32 DropWeight;
};

// Generated item quality when none is stored yet (rows from before quality was saved): rebuilt from the base.
constexpr uint8 ITEM_AFFIX_QUALITY_UNSET = 0xFF;

struct GeneratedItem
{
    uint32 Entry;
    uint32 BaseEntry;
    uint8 ItemLevel;
    uint8 Quality = ITEM_AFFIX_QUALITY_UNSET;
    std::vector<ItemAffixRoll> Rolls;
    std::unique_ptr<ItemTemplate> Template;
    bool Preview = false; // CreatePreview: shown as a reward, never handed out, not saved
    uint8 Flags = 0;       // GeneratedItemFlags
    // A Unique given its own entry by an orb (Tear of the Naaru, Felfire Orb): Rolls hold its stats' values,
    // Tier = index into the Unique's Stats, AffixId 0.
    std::string UniqueKey;
};

// A hand-authored Unique (quality 6, light gold; docs/uniques-draft.md): fixed stats on a base look, dropped
// only by its creature. Its entry is taken once from the base look's pool and kept in item_unique_state.
struct UniqueItem
{
    std::string Key;
    uint32 Entry = 0;
    uint32 BaseEntry = 0;
    uint8 ItemLevel = 0;
    uint8 RequiredLevel = 0;
    std::string Name;
    std::string Description;
    uint32 CreatureEntry = 0;
    float Chance = 0.0f; // % per kill
    std::vector<std::tuple<ItemAffixEffect, int32, int32>> Stats; // effect, arg, value
    std::unique_ptr<ItemTemplate> Template;
};

class ForgeMgr
{
public:
    static ForgeMgr* instance();

    // World startup (WorldScript::OnAfterItemTemplatesLoad, core patch 0001): before anything loads item instances.
    void Load();

    // Generated template for entry, or nullptr. Thread-safe; returned pointers stay valid
    // until shutdown (templates are never freed while the server runs).
    ItemTemplate const* GetTemplate(uint32 entry) const;
    bool IsGenerated(uint32 entry) const;
    GeneratedItem const* GetGenerated(uint32 entry) const; // read-only snapshot pointer
    ItemAffix const* GetAffix(uint32 id) const;

    // After a loot template fills a loot (MiscScript::OnAfterLootTemplateProcess): turn eligible items in a
    // creature/chest loot into generated items. lootSource is the creature for creature loot.
    void OnLootFilled(Loot& loot, LootStore const& store, Player* lootOwner, WorldObject* lootSource);

    // Roll a new item from base at itemLevel. prefixes/suffixes force the affix counts (1-3);
    // 0 rolls them (by quality: Forge.Count.*). quality overrides the base's quality
    // (e.g. ITEM_QUALITY_LEGENDARY for a promoted drop). Returns the new entry, or 0 (not
    // eligible, no free pool entry, nothing rolled; a 0-affix roll keeps the plain item).
    uint32 Generate(ItemTemplate const* base, uint8 itemLevel, bool force = false, uint8 prefixes = 0, uint8 suffixes = 0,
        uint8 quality = ITEM_AFFIX_QUALITY_UNSET);

    // Orb of Upheaval (docs/forge-currency.md): reroll every affix of a generated item under the same entry, keeping
    // its base, item level and quality (the count rerolls within the quality's range). The old template is retired.
    // False if entry isn't a generated item (Uniques and previews aren't). World thread.
    bool Reroll(uint32 entry);
    // Reroll player's item in place: an equipped item loses the old template's stats and gains the new ones. Sends the
    // new template to player.
    bool RerollItem(Player* player, Item* item);

    // Crafting orbs (ForgeCurrency.cpp). GetOrb: the orb whose Use: spell is spellId, or nullptr.
    ForgeOrb const* GetOrb(uint32 spellId) const;
    // Why orb can't be used on item ("" = it can). Any thread.
    std::string CheckOrb(ForgeOrb const& orb, Player* player, Item* item) const;
    // Use orb on player's item (checked with CheckOrb first). Orbs that replace the item do it after the cast, on
    // player's next update. Tells player what happened. Map thread.
    void UseOrb(ForgeOrb const& orb, Player* player, Item* item);

    // A reward preview of base (e.g. a generated quest's reward choice): the base stripped like a roll with no
    // affixes, "<Random enchantment>" and note in the tooltip. Takes a pool entry but isn't saved, so the entry is
    // free again after a restart; ReleasePreview frees it earlier. Returns the entry, or 0. World thread.
    uint32 CreatePreview(ItemTemplate const* base, uint8 itemLevel, std::string const& note);
    void ReleasePreview(uint32 entry);

    // Vanilla-style stat budget (docs/loot-vanilla-plan.md): (sum (value * cost)^1.5)^(2/3).
    float BaseBudget(ItemTemplate const* base) const;
    float EstimateBudget(uint32 itemLevel, uint32 inventoryType, uint32 quality) const;
    float GetLegendaryChance() const { return _legendaryChance; }

    // WorldScript::OnUpdate: every Forge.RecycleIntervalMinutes, return the entries of generated items that no
    // item_instance references and that are older than Forge.RecycleGraceMinutes to their pools.
    void Update(uint32 diff);

    UniqueItem const* GetUnique(std::string const& key) const;
    std::vector<UniqueItem const*> GetUniques() const;

    bool IsEnabled() const { return _enabled; }
    // Send entry's template to player (an unsolicited item query response), so a reused entry never shows a
    // stale tooltip from the client's cache.
    void SendTemplate(Player* player, uint32 entry) const;
    bool IsEligible(ItemTemplate const* base) const;
    static uint32 GetItemMask(ItemTemplate const* proto);

private:
    ForgeMgr() = default;

    using PoolKey = std::tuple<uint32, uint32, int32, int32, uint32, uint32>; // class, subclass, sound, material, invtype, sheath
    static PoolKey MakePoolKey(ItemTemplate const* proto);

    void LoadConfig();
    void LoadAffixes();
    void LoadPools();
    void LoadGenerated();
    void LoadUniqueDefs();   // definitions and saved entries; before LoadGenerated (their entries aren't free)
    void LoadCurrency();
    void FinishUniques();    // allocate missing entries, build templates; after LoadGenerated

    std::vector<ItemAffixRoll> Roll(ItemTemplate const* base, uint8 itemLevel, uint8 prefixes, uint8 suffixes, uint8 quality) const;
    static uint8 RollGrade(); // 1..ITEM_AFFIX_GRADES by the grade weights
    // Grade mode: a green item's stat budget for base's level and slot, from its own stats if it has any.
    float GreenBudget(ItemTemplate const* base) const;
    // Grade mode: set roll's value from its grade (a random share within the grade's band of greenBudget).
    void SetGradeValue(ItemAffixRoll& roll, ItemAffix const& affix, ItemAffixTier const& tier, float greenBudget) const;
    // Budget mode: set the values of budgeted rolls (stats, resistances, flat armor) from budget.
    void SplitBudget(std::vector<ItemAffixRoll>& rolls, float budget) const;
    float ItemBudget(ItemTemplate const* base, uint8 quality) const;
    std::unique_ptr<ItemTemplate> BuildTemplate(uint32 entry, ItemTemplate const& base, uint8 itemLevel,
        std::vector<ItemAffixRoll> const& rolls, uint32 displayOverride, uint8 quality) const;
    // A Unique's template at entry; values (a Unique-derived item's rolls) override its stats' values.
    std::unique_ptr<ItemTemplate> BuildUniqueTemplate(UniqueItem const& unique, ItemTemplate const& base, uint32 entry,
        uint32 displayOverride, std::vector<ItemAffixRoll> const* values) const;
    // The template for a generated item: affixes or Unique-derived, plus the Corrupted line.
    std::unique_ptr<ItemTemplate> MakeTemplate(GeneratedItem const& item, ItemTemplate const& base, uint32 displayOverride) const;
    UniqueItem const* FindUniqueDef(std::string const& key) const;
    // Allocate an entry for base and save a new generated item. Returns the entry, or 0 (no free pool entry).
    uint32 CreateGenerated(ItemTemplate const* base, uint8 itemLevel, uint8 quality, std::vector<ItemAffixRoll> rolls,
        std::string const& uniqueKey = "", uint8 flags = 0);
    // Orbs (ForgeCurrency.cpp). Caller holds _lock exclusively for the Locked ones.
    void RebuildLocked(GeneratedItem& item);
    // Affixes that may still be added to rolls (kinds with room, groups not used, slot and item level fit).
    std::vector<ItemAffix const*> AffixCandidates(std::vector<ItemAffixRoll> const& rolls, ItemTemplate const* base,
        uint8 itemLevel, uint32 onlyAffix = 0) const;
    // Add one affix (onlyAffix, if set) at grade (0: rolled). False if nothing fits.
    bool AddRoll(std::vector<ItemAffixRoll>& rolls, ItemTemplate const* base, uint8 itemLevel, uint32 onlyAffix = 0,
        uint8 grade = 0) const;
    // count affixes (the first is onlyAffix at grade 1, if set).
    std::vector<ItemAffixRoll> RollCount(ItemTemplate const* base, uint8 itemLevel, uint32 count, uint32 onlyAffix = 0) const;
    void RerollValue(ItemAffixRoll& roll, ItemTemplate const* base) const;
    // Caller holds _lock exclusively. Returns {entry, pool}.
    std::pair<uint32, ItemAffixPool*> AllocateEntry(ItemTemplate const* base, bool& reused);
    ItemAffixPool* FindPool(uint32 entry);
    std::unordered_set<uint32> CollectHeldEntries() const;
    void SaveGenerated(GeneratedItem const& item, ItemAffixPool const& pool) const;

    bool _enabled = false;
    float _chance = 100.0f;
    uint32 _minQuality = ITEM_QUALITY_UNCOMMON;
    uint32 _maxQuality = ITEM_QUALITY_RARE;
    std::vector<uint32> _prefixWeights; // [n-1] = weight of rolling n prefixes
    std::vector<uint32> _suffixWeights;
    // Quality follows the base item; affix count range per quality; values split the item's stat budget.
    bool _budgetMode = true;
    bool _affixGrades = true; // with the ladder: each affix rolls a grade instead of splitting the base's budget
    uint8 _countMin[MAX_ITEM_QUALITY] = { 0, 0, 1, 2, 3, 4, 6, 0 };
    uint8 _countMax[MAX_ITEM_QUALITY] = { 1, 1, 2, 3, 4, 6, 6, 0 };
    float _qualityBudget[MAX_ITEM_QUALITY] = { 0.25f, 0.5f, 1.0f, 1.35f, 1.7f, 2.2f, 2.2f, 1.0f }; // x green
    float _budgetPerLevel = 0.45f; // green big-slot budget ~ perLevel * ilvl + offset (audit 2026-10-13)
    float _budgetOffset = -3.0f;
    float _legendaryChance = 0.5f; // % of eligible purple drops promoted to Legendary
    float _epicChance = 0.0f;      // % of eligible blue drops promoted to Epic, from creature level _epicMinLevel
    uint32 _epicMinLevel = 21;
    bool _uniquesEnabled = true;   // Uniques drop from their creatures and from Gambler's Coins
    uint32 _recycleIntervalMs = 30 * 60 * 1000;
    uint32 _recycleGraceMinutes = 120; // longer than PlayerSaveInterval, so items held in memory are saved first
    uint32 _recycleTimer = 0;
    float _legendaryBudget = 1.3f; // x the base item's budget

    std::unordered_map<uint32, ItemAffix> _affixes;
    std::vector<ItemAffixPool> _pools;
    std::map<PoolKey, std::vector<std::size_t>> _poolsByKey; // indexes into _pools

    // Every generated entry lies in [_minEntry, _maxEntry); fixed after Load, read without the lock
    // so GetItemTemplate misses (entry 0, empty slots) stay cheap.
    uint32 _minEntry = 0;
    uint32 _maxEntry = 0;

    // Uniques are fixed after Load and read without the lock.
    std::vector<std::unique_ptr<UniqueItem>> _uniques;
    std::unordered_map<uint32, UniqueItem*> _uniqueByEntry;
    std::unordered_multimap<uint32, UniqueItem*> _uniqueByCreature;

    mutable std::shared_mutex _lock; // guards _generated and pool allocation state
    std::unordered_map<uint32, std::unique_ptr<GeneratedItem>> _generated;
    // Templates of recycled entries. Kept until shutdown, so pointers from GetTemplate never dangle.
    std::vector<std::unique_ptr<ItemTemplate>> _retired;
    bool _warnedNoPool = false;

    // Crafting orbs: fixed after Load, read without the lock.
    std::unordered_map<uint32, ForgeOrb> _orbs; // by spell id
    bool _currencyEnabled = true;
    float _orbDropChance = 2.0f;      // % per kill
    float _orbEliteMultiplier = 3.0f;
    float _orbBossMultiplier = 10.0f;
    float _orbDisenchantChance = 50.0f; // % per disenchanted forged item
    uint32 PickOrbEntry() const;      // a random orb item by drop weight, or 0
};

#define sForgeMgr ForgeMgr::instance()

#endif
