-- PoE-style generated loot (wowproject docs/loot-affixes.md): affix definitions and the
-- Item.dbc entry pools shipped in the client patch (patch-enUS-4.MPQ).
CREATE TABLE IF NOT EXISTS `item_affix` (
  `id` INT UNSIGNED NOT NULL,
  `kind` TINYINT UNSIGNED NOT NULL COMMENT '0 prefix, 1 suffix',
  `grp` INT UNSIGNED NOT NULL COMMENT 'at most one affix per group on an item',
  `effect` TINYINT UNSIGNED NOT NULL COMMENT '0 stat, 1 armor, 2 armor %, 3 weapon damage %, 4 attack speed %, 5 resist, 6 block %',
  `effect_arg` INT NOT NULL DEFAULT 0 COMMENT 'stat: ItemModType; resist: spell school',
  `item_mask` INT UNSIGNED NOT NULL COMMENT 'ItemAffixMask bits',
  `weight` INT UNSIGNED NOT NULL DEFAULT 100,
  `comment` VARCHAR(64) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `item_affix_tier` (
  `affix_id` INT UNSIGNED NOT NULL,
  `tier` TINYINT UNSIGNED NOT NULL COMMENT '1 = lowest',
  `min_ilvl` TINYINT UNSIGNED NOT NULL,
  `req_level` TINYINT UNSIGNED NOT NULL,
  `min_value` INT NOT NULL,
  `max_value` INT NOT NULL,
  `weight` INT UNSIGNED NOT NULL DEFAULT 100,
  `name` VARCHAR(64) NOT NULL COMMENT 'prefix word or suffix phrase',
  PRIMARY KEY (`affix_id`, `tier`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `item_affix_pool` (
  `id` INT UNSIGNED NOT NULL,
  `first_entry` INT UNSIGNED NOT NULL,
  `count` INT UNSIGNED NOT NULL,
  `class` TINYINT UNSIGNED NOT NULL,
  `subclass` TINYINT UNSIGNED NOT NULL,
  `sound` INT NOT NULL,
  `material` INT NOT NULL,
  `display` INT UNSIGNED NOT NULL COMMENT '0: rows carry no particular model',
  `inventory_type` TINYINT UNSIGNED NOT NULL,
  `sheath` TINYINT UNSIGNED NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `first_entry` (`first_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
