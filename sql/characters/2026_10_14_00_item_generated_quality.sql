-- Quality ladder (wowproject docs/loot-vanilla-plan.md): a generated item's quality now follows its base (or a
-- Legendary promotion) instead of its affix count. NULL = row from before the ladder; the worldserver regenerates
-- those at startup (same affixes, budget re-split, base quality) and saves the quality.
ALTER TABLE `item_generated` ADD COLUMN `quality` TINYINT UNSIGNED NULL DEFAULT NULL AFTER `item_level`;

-- Uniques (docs/uniques-draft.md): the pool entry each Unique was given, kept so items stay valid across restarts.
CREATE TABLE IF NOT EXISTS `item_unique_state` (
  `unique_key` VARCHAR(30) NOT NULL,
  `entry` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`unique_key`),
  UNIQUE KEY `entry` (`entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
