-- PoE-style generated loot (wowproject docs/loot-affixes.md): one row per generated item
-- template still referenced by item_instance (the worldserver sweeps the rest at startup).
CREATE TABLE IF NOT EXISTS `item_generated` (
  `entry` INT UNSIGNED NOT NULL,
  `base_entry` INT UNSIGNED NOT NULL,
  `item_level` TINYINT UNSIGNED NOT NULL,
  `created` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `item_generated_affix` (
  `entry` INT UNSIGNED NOT NULL,
  `idx` TINYINT UNSIGNED NOT NULL,
  `affix_id` INT UNSIGNED NOT NULL,
  `tier` TINYINT UNSIGNED NOT NULL,
  `value` INT NOT NULL,
  PRIMARY KEY (`entry`, `idx`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `item_affix_pool_state` (
  `pool_id` INT UNSIGNED NOT NULL,
  `next_unused` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`pool_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
