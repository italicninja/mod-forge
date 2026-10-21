-- Affix grades (Forge.AffixGrades, 2026-10-21): each affix rolls its own grade (1 best .. 4) and its value comes
-- from that grade, PoE-style, so an orb can reroll a value within its grade. 0 = rolled before grades, or a spell affix.
ALTER TABLE `item_generated_affix` ADD COLUMN `grade` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `value`;
