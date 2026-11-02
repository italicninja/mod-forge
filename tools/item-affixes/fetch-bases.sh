#!/usr/bin/env bash
# Eligible base items for generated loot, as bases.tsv for gen.py pools. Mirrors
# ForgeMgr::IsEligible / GetItemMask (QUALITIES, default grey..purple: the quality ladder; BoE, no set, not unique,
# equippable weapon/armor) with RequiredLevel <= the realm cap (60).
#
#   MYSQL="mysql -h127.0.0.1 -uacore -pacore" tools/item-affixes/fetch-bases.sh > bases.tsv
#
# MYSQL is the client command without a database name (default: mysql, with your ~/.my.cnf);
# WORLD_DB the world database (default: acore_world).
set -euo pipefail
QUALITIES=${QUALITIES:-0,1,2,3,4}
read -r -a MYSQL_CMD <<<"${MYSQL:-mysql}"
"${MYSQL_CMD[@]}" -B "${WORLD_DB:-acore_world}" <<SQL
SELECT entry, class, subclass, SoundOverrideSubclass, Material, displayid, InventoryType, sheath
FROM item_template
WHERE entry < 1000000 AND Quality IN ($QUALITIES) AND bonding NOT IN (1, 4, 5) AND itemset = 0 AND startquest = 0
  AND maxcount <= 0 AND (Flags & 0x80000) = 0 AND RequiredLevel <= 60
  AND (
    (class = 2 AND (subclass IN (2, 3, 16, 18, 19)
                    OR (subclass IN (0, 1, 4, 5, 6, 7, 8, 10, 13, 15) AND InventoryType IN (13, 17, 21, 22))))
    OR (class = 4 AND (InventoryType IN (2, 11, 12, 14, 16, 23)
                       OR (subclass IN (1, 2, 3, 4) AND InventoryType IN (1, 3, 5, 6, 7, 8, 9, 10, 20))))
  )
ORDER BY entry;
SQL
