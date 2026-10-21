# mod-forge

Path of Exile-style generated loot for AzerothCore 3.3.5a. An eligible creature or chest drop becomes a new item:
the base item stripped to its armor, damage and look, plus prefixes and suffixes whose values split the base's
vanilla stat budget ("Vicious Chain Leggings of the Bear"). Also Legendaries (orange, 4–6 affixes), hand-authored
Uniques dropped by one creature, and spell affixes (equip effects and weapon procs from existing Spell.dbc spells).

Each roll is a real `ItemTemplate` with its own entry, taken from pools reserved in the client's `Item.dbc`
(1000000 and up), so players need `patch-enUS-4.MPQ` (wowproject `tools/client-patch`). Entries nothing holds go
back to their pool (recycling). Other modules use the API in `src/ForgeMgr.h`: mod-procedurality rolls Wanted
rewards with it, and mod-playerbots keeps bots' generated gear through core's `ObjectMgr::IsGeneratedItem`.

Needs core patch `0001-generated-item-hooks` (`ObjectMgr::SetGeneratedItemTemplateLookup`,
`WorldScript::OnAfterItemTemplatesLoad`). SQL is applied by hand from `sql/` (see `sql/README.md`). Config:
`conf/mod_forge.conf.dist`. GM commands: `.forge` / `.affix` (`roll <base> [ilvl] [prefixes] [suffixes]`,
`legendary <base> [ilvl]`, `unique [key]`, `info <entry>`). Design docs (`docs/loot-affixes.md`,
`docs/loot-vanilla-plan.md`, `docs/uniques-draft.md`) and the data generators (`tools/item-affixes/`,
`tools/client-patch/`) live in the wowproject repo for now.

## Install

Clone into `azerothcore-wotlk/modules/mod-forge`, apply the core patch, re-run CMake and build. Copy
`conf/mod_forge.conf.dist` to `etc/modules/mod_forge.conf`.

## Status

Extracted on 2026-10-20 from wowproject core patches `0001-item-affixes` and `0006-item-affix-spells`, where the
code ran live from 2026-10-13. Not yet built as a module.
