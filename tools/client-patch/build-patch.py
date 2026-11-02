#!/usr/bin/env python3
"""Build the client patch Data/enUS/patch-enUS-4.MPQ.

DBCs live in the client's locale archives (highest stock: patch-enUS-3.MPQ), so the patch must
be a locale patch to win. Item.dbc gets one row per entry of every generated-item pool
(tools/item-affixes/pools.tsv), plus optional extra rows from --items:
    id  class  subclass  sound_override  material  display_id  inventory_type  sheathe_type

The client takes an item's icon, model and equip slot from Item.dbc, not from the server.

--currency forge-currency.json adds the Forge crafting orbs (wowproject docs/forge-currency.md): per orb an Item.dbc
row and a Spell.dbc row (its Use: spell), cloned from a stock spell with named fields changed (spell-columns.tsv:
Spell.dbc field order and SQL types, from the server's spell_dbc table). With --sql, it also writes the server side:
item_template, spell_dbc, spell_script_names and forge_currency rows.

usage: build-patch.py --dbc-dir <server dbc dir> --pools pools.tsv [--items rows.tsv]
                      [--currency forge-currency.json --sql out.sql] --out patch-enUS-4.MPQ
"""
import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mpq  # noqa: E402

ITEM_FIELDS = 8


def read_dbc(path):
    data = open(path, "rb").read()
    magic, nrec, nfield, recsize, strsize = struct.unpack_from("<4s4I", data)
    if magic != b"WDBC":
        raise ValueError(path + ": not a WDBC file")
    recs = data[20:20 + nrec * recsize]
    strings = data[20 + nrec * recsize:20 + nrec * recsize + strsize]
    return nfield, recsize, [recs[i * recsize:(i + 1) * recsize] for i in range(nrec)], strings


def write_dbc(nfield, recsize, rows, strings):
    head = struct.pack("<4s4I", b"WDBC", len(rows), nfield, recsize, len(strings))
    return head + b"".join(rows) + strings


def read_rows(items_tsv, pools_tsv):
    rows = []
    if pools_tsv:
        for line in open(pools_tsv):
            if not line.strip() or line.startswith("#"):
                continue
            _, first, count, cls, sub, sound, mat, disp, inv, sheath = (int(v) for v in line.split("\t"))
            rows += [[e, cls, sub, sound, mat, disp, inv, sheath] for e in range(first, first + count)]
    if items_tsv:
        for line in open(items_tsv):
            line = line.split("#", 1)[0].strip()
            if line:
                rows.append([int(v) for v in line.split()])
    return rows


def build_item_dbc(src, rows_in):
    nfield, recsize, rows, strings = read_dbc(src)
    if nfield != ITEM_FIELDS or recsize != ITEM_FIELDS * 4:
        raise ValueError("unexpected Item.dbc layout: %d fields, %d bytes" % (nfield, recsize))
    by_id = {struct.unpack_from("<I", r)[0]: r for r in rows}
    added = 0
    for vals in rows_in:
        if len(vals) != ITEM_FIELDS:
            raise ValueError("bad row (need %d columns): %s" % (ITEM_FIELDS, vals))
        if vals[0] in by_id:
            raise ValueError("item %d already in Item.dbc" % vals[0])
        by_id[vals[0]] = struct.pack("<8i", *vals)
        added += 1
    out = [by_id[k] for k in sorted(by_id)]
    return write_dbc(nfield, recsize, out, strings), added


SPELL_FIELDS = 234
HERE = os.path.dirname(os.path.abspath(__file__))


def spell_columns():
    cols = [line.rstrip("\n").split("\t") for line in open(os.path.join(HERE, "spell-columns.tsv")) if line.strip()]
    if len(cols) != SPELL_FIELDS:
        raise ValueError("spell-columns.tsv: %d columns, need %d" % (len(cols), SPELL_FIELDS))
    return cols


