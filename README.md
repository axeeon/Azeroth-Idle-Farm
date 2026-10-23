# mod-idle-farm

Simulated offline farming. Pick a mob, log out, and your character is credited with kills of it
while you were away. Nothing exists in the world while you are offline; the kills are calculated
when you next log in.

## Players

| command | effect |
| --- | --- |
| `.idle set` | farm the creature you have selected |
| `.idle zone` | farm the zone you are standing in: a mix of its creatures that suit your level |
| `.idle clear` | stop idle farming |
| `.idle` / `.idle status` | show your target and its estimated kills per hour |

## Idle Farming Master (gossip NPC)

Entry `9850001`, using Chromie's model. Talk to it for:

- **Farm a zone for my level...** lists up to 12 zones that have enough creatures suited to your
  level (levels shown are the creatures that qualify). Pick one and, while you are away, kills are
  spread across that zone's suitable creatures in proportion to how many of each spawn there.
- **Farm a specific creature near me...** lists the creatures within `IdleFarm.NpcSearchRadius`
  yards.
- **What am I farming right now?** and **Stop idle farming**.

The zone list comes from the world's creature spawns, so it needs no data tables. Zone farming
re-checks which creatures suit your level every time you log in, so it keeps following you as you
level; the level in effect is the one you had when you logged out.

Spawn it with a GM account, standing where you want it:

    .npc add 9850001

Move it with `.npc move` and remove it with `.npc delete` while it is selected. The NPC's rows are
created automatically on first start, like the characters table.

## What an absence earns

- Time counted is the absence, capped by `IdleFarm.MaxOfflineHours`, and ignored under
  `IdleFarm.MinOfflineMinutes`.
- In zone mode the average time per kill is weighted by how common each creature is in the zone.
- Kills = counted seconds x `IdleFarm.Efficiency` / seconds per kill, capped by `IdleFarm.MaxKills`.
  Seconds per kill grows 25% per level the mob is above you, and elites take three times as long.
- XP uses the core's own `BaseGain`, so gray mobs give nothing, and the realm's kill XP rate applies.
  The XP is given through `GiveXP`, so other modules that scale XP (mod-dynamic-xp) still apply.
- Money and items are rolled per kill from the creature's real loot tables, then mailed
  ("Idle Farming Report"). Grey items are sold for money when `IdleFarm.SellGrey` is on.

## Install

Copy this folder to `modules/mod-idle-farm`, re-run CMake, rebuild, and copy
`conf/idle_farm.conf.dist` next to `worldserver.conf`. The DB updater creates
`character_idle_farm` in the characters database on first start.

## Limits

A crash or kill of the worldserver skips the logout stamp, so that absence earns nothing.
Characters logged out inside a battleground or arena are not stamped.
