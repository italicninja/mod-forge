-- Talent affixes (wowproject docs/loot-affixes.md, 2026-10-22): effect 10, effect_arg = Talent.dbc id, value = +ranks.
-- Group 200 holds them all (one per item). Proof of concept: +1 Improved Fireball (talent 26), weight 0 so it never
-- drops; hand it out with .forge with <base entry> 46. Needs Forge.TalentAffixes.Enable = 1.
DELETE FROM `item_affix_tier` WHERE `affix_id` = 46;
DELETE FROM `item_affix` WHERE `id` = 46;
INSERT INTO `item_affix` (`id`, `kind`, `grp`, `effect`, `effect_arg`, `item_mask`, `weight`, `comment`) VALUES
(46, 1, 200, 10, 26, 2047, 0, 'talent: +1 Improved Fireball (mage)');
INSERT INTO `item_affix_tier` (`affix_id`, `tier`, `min_ilvl`, `req_level`, `min_value`, `max_value`, `weight`, `name`) VALUES
(46, 1, 1, 0, 1, 1, 100, 'of the Fireball');