def build_spell_dbc(src, specs):
    """Returns (Spell.dbc bytes, {spell id: [column values for spell_dbc]})."""
    nfield, recsize, rows, strings = read_dbc(src)
    if nfield != SPELL_FIELDS or recsize != SPELL_FIELDS * 4:
        raise ValueError("unexpected Spell.dbc layout: %d fields, %d bytes" % (nfield, recsize))
    cols = spell_columns()
    index = {name: i for i, (name, _) in enumerate(cols)}
    by_id = {struct.unpack_from("<I", r)[0]: r for r in rows}
    strings = bytearray(strings)

    def text(off):
        return strings[off:strings.index(b"\0", off)].decode() if off else ""

    def add_string(value):
        if not value:
            return 0
        off = len(strings)
        strings.extend(value.encode() + b"\0")
        return off

    sql_rows = {}
    for spec in specs:
        sid = spec["id"]
        if sid in by_id:
            raise ValueError("spell %d already in Spell.dbc" % sid)
        vals = list(struct.unpack("<%di" % SPELL_FIELDS, by_id[spec["clone"]]))
        vals[0] = sid
        fields = dict(spec.get("set", {}))
        for key, col in (("name", "Name_Lang_enUS"), ("rank", "NameSubtext_Lang_enUS"),
                         ("description", "Description_Lang_enUS"), ("aura", "AuraDescription_Lang_enUS")):
            fields[col] = spec.get(key, "")
        out = list(vals)
        for name, value in fields.items():
            i = index[name]
            typ = cols[i][1]
            if typ.startswith(("varchar", "text")):
                vals[i] = add_string(value)
            elif typ == "float":
                vals[i] = struct.unpack("<i", struct.pack("<f", float(value)))[0]
            else:
                vals[i] = int(value)
        for i, (name, typ) in enumerate(cols):
            if typ.startswith(("varchar", "text")):
                out[i] = text(vals[i])
            elif typ == "float":
                out[i] = struct.unpack("<f", struct.pack("<i", vals[i]))[0]
            elif "unsigned" in typ:
                out[i] = vals[i] & 0xFFFFFFFF
            else:
                out[i] = vals[i]
        by_id[sid] = struct.pack("<%di" % SPELL_FIELDS, *vals)
        sql_rows[sid] = out
    rows_out = [by_id[k] for k in sorted(by_id)]
    return write_dbc(nfield, recsize, rows_out, bytes(strings)), sql_rows


def sql_value(v):
    if isinstance(v, str):
        return "'" + v.replace("\\", "\\\\").replace("'", "''") + "'"
    if isinstance(v, float):
        return repr(v)
    return str(v)


ITEM_SQL_COLUMNS = ["entry", "class", "subclass", "SoundOverrideSubclass", "name", "displayid", "Quality", "Flags",
                    "BuyPrice", "SellPrice", "InventoryType", "ItemLevel", "RequiredLevel", "maxcount", "stackable",
                    "spellid_1", "spelltrigger_1", "spellcharges_1", "spellcooldown_1", "spellcategorycooldown_1",
                    "bonding", "description", "Material", "sheath"]


def currency_spells(currency):
    """Spell specs for build_spell_dbc, one per orb."""
    specs = []
    for o in currency["orbs"]:
        fields = dict(currency["spell_set"])
        if o.get("spell_icon"):
            fields["SpellIconID"] = o["spell_icon"]
        specs.append({"id": o["spell"], "clone": currency["clone"], "name": o["name"], "description": o["use"], "set": fields})
    return specs


