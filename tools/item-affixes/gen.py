#!/usr/bin/env python3
"""Generate the PoE-style affix data and the Item.dbc entry pools (docs/loot-affixes.md).

    gen.py affixes > sql/world/2026_10_13_01_item_affix_data.sql
    gen.py pools bases.tsv pools.tsv > sql/world/2026_10_13_02_item_affix_pool.sql

bases.tsv is the eligible base items, one row each (see fetch-bases.sh):
    entry class subclass sound material display inventory_type sheath
pools.tsv is what tools/client-patch/build-patch.py --pools expands into Item.dbc rows. The SQL
and the client patch must come from the same pools.tsv, or generated items get the wrong
model or can't be equipped.
"""
import sys

# ItemAffixMask
W1H, W2H, RANGED, SHIELD, HOLD = 0x001, 0x002, 0x004, 0x008, 0x010
CLOTH, LEATHER, MAIL, PLATE, JEWEL, CLOAK = 0x020, 0x040, 0x080, 0x100, 0x200, 0x400
WEAPON = W1H | W2H | RANGED
BODY = CLOTH | LEATHER | MAIL | PLATE
ALL = WEAPON | SHIELD | HOLD | BODY | JEWEL | CLOAK
ARMORED = BODY | CLOAK | SHIELD

# effect
STAT, ARMOR, ARMOR_PCT, WDMG_PCT, SPEED_PCT, RESIST, BLOCK_PCT, EQUIP_SPELL, HIT_SPELL = range(9)
PREFIX, SUFFIX = 0, 1

# Item level gates and required level per tier (7 tiers).
TIER_ILVL = [1, 10, 20, 30, 40, 50, 58]
TIER_REQ = [1, 8, 16, 24, 32, 40, 46]
TIER_WEIGHT = [100, 100, 90, 80, 70, 60, 50]  # PoE: the best tiers are the rarest
# Primary-stat value ranges on a chest piece; other affixes scale from these. The server
# multiplies flat values by the slot factor (chest 1.0 ... ranged 0.32).
PRIMARY = [(1, 2), (3, 4), (5, 6), (7, 8), (9, 10), (11, 12), (13, 15)]


def scaled(mult):
    return [(max(1, round(lo * mult)), max(1, round(hi * mult))) for lo, hi in PRIMARY]


