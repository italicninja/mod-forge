-- Crafting orbs (wowproject docs/forge-currency.md, 2026-10-21): flags 1 = Corrupted (Felfire Orb); unique_key = a Unique
-- given its own entry by Tear of the Naaru or Felfire Orb (its affix rows then hold the Unique's stat values, tier =
-- stat index, affix_id 0).
ALTER TABLE `item_generated`
  ADD COLUMN `flags` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `quality`,
  ADD COLUMN `unique_key` VARCHAR(30) NULL DEFAULT NULL AFTER `flags`;