def currency_sql(currency, spell_rows):
    cols = spell_columns()
    lines = ["-- Generated by tools/client-patch/build-patch.py --currency forge-currency.json --sql; edit the JSON, not this.",
             "-- Forge crafting orbs (wowproject docs/forge-currency.md): the orb items, their Use: spells (copies of a stock",
             "-- enchanting-scroll spell; the client has the same rows in patch-enUS-4.MPQ) and the mod-forge scripts."]
    orbs = currency["orbs"]
    spell_ids = ",".join(str(o["spell"]) for o in orbs)
    item_ids = ",".join(str(o["entry"]) for o in orbs)
    lines.append("DELETE FROM `spell_dbc` WHERE `ID` IN (%s);" % spell_ids)
    lines.append("INSERT INTO `spell_dbc` (%s) VALUES" % ", ".join("`%s`" % c for c, _ in cols))
    lines.append(",\n".join("(" + ", ".join(sql_value(v) for v in spell_rows[o["spell"]]) + ")" for o in orbs) + ";")
    lines.append("DELETE FROM `spell_script_names` WHERE `spell_id` IN (%s);" % spell_ids)
    lines.append("INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES")
    lines.append(",\n".join("(%d, '%s')" % (o["spell"], currency["script"]) for o in orbs) + ";")
    lines.append("""CREATE TABLE IF NOT EXISTS `forge_currency` (
  `spell_id` INT UNSIGNED NOT NULL,
  `item_entry` INT UNSIGNED NOT NULL,
  `kind` VARCHAR(20) NOT NULL COMMENT 'transmute alteration augment scour alchemy chance chaos regal annul vaal divine exalt essence raise',
  `arg` INT NOT NULL DEFAULT 0 COMMENT 'essence: item_affix id',
  `drop_weight` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'share of orb drops (0: never; Sands of Nozdormu drops from Unique bosses)',
  `name` VARCHAR(64) NOT NULL DEFAULT '',
  PRIMARY KEY (`spell_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;""")
    lines.append("DELETE FROM `forge_currency`;")
    lines.append("INSERT INTO `forge_currency` (`spell_id`, `item_entry`, `kind`, `arg`, `drop_weight`, `name`) VALUES")
    lines.append(",\n".join("(%d, %d, '%s', %d, %d, %s)" % (o["spell"], o["entry"], o["kind"], o["arg"], o["drop_weight"],
                                                          sql_value(o["name"])) for o in orbs) + ";")
    lines.append("DELETE FROM `item_template` WHERE `entry` IN (%s);" % item_ids)
    lines.append("INSERT INTO `item_template` (%s) VALUES" % ", ".join("`%s`" % c for c in ITEM_SQL_COLUMNS))
    rows = []
    for it in orbs:
        it = dict(it, spellid_1=it["spell"])
        v = {"class": 0, "subclass": 6, "SoundOverrideSubclass": -1, "Flags": 0, "InventoryType": 0, "ItemLevel": 1,
             "RequiredLevel": 0, "maxcount": 0, "stackable": 20, "spelltrigger_1": 0, "spellcharges_1": -1,
             "spellcooldown_1": -1, "spellcategorycooldown_1": -1, "bonding": 0, "description": "", "Material": 8,
             "sheath": 0, "BuyPrice": 0}
        v.update({k: it[k] for k in it if k in ITEM_SQL_COLUMNS})
        rows.append("(" + ", ".join(sql_value(v[c]) for c in ITEM_SQL_COLUMNS) + ")")
    lines.append(",\n".join(rows) + ";")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dbc-dir", required=True)
    ap.add_argument("--pools")
    ap.add_argument("--items")
    ap.add_argument("--currency")
    ap.add_argument("--sql")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    rows = read_rows(a.items, a.pools)
    currency = json.load(open(a.currency)) if a.currency else None
    if currency:
        rows += [[o["entry"], 0, 6, -1, 8, o["displayid"], 0, 0] for o in currency["orbs"]]
    item_dbc, added = build_item_dbc(os.path.join(a.dbc_dir, "Item.dbc"), rows)
    files = {"DBFilesClient\\Item.dbc": item_dbc}
    spells = 0
    if currency and currency["orbs"]:
        files["DBFilesClient\\Spell.dbc"], spell_rows = build_spell_dbc(os.path.join(a.dbc_dir, "Spell.dbc"),
                                                                         currency_spells(currency))
        spells = len(spell_rows)
        if a.sql:
            open(a.sql, "w").write(currency_sql(currency, spell_rows))
    mpq.write_archive(a.out, files)
    print("%s: Item.dbc +%d rows, Spell.dbc +%d rows, %d bytes" % (a.out, added, spells, os.path.getsize(a.out)))


if __name__ == "__main__":
    main()