# (kind, effect, arg, mask, weight, ranges, names, comment). ranges/names per tier, lowest first.
AFFIXES = [
    # ---- prefixes
    (PREFIX, STAT, 38, WEAPON | LEATHER | MAIL | PLATE | JEWEL | CLOAK, 100, scaled(2.0),
     ["Squire's", "Journeyman's", "Reaver's", "Mercenary's", "Champion's", "Conqueror's", "Warlord's"],
     "attack power"),
    (PREFIX, STAT, 45, ALL, 100, scaled(1.15),
     ["Apprentice's", "Adept's", "Sorcerer's", "Thaumaturge's", "Wizard's", "Magus's", "Archmage's"],
     "spell power"),
    (PREFIX, STAT, 1, ALL, 80, scaled(10),
     ["Healthy", "Sanguine", "Stalwart", "Stout", "Robust", "Rotund", "Virile"], "health"),
    (PREFIX, STAT, 0, ALL, 80, scaled(15),
     ["Beryl", "Cobalt", "Azure", "Sapphire", "Cerulean", "Aqua", "Opalescent"], "mana"),
    (PREFIX, ARMOR, 0, ARMORED, 100, scaled(6),
     ["Lacquered", "Studded", "Ribbed", "Fortified", "Plated", "Carapaced", "Encased"], "flat armor"),
    (PREFIX, ARMOR_PCT, 0, ARMORED, 100,
     [(5, 9), (10, 14), (15, 19), (20, 24), (25, 29), (30, 34), (35, 40)],
     ["Reinforced", "Layered", "Lobstered", "Buttressed", "Thickened", "Girded", "Impregnable"], "armor %"),
    (PREFIX, WDMG_PCT, 0, WEAPON, 120,
     [(5, 9), (10, 14), (15, 19), (20, 24), (25, 29), (30, 34), (35, 40)],
     ["Heavy", "Serrated", "Wicked", "Vicious", "Bloodthirsty", "Cruel", "Tyrannical"], "weapon damage %"),
    (PREFIX, BLOCK_PCT, 0, SHIELD, 80,
     [(10, 15), (16, 22), (23, 29), (30, 36), (37, 43), (44, 50), (51, 60)],
     ["Steady", "Braced", "Sturdy", "Unwavering", "Unyielding", "Immovable", "Adamant"], "block value %"),
    # ---- suffixes
    (SUFFIX, STAT, 4, ALL, 100, scaled(1.0),
     ["of the Brute", "of the Wrestler", "of the Bear", "of the Lion", "of the Gorilla", "of the Goliath",
      "of the Titan"], "strength"),
    (SUFFIX, STAT, 3, ALL, 100, scaled(1.0),
     ["of the Mongoose", "of the Lynx", "of the Fox", "of the Falcon", "of the Panther", "of the Leopard",
      "of the Jaguar"], "agility"),
    (SUFFIX, STAT, 7, ALL, 100, scaled(1.0),
     ["of the Ox", "of the Boar", "of the Whale", "of the Rhino", "of the Mammoth", "of the Kodo",
      "of the Behemoth"], "stamina"),
    (SUFFIX, STAT, 5, ALL, 100, scaled(1.0),
     ["of the Pupil", "of the Student", "of the Prodigy", "of the Augur", "of the Philosopher", "of the Savant",
      "of the Virtuoso"], "intellect"),
    (SUFFIX, STAT, 6, ALL, 100, scaled(1.0),
     ["of the Owl", "of the Dove", "of the Monk", "of the Sage", "of the Saint", "of the Prophet",
      "of the Ascended"], "spirit"),
    (SUFFIX, STAT, 32, ALL, 80, scaled(0.8),
     ["of Menace", "of Havoc", "of Disaster", "of Calamity", "of Ruin", "of Unmaking", "of Devastation"],
     "crit rating"),
    (SUFFIX, STAT, 31, ALL, 80, scaled(0.8),
     ["of Precision", "of Accuracy", "of the Marksman", "of the Sharpshooter", "of the Deadeye", "of True Aim",
      "of the Assassin"], "hit rating"),
    (SUFFIX, STAT, 36, ALL, 60, scaled(0.8),
     ["of Alacrity", "of Celerity", "of Swiftness", "of Quickness", "of Speed", "of the Tempest",
      "of the Zephyr"], "haste rating"),
    (SUFFIX, STAT, 13, LEATHER | MAIL | PLATE | SHIELD | JEWEL | CLOAK, 60, scaled(0.8),
     ["of Evasion", "of Skirting", "of Elusion", "of the Shadow", "of the Wind", "of the Ghost", "of the Wraith"],
     "dodge rating"),
    (SUFFIX, STAT, 12, MAIL | PLATE | SHIELD | JEWEL | CLOAK, 60, scaled(0.8),
     ["of Defense", "of the Guard", "of the Sentinel", "of the Bulwark", "of the Protector", "of the Champion",
      "of the Defender"], "defense rating"),
    (SUFFIX, STAT, 43, ALL, 60, scaled(0.4),
     ["of Clarity", "of Focus", "of Serenity", "of Meditation", "of Insight", "of Reverie", "of Enlightenment"],
     "mana per 5"),
    (SUFFIX, STAT, 46, ALL, 60, scaled(0.4),
     ["of the Troll", "of Regrowth", "of Recovery", "of Renewal", "of Restoration", "of Rejuvenation",
      "of Vitality"], "health per 5"),
    (SUFFIX, RESIST, 2, ARMORED | JEWEL, 40, scaled(1.5),
     ["of the Brazier", "of the Furnace", "of the Kiln", "of the Volcano", "of the Magma", "of the Inferno",
      "of the Phoenix"], "fire resistance"),
    (SUFFIX, RESIST, 4, ARMORED | JEWEL, 40, scaled(1.5),
     ["of the Seal", "of the Penguin", "of the Walrus", "of the Yeti", "of the Glacier", "of the Wendigo",
      "of the Frost Wyrm"], "frost resistance"),
    (SUFFIX, RESIST, 3, ARMORED | JEWEL, 40, scaled(1.5),
     ["of the Snake", "of the Viper", "of the Asp", "of the Cobra", "of the Basilisk", "of the Hydra",
      "of the Wyvern"], "nature resistance"),
    (SUFFIX, RESIST, 5, ARMORED | JEWEL, 40, scaled(1.5),
     ["of the Lost", "of the Shade", "of the Dark", "of the Night", "of the Void", "of the Abyss",
      "of the Nether"], "shadow resistance"),
    (SUFFIX, RESIST, 6, ARMORED | JEWEL, 40, scaled(1.5),
     ["of Warding", "of the Ward", "of Banishing", "of Abjuration", "of the Arcanum", "of the Magister",
      "of the Leyline"], "arcane resistance"),
    (SUFFIX, SPEED_PCT, 0, WEAPON, 60, [(2, 3), (3, 4), (4, 5), (5, 6), (6, 7), (7, 8), (8, 10)],
     ["of Skill", "of Ease", "of Grace", "of Mastery", "of Renown", "of Acclaim", "of Fame"], "attack speed %"),
]


