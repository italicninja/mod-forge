-- Sands of Nozdormu (wowproject docs/forge-currency.md, 2026-10-22): a Unique below raised_item_level can be raised to
-- it and to raised_required_level (0: it can't be raised); raised_value is each line's value at the new item level
-- (the spell id for spell lines). Apply before 2026_10_14_01_item_unique_data.sql, which fills them.
ALTER TABLE `item_unique`
  ADD COLUMN `raised_item_level` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `chance`,
  ADD COLUMN `raised_required_level` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `raised_item_level`;
ALTER TABLE `item_unique_stat`
  ADD COLUMN `raised_value` INT NOT NULL DEFAULT 0 AFTER `value`;
