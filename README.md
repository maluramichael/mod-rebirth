# mod-rebirth

A **rebirth / prestige-reset** module for the mod-playerbots AzerothCore fork (WotLK 3.3.5a).

A real player who has reached the server level cap can talk to the **Rebirth NPC**
(*Aevum, Hüter der Wiedergeburt*) and be reborn:

- **Level → 1** (Death Knights → 55), **talents & skills reset**.
- **Kept:** gear, gold, bags, bank, mounts, recipes, quest progress.
- **Gained per rebirth:**
  - a **permanent, stacking XP bonus** (`+X%` per rebirth, applied to every XP gain), and
  - **Rebirth Tokens** — a currency spent at the same NPC's **token shop**.

Bots never use the NPC and never earn rebirth rows, so the XP bonus only ever
applies to real players.

## Setup

1. Build the server (the module compiles into the shared `modules` lib).
2. Deploy — the config is generated from `conf/mod_rebirth.conf.dist`.
3. On first boot the module creates its tables and the NPC row automatically.
4. Spawn the NPC once, standing where you want it:

   ```
   .npc add 800100
   ```

   (Use `Rebirth.NpcEntry` if you changed it.)

## Config (`mod_rebirth.conf`)

| Key | Default | Meaning |
|-----|---------|---------|
| `Rebirth.Enable` | `1` | Master switch. |
| `Rebirth.NpcEntry` | `800100` | creature_template entry for the NPC. |
| `Rebirth.RequiredLevel` | `0` | Level needed to rebirth (`0` = server max level). |
| `Rebirth.TokensPerRebirth` | `5` | Tokens granted per rebirth. |
| `Rebirth.XpBonusPctPerRebirth` | `10` | Permanent XP bonus added per rebirth. |
| `Rebirth.MaxXpBonusPct` | `0` | Cap on total XP bonus (`0` = uncapped). |
| `Rebirth.MaxRebirths` | `0` | Cap on rebirth count (`0` = unlimited). |
| `Rebirth.AnnounceWorld` | `1` | Server-wide message on each rebirth. |

## Token shop (data-driven, no recompile)

The shop is the world-DB table **`rebirth_shop`**. Add or tune rows freely; the
NPC lists every `enabled = 1` row ordered by `sort`, `id`.

| Column | Meaning |
|--------|---------|
| `id` | Unique id (also the gossip action). |
| `enabled` | `1` shows the entry. |
| `name` | Label shown in the shop. |
| `cost` | Tokens charged. |
| `reward_type` | `gold`, `item`, or `spell`. |
| `reward_value` | `gold` → copper amount · `item` → item entry · `spell` → spell id (mounts, tabards, buffs …). |
| `reward_count` | Stack size for `item` (else 1). |
| `sort` | Display order. |

**Default seeded rewards** (verified item ids, created on first boot):

| Kategorie | Belohnung | Kosten |
|-----------|-----------|--------|
| Gold | 500 / 5.000 / 25.000 Gold | 1 / 8 / 30 |
| Heirloom-Waffe (skaliert 1–80) | Bloodied Arcanite Reaper (2H), Balanced Heartseeker (Dolch), Dal'Rend's Sacred Charge (1H), Charmed Ancient Bone Bow (Bogen) | 10 |
| Heirloom-Brust (+10 % XP, je Rüstungsart) | Stoff / Leder / Kette / Platte | 8 |
| Mount | Swift Zhevra / Big Love Rocket / Turbo-Charged Flying Machine (fliegend) / Grand Black War Mammoth | 10 / 15 / 25 / 25 |

Seeding uses `INSERT IGNORE` on `id`: to **hide** a default reward set its `enabled = 0`
(deleting it just re-seeds on the next boot). Add your own rows freely, e.g.:

```sql
-- 5x Flask of the Frost Wyrm for 3 tokens (item entry 46376):
INSERT INTO rebirth_shop (id,enabled,name,cost,reward_type,reward_value,reward_count,sort)
VALUES (40,1,'5x Fläschchen des Frostwyrm',3,'item',46376,5,400);

-- A learned spell/mount for 20 tokens (spell id, not item):
INSERT INTO rebirth_shop (id,enabled,name,cost,reward_type,reward_value,reward_count,sort)
VALUES (41,1,'Gelernter Zauber',20,'spell',54753,1,410);
```

## Tables

- `character_rebirth` (characters DB): `guid`, `rebirth_count`, `tokens`.
- `rebirth_shop` (world DB): the shop, above.

Both tables and the NPC row are created programmatically at startup (a failing
module SQL file aborts the whole worldserver boot on this fork, so the module
does its DDL in code and tolerates runtime failure instead).

## License

GNU GPL v2 (or later). See `LICENSE`.