# Spell affixes (effect EQUIP_SPELL / HIT_SPELL): each tier is an existing Spell.dbc spell, so the client
# draws the "Equip:" / "Chance on hit:" line and no client patch change is needed. The server rolls the
# highest tier the item level unlocks. All share one group: at most one spell affix per item.
# arg: EQUIP_SPELL = InventoryType bitmask (0 = any slot in the mask); HIT_SPELL = procs per minute x 10.
# (kind, effect, arg, mask, weight, [(min_ilvl, spell, name), ...], comment). Values in comments are the
# spell's own numbers (Spell.dbc, checked 2026-10-14).
SPELL_GROUP = 100
MELEE = W1H | W2H


def slots(*types):
    return sum(1 << t for t in types)


HEAD, NECK, SHOULDERS, CHEST, LEGS, FEET, HANDS, FINGER, TRINKET, SHIELD_T, CLOAK_T = 1, 2, 3, 5, 7, 8, 10, 11, 12, 14, 16

SPELL_AFFIXES = [
    # ---- prefixes: weapon procs
    (PREFIX, HIT_SPELL, 20, MELEE, 8,
     [(10, 16408, "Dusky"), (18, 16409, "Shadowed"), (28, 18205, "Umbral"), (40, 18138, "Tenebrous"),
      (52, 18214, "Nightfall")], "proc: shadow bolt 25/35/55-85/110-140/150"),
    (PREFIX, HIT_SPELL, 20, MELEE, 8,
     [(14, 13442, "Smoldering"), (30, 18833, "Searing"), (48, 18086, "Blazing")], "proc: firebolt 35/75-105/150"),
    (PREFIX, HIT_SPELL, 20, MELEE, 8,
     [(12, 16407, "Chilling"), (24, 18398, "Frosted"), (34, 20869, "Frigid"), (44, 18276, "Glacial"),
      (54, 18204, "Rimebound")], "proc: frost blast 30/45/75/90/110-120"),
    (PREFIX, HIT_SPELL, 20, MELEE, 8,
     [(10, 13482, "Crackling"), (20, 14119, "Sparking"), (30, 18089, "Thundering"), (40, 13527, "Stormstruck"),
      (50, 18081, "Stormcaller's")], "proc: lightning bolt 10-20/35/45-75/80-100/91-125"),
    (PREFIX, HIT_SPELL, 15, MELEE, 6,
     [(20, 18084, "Leeching"), (36, 16414, "Vampiric"), (54, 21170, "Soul-Drinking")], "proc: drain life 30/45/100-180"),
    (PREFIX, HIT_SPELL, 20, MELEE, 6,
     [(24, 11791, "Piercing"), (46, 17315, "Sundering")], "proc: armor -100 / -200 x3"),
    # ---- suffixes: equip effects
    (SUFFIX, EQUIP_SPELL, 0, MELEE | RANGED | JEWEL, 10,
     [(10, 20877, "of the Trapper"), (20, 18201, "of the Huntsman"), (30, 14565, "of Beast Slaying"),
      (40, 18109, "of the Beastmaster"), (50, 17482, "of the Beastslayer")], "AP vs beasts 15/24/30/39/48"),
    (SUFFIX, EQUIP_SPELL, 0, MELEE | RANGED | JEWEL, 10,
     [(14, 20606, "of the Vigil"), (30, 18074, "of the Crusade"), (44, 18098, "of the Dawn"),
      (54, 28870, "of the Lightbringer")], "AP vs undead 15/30/45/60"),
    (SUFFIX, EQUIP_SPELL, 0, MELEE | RANGED | JEWEL, 8,
     [(16, 12822, "of the Exorcist"), (30, 20858, "of Demonbane"), (40, 12823, "of the Felhunter"),
      (50, 14097, "of the Demonslayer")], "AP vs demons 18/30/36/45"),
    (SUFFIX, EQUIP_SPELL, slots(CHEST, LEGS, SHOULDERS, SHIELD_T), BODY | SHIELD, 15,
     [(1, 20888, "of Thorns"), (12, 20887, "of Brambles"), (24, 20886, "of Briars"), (36, 20889, "of Spines"),
      (48, 21142, "of Immolation")], "damage shield 1/2/3/4 arcane, 5 fire"),
    (SUFFIX, EQUIP_SPELL, slots(CHEST, LEGS, SHIELD_T), BODY | SHIELD, 10,
     [(28, 13959, "of Static"), (42, 7617, "of Retribution")], "when struck: 35-65 nature / 75-125 shadow"),
    (SUFFIX, EQUIP_SPELL, slots(FEET), BODY, 15, [(20, 23990, "of the Strider")], "run speed +8%"),
    (SUFFIX, EQUIP_SPELL, slots(LEGS, FEET), BODY, 8,
     [(10, 40513, "of the Otter"), (30, 8747, "of the Dolphin")], "swim speed +8% / +15%"),
    (SUFFIX, EQUIP_SPELL, slots(HEAD), BODY, 8, [(20, 11789, "of the Deep")], "water breathing"),
    (SUFFIX, EQUIP_SPELL, slots(HEAD, HANDS), BODY, 8,
     [(1, 15956, "of the Angler"), (10, 7823, "of the Fisherman"), (20, 7825, "of the Netcaster"),
      (30, 7826, "of the Seafarer"), (40, 8082, "of the Master Angler")], "fishing +3/5/15/20/25"),
    (SUFFIX, EQUIP_SPELL, slots(HANDS), BODY, 8, [(1, 9134, "of the Herbalist")], "herbalism +5"),
    (SUFFIX, EQUIP_SPELL, slots(HANDS), BODY, 8, [(1, 12560, "of the Miner")], "mining +5"),
    (SUFFIX, EQUIP_SPELL, slots(HANDS), BODY, 8, [(1, 16718, "of the Skinner")], "skinning +10"),
    (SUFFIX, EQUIP_SPELL, slots(NECK, CLOAK_T), JEWEL | CLOAK, 5, [(1, 57353, "of Learning")], "experience +10%"),
]

def sql_str(s):
    return "'" + s.replace("\\", "\\\\").replace("'", "''") + "'"


def cmd_affixes():
    print("-- Generated by tools/item-affixes/gen.py affixes; edit the script, not this file.")
    print("-- Never change an existing affix's effect: generated items rebuild from these rows.")
    print("DELETE FROM `item_affix_tier`;")
    print("DELETE FROM `item_affix`;")
    rows, tiers = [], []
    for i, (kind, effect, arg, mask, weight, ranges, names, comment) in enumerate(AFFIXES, 1):
        assert len(ranges) == len(names) == len(TIER_ILVL), comment
        rows.append("(%d, %d, %d, %d, %d, %d, %d, %s)" % (i, kind, i, effect, arg, mask, weight, sql_str(comment)))
        for t, ((lo, hi), name) in enumerate(zip(ranges, names)):
            tiers.append("(%d, %d, %d, %d, %d, %d, %d, %s)" % (
                i, t + 1, TIER_ILVL[t], TIER_REQ[t], lo, hi, TIER_WEIGHT[t], sql_str(name)))
    for i, (kind, effect, arg, mask, weight, tiers_, comment) in enumerate(SPELL_AFFIXES, len(AFFIXES) + 1):
        rows.append("(%d, %d, %d, %d, %d, %d, %d, %s)" % (i, kind, SPELL_GROUP, effect, arg, mask, weight, sql_str(comment)))
        for t, (ilvl, spell, name) in enumerate(tiers_):
            tiers.append("(%d, %d, %d, %d, %d, %d, %d, %s)" % (
                i, t + 1, ilvl, max(1, ilvl - 5), spell, spell, 100, sql_str(name)))
    print("INSERT INTO `item_affix` (`id`, `kind`, `grp`, `effect`, `effect_arg`, `item_mask`, `weight`, `comment`) VALUES")
    print(",\n".join(rows) + ";")
    print("INSERT INTO `item_affix_tier` (`affix_id`, `tier`, `min_ilvl`, `req_level`, `min_value`, `max_value`, `weight`, `name`) VALUES")
    print(",\n".join(tiers) + ";")


FIRST_ENTRY = 1000000
POOL_SIZE = 100


def cmd_pools(bases_path, pools_path):
    keys = {}
    for line in open(bases_path):
        line = line.strip()
        if not line or line.startswith("entry"):
            continue
        f = [int(v) for v in line.split("\t")]
        _, cls, sub, sound, mat, disp, inv, sheath = f
        keys.setdefault((cls, sub, sound, mat, disp, inv, sheath), 0)
        keys[(cls, sub, sound, mat, disp, inv, sheath)] += 1

    # Append-only: pools already in pools.tsv keep their id and entry range, because live
    # generated items (and the client patch players have) depend on them. New looks go at the end.
    pools = []
    try:
        for line in open(pools_path):
            if line.strip() and not line.startswith("#"):
                pools.append(tuple(int(v) for v in line.split("\t")))
    except FileNotFoundError:
        pass
    known = {p[3:] for p in pools}
    entry = max([p[1] + p[2] for p in pools], default=FIRST_ENTRY)
    pid = max([p[0] for p in pools], default=0)
    for key in sorted(keys):
        if key in known:
            continue
        pid += 1
        pools.append((pid, entry, POOL_SIZE) + key)
        entry += POOL_SIZE

    out_sql = ["(%s)" % ", ".join(str(v) for v in p) for p in pools]
    out_tsv = ["\t".join(str(v) for v in p) for p in pools]
    total = sum(p[2] for p in pools)
    with open(pools_path, "w") as f:
        f.write("# id\tfirst_entry\tcount\tclass\tsubclass\tsound\tmaterial\tdisplay\tinventory_type\tsheath\n")
        f.write("\n".join(out_tsv) + "\n")

    print("-- Generated by tools/item-affixes/gen.py pools; the client patch is built from the same pools.tsv.")
    print("-- %d pools, %d Item.dbc rows (%d..%d)." % (len(pools), total, FIRST_ENTRY, entry - 1))
    print("DELETE FROM `item_affix_pool`;")
    print("INSERT INTO `item_affix_pool` (`id`, `first_entry`, `count`, `class`, `subclass`, `sound`, `material`, `display`, `inventory_type`, `sheath`) VALUES")
    print(",\n".join(out_sql) + ";")


if __name__ == "__main__":
    if sys.argv[1:2] == ["affixes"]:
        cmd_affixes()
    elif sys.argv[1:2] == ["pools"] and len(sys.argv) == 4:
        cmd_pools(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
