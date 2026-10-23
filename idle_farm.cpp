/*
 * mod-idle-farm - simulated offline farming.
 *
 * `.idle set` stores the hostile creature the player has selected as their farming target. On
 * logout the time is stamped; on the next login the absence is turned into kills of that creature:
 * experience is given directly, money and loot are rolled from the creature's real loot tables
 * and delivered by mail.
 */

#include "CellImpl.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureScript.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Formulas.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "Util.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
constexpr char const* Prefix = "|cff66ccff[Idle Farm]|r ";
constexpr float ElitePenalty = 3.0f;
constexpr float LevelGapPenalty = 0.25f;

struct Settings
{
    bool enabled;
    uint32 minSeconds;
    uint32 maxSeconds;
    float secondsPerKill;
    float efficiency;
    uint32 maxKills;
    float xpRate;
    float moneyRate;
    float lootRate;
    bool sellGrey;
    uint32 maxMails;
    bool allowElite;
    uint32 levelsBelow;
    uint32 levelsAbove;
    float npcRadius;
};

Settings LoadSettings()
{
    Settings s;
    s.enabled = sConfigMgr->GetOption<bool>("IdleFarm.Enable", true, false);
    s.minSeconds = sConfigMgr->GetOption<uint32>("IdleFarm.MinOfflineMinutes", 5, false) * MINUTE;
    s.maxSeconds = sConfigMgr->GetOption<uint32>("IdleFarm.MaxOfflineHours", 8, false) * HOUR;
    s.secondsPerKill = std::max(1.0f, sConfigMgr->GetOption<float>("IdleFarm.SecondsPerKill", 30.0f, false));
    s.efficiency = std::clamp(sConfigMgr->GetOption<float>("IdleFarm.Efficiency", 0.5f, false), 0.1f, 1.0f);
    s.maxKills = sConfigMgr->GetOption<uint32>("IdleFarm.MaxKills", 600, false);
    s.xpRate = std::max(0.0f, sConfigMgr->GetOption<float>("IdleFarm.Rate.XP", 1.0f, false));
    s.moneyRate = std::max(0.0f, sConfigMgr->GetOption<float>("IdleFarm.Rate.Money", 1.0f, false));
    s.lootRate = std::clamp(sConfigMgr->GetOption<float>("IdleFarm.Rate.Loot", 1.0f, false), 0.0f, 1.0f);
    s.sellGrey = sConfigMgr->GetOption<bool>("IdleFarm.SellGrey", true, false);
    s.maxMails = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("IdleFarm.MaxMails", 5, false), 1, 20);
    s.allowElite = sConfigMgr->GetOption<bool>("IdleFarm.AllowElite", false, false);
    s.levelsBelow = sConfigMgr->GetOption<uint32>("IdleFarm.LevelsBelow", 4, false);
    s.levelsAbove = sConfigMgr->GetOption<uint32>("IdleFarm.LevelsAbove", 2, false);
    s.npcRadius = std::clamp(sConfigMgr->GetOption<float>("IdleFarm.NpcSearchRadius", 150.0f, false), 10.0f, 500.0f);
    return s;
}

uint8 MobLevel(CreatureTemplate const* info)
{
    return uint8((uint32(info->minlevel) + uint32(info->maxlevel)) / 2);
}

float SecondsPerKill(Settings const& s, CreatureTemplate const* info, uint8 playerLevel)
{
    uint8 const mobLevel = MobLevel(info);
    float const gap = mobLevel > playerLevel ? float(mobLevel - playerLevel) : 0.0f;
    float seconds = s.secondsPerKill * (1.0f + LevelGapPenalty * gap);
    if (info->rank != CREATURE_ELITE_NORMAL)
        seconds *= ElitePenalty;
    return seconds;
}

char const* TargetProblem(Settings const& s, Player* player, CreatureTemplate const* info)
{
    if (!info)
        return "that creature no longer exists";
    if (info->rank == CREATURE_ELITE_WORLDBOSS || (info->type_flags & CREATURE_TYPE_FLAG_BOSS_MOB))
        return "bosses cannot be farmed";
    if (!s.allowElite && info->rank != CREATURE_ELITE_NORMAL && info->rank != CREATURE_ELITE_RARE)
        return "elites cannot be farmed";

    uint32 const mobLevel = MobLevel(info);
    uint32 const playerLevel = player->GetLevel();
    if (mobLevel + s.levelsBelow < playerLevel)
        return "that creature is too low level for you";
    if (mobLevel > playerLevel + s.levelsAbove)
        return "that creature is too high level for you";
    return nullptr;
}

char const* CreatureProblem(Settings const& s, Player* player, Creature* creature)
{
    if (creature->IsPet() || creature->IsTotem() || creature->IsGuard() || creature->IsTrigger() ||
        creature->IsSummon())
        return "that is not a creature that can be farmed";
    if (!player->IsValidAttackTarget(creature))
        return "you cannot attack that creature";
    return TargetProblem(s, player, creature->GetCreatureTemplate());
}

std::string Duration(uint32 seconds)
{
    uint32 const hours = seconds / HOUR;
    uint32 const minutes = (seconds % HOUR) / MINUTE;
    if (hours)
        return std::to_string(hours) + "h " + std::to_string(minutes) + "m";
    return std::to_string(minutes) + "m";
}

std::string Money(uint32 copper)
{
    return std::to_string(copper / 10000) + "g " + std::to_string((copper / 100) % 100) + "s " +
        std::to_string(copper % 100) + "c";
}

void Tell(Player* player, std::string const& text)
{
    ChatHandler(player->GetSession()).SendSysMessage(std::string(Prefix) + text);
}

struct Stack
{
    uint32 entry;
    uint32 count;
    uint32 quality;
};

void SendReport(Player* player, Settings const& s, std::string const& body, uint32 money, std::vector<Stack>& stacks)
{
    std::stable_sort(stacks.begin(), stacks.end(),
        [](Stack const& a, Stack const& b) { return a.quality > b.quality; });
    if (stacks.size() > size_t(s.maxMails) * MAX_MAIL_ITEMS)
        stacks.resize(size_t(s.maxMails) * MAX_MAIL_ITEMS);

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    MailSender const sender(MAIL_NORMAL, player->GetGUID().GetCounter(), MAIL_STATIONERY_GM);

    size_t index = 0;
    bool first = true;
    while (first || index < stacks.size())
    {
        MailDraft draft("Idle Farming Report", body);
        for (uint32 slot = 0; slot < MAX_MAIL_ITEMS && index < stacks.size(); ++index)
        {
            Item* item = Item::CreateItem(stacks[index].entry, stacks[index].count, player);
            if (!item)
                continue;
            item->SaveToDB(trans);
            draft.AddItem(item);
            ++slot;
        }
        if (first && money)
            draft.AddMoney(money);
        draft.SendMailTo(trans, MailReceiver(player), sender);
        first = false;
    }

    CharacterDatabase.CommitTransaction(trans);
}

constexpr uint32 MinZoneSpawns = 10;
constexpr size_t MaxListed = 12;

struct ZoneMob
{
    uint32 entry;
    uint32 weight;
};

std::unordered_map<uint32, std::vector<ZoneMob>> const& ZonePools()
{
    static std::unordered_map<uint32, std::vector<ZoneMob>> pools;
    static std::once_flag loaded;
    std::call_once(loaded, []()
    {
        QueryResult result = WorldDatabase.Query(
            "SELECT `zoneId`, `map`, `id`, COUNT(*) FROM `creature` WHERE `zoneId` > 0 AND `id` > 0 "
            "GROUP BY `zoneId`, `map`, `id`");
        if (!result)
            return;

        do
        {
            Field* fields = result->Fetch();
            MapEntry const* mapEntry = sMapStore.LookupEntry(fields[1].Get<uint32>());
            if (!mapEntry || mapEntry->Instanceable())
                continue;
            pools[fields[0].Get<uint32>()].push_back({ fields[2].Get<uint32>(), fields[3].Get<uint32>() });
        } while (result->NextRow());
    });
    return pools;
}

bool ZoneMobUsable(Settings const& s, Player* player, CreatureTemplate const* info)
{
    if (!info || info->npcflag != 0)
        return false;
    if (info->type == CREATURE_TYPE_CRITTER || info->type == CREATURE_TYPE_TOTEM ||
        info->type == CREATURE_TYPE_NON_COMBAT_PET)
        return false;
    if (info->flags_extra & (CREATURE_FLAG_EXTRA_TRIGGER | CREATURE_FLAG_EXTRA_GUARD))
        return false;

    FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info->faction);
    FactionTemplateEntry const* playerFaction = player->GetFactionTemplateEntry();
    if (!faction || !playerFaction || Unit::GetFactionReactionTo(faction, playerFaction) > REP_NEUTRAL)
        return false;
    return TargetProblem(s, player, info) == nullptr;
}

struct PoolMob
{
    uint32 entry;
    uint32 weight;
    float seconds;
    uint8 level;
};

std::vector<PoolMob> UsableMobs(Settings const& s, Player* player, std::vector<ZoneMob> const& list)
{
    std::vector<PoolMob> mobs;
    for (ZoneMob const& mob : list)
    {
        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(mob.entry);
        if (ZoneMobUsable(s, player, info))
            mobs.push_back({ mob.entry, mob.weight, SecondsPerKill(s, info, player->GetLevel()), MobLevel(info) });
    }
    return mobs;
}

std::vector<PoolMob> UsableMobsInZone(Settings const& s, Player* player, uint32 zone)
{
    auto const& pools = ZonePools();
    auto const found = pools.find(zone);
    if (found == pools.end())
        return {};
    return UsableMobs(s, player, found->second);
}

std::string ZoneName(Player* player, uint32 zone)
{
    AreaTableEntry const* area = GetAreaEntryByAreaID(zone);
    if (!area)
        return std::string();

    char const* name = area->area_name[player->GetSession()->GetSessionDbcLocale()];
    if (!name || !*name)
        name = area->area_name[0];
    return name ? name : std::string();
}

struct ZoneChoice
{
    uint32 zone;
    std::string name;
    uint32 weight;
    uint8 minLevel;
    uint8 maxLevel;
};

std::vector<ZoneChoice> ZoneChoices(Settings const& s, Player* player)
{
    std::vector<ZoneChoice> result;
    for (auto const& [zone, list] : ZonePools())
    {
        std::vector<PoolMob> const mobs = UsableMobs(s, player, list);
        uint32 weight = 0;
        uint8 lowest = 255;
        uint8 highest = 0;
        for (PoolMob const& mob : mobs)
        {
            weight += mob.weight;
            lowest = std::min(lowest, mob.level);
            highest = std::max(highest, mob.level);
        }
        if (weight < MinZoneSpawns)
            continue;

        std::string name = ZoneName(player, zone);
        if (!name.empty())
            result.push_back({ zone, std::move(name), weight, lowest, highest });
    }

    std::sort(result.begin(), result.end(), [](ZoneChoice const& a, ZoneChoice const& b)
    {
        return a.weight != b.weight ? a.weight > b.weight : a.name < b.name;
    });
    if (result.size() > MaxListed)
        result.resize(MaxListed);
    return result;
}

void Settle(Player* player, uint32 entry, uint32 zone, uint32 awaySeconds, Settings const& s)
{
    uint32 const farmSeconds = std::min(awaySeconds, s.maxSeconds);
    uint8 const playerLevel = player->GetLevel();

    std::vector<PoolMob> pool;
    std::string label;
    bool const zoneMode = entry == 0;
    if (!zoneMode)
    {
        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(entry);
        if (!info || TargetProblem(s, player, info))
        {
            Tell(player, "Your farming target is no longer valid for you; pick a new one.");
            return;
        }
        pool.push_back({ entry, 1, SecondsPerKill(s, info, playerLevel), MobLevel(info) });
        label = info->Name;
    }
    else
    {
        pool = UsableMobsInZone(s, player, zone);
        label = ZoneName(player, zone);
        if (pool.empty())
        {
            Tell(player, "There is nothing suitable left to farm in " + label + " at your level; pick a new target.");
            return;
        }
    }

    uint64 totalWeight = 0;
    double weightedSeconds = 0.0;
    for (PoolMob const& mob : pool)
    {
        totalWeight += mob.weight;
        weightedSeconds += double(mob.weight) * mob.seconds;
    }
    double const averageSeconds = weightedSeconds / double(totalWeight);
    uint32 const total = std::min<uint32>(uint32(farmSeconds * s.efficiency / averageSeconds), s.maxKills);
    if (!total)
        return;

    std::map<uint32, uint32> kills;
    if (pool.size() == 1)
        kills[pool[0].entry] = total;
    else
    {
        std::vector<uint64> cumulative;
        uint64 running = 0;
        for (PoolMob const& mob : pool)
        {
            running += mob.weight;
            cumulative.push_back(running);
        }
        for (uint32 i = 0; i < total; ++i)
        {
            uint64 const roll = urand(1, uint32(totalWeight));
            size_t const index = std::lower_bound(cumulative.begin(), cumulative.end(), roll) - cumulative.begin();
            ++kills[pool[index].entry];
        }
    }

    ContentLevels const content = GetContentLevelsForMapAndZone(player->GetMapId(), player->GetZoneId());
    uint64 xp = 0;
    uint32 money = 0;
    std::map<uint32, uint32> drops;
    for (auto const& [mobEntry, count] : kills)
    {
        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(mobEntry);
        if (!info)
            continue;

        uint32 const xpPerKill = Acore::XP::BaseGain(playerLevel, MobLevel(info), content);
        xp += uint64(xpPerKill * double(count) * sWorld->getRate(RATE_XP_KILL) * s.xpRate);

        for (uint32 i = 0; i < count; ++i)
        {
            if (s.lootRate < 1.0f && frand(0.0f, 1.0f) >= s.lootRate)
                continue;

            Loot loot;
            loot.FillLoot(info->lootid, LootTemplates_Creature, player, true, true);
            loot.generateMoneyLoot(info->mingold, info->maxgold);
            money += uint32(loot.gold * s.moneyRate);
            for (LootItem const& drop : loot.items)
                drops[drop.itemid] += drop.count;
        }
    }

    std::vector<Stack> stacks;
    uint32 itemCount = 0;
    for (auto const& [itemId, itemTotal] : drops)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            continue;
        if (s.sellGrey && proto->Quality == ITEM_QUALITY_POOR)
        {
            money += proto->SellPrice * itemTotal;
            continue;
        }
        itemCount += itemTotal;
        uint32 remaining = itemTotal;
        uint32 const maxStack = std::max<uint32>(1, proto->GetMaxStackSize());
        while (remaining)
        {
            uint32 const count = std::min(remaining, maxStack);
            stacks.push_back({ itemId, count, proto->Quality });
            remaining -= count;
        }
    }

    uint8 const levelBefore = player->GetLevel();
    if (xp)
        player->GiveXP(uint32(std::min<uint64>(xp, 0x7FFFFFFF)), nullptr);

    std::string const farmed = zoneMode ? std::to_string(total) + " kills in " + label
                                        : std::to_string(total) + " x " + label;
    std::string const body = "While you were away for " + Duration(awaySeconds) + " you farmed " + farmed + ".";
    if (money || !stacks.empty())
        SendReport(player, s, body, money, stacks);

    std::string summary = "Away " + Duration(awaySeconds);
    if (awaySeconds > s.maxSeconds)
        summary += " (" + Duration(s.maxSeconds) + " counted)";
    summary += ": " + farmed + ", " + std::to_string(xp) + " XP, " + Money(money);
    if (itemCount)
        summary += ", " + std::to_string(itemCount) + " item(s)";
    summary += ".";
    if (money || !stacks.empty())
        summary += " Loot and money are in your mailbox.";
    if (player->GetLevel() > levelBefore)
        summary += " Level " + std::to_string(player->GetLevel()) + " reached!";
    Tell(player, summary);
}

struct CurrentTarget
{
    uint32 entry = 0;
    uint32 zone = 0;
};

CurrentTarget LoadTarget(Player* player)
{
    CurrentTarget target;
    QueryResult result = CharacterDatabase.Query(
        "SELECT `creature_entry`, `farm_zone` FROM `character_idle_farm` WHERE `guid` = {}",
        player->GetGUID().GetCounter());
    if (result)
    {
        target.entry = (*result)[0].Get<uint32>();
        target.zone = (*result)[1].Get<uint32>();
    }
    return target;
}

void SetTarget(Player* player, uint32 entry, uint32 zone)
{
    CharacterDatabase.Execute(
        "REPLACE INTO `character_idle_farm` (`guid`, `creature_entry`, `farm_zone`, `logout_time`) "
        "VALUES ({}, {}, {}, 0)", player->GetGUID().GetCounter(), entry, zone);
}

void ClearTarget(Player* player)
{
    CharacterDatabase.Execute("DELETE FROM `character_idle_farm` WHERE `guid` = {}", player->GetGUID().GetCounter());
}

std::string StatusText(Settings const& s, Player* player)
{
    CurrentTarget const target = LoadTarget(player);
    if (!target.entry && !target.zone)
        return "No farming target set.";

    if (target.zone)
    {
        std::string const name = ZoneName(player, target.zone);
        std::vector<PoolMob> const mobs = UsableMobsInZone(s, player, target.zone);
        if (mobs.empty())
            return "Zone " + name + " has nothing suitable to farm at your level right now.";

        uint64 totalWeight = 0;
        double weightedSeconds = 0.0;
        uint8 lowest = 255;
        uint8 highest = 0;
        for (PoolMob const& mob : mobs)
        {
            totalWeight += mob.weight;
            weightedSeconds += double(mob.weight) * mob.seconds;
            lowest = std::min(lowest, mob.level);
            highest = std::max(highest, mob.level);
        }
        uint32 const killsPerHour = uint32(HOUR * s.efficiency / (weightedSeconds / double(totalWeight)));
        return "Zone: " + name + " (levels " + std::to_string(lowest) + "-" + std::to_string(highest) +
            " for you), about " + std::to_string(killsPerHour) + " kills per hour away, up to " +
            std::to_string(s.maxSeconds / HOUR) + "h counted.";
    }

    CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(target.entry);
    if (!info)
        return "Your farming target no longer exists.";

    if (char const* problem = TargetProblem(s, player, info))
        return "Target " + std::string(info->Name) + " is not valid right now: " + problem + ".";

    float const perKill = SecondsPerKill(s, info, player->GetLevel());
    uint32 const killsPerHour = uint32(HOUR * s.efficiency / perKill);
    return "Target: " + std::string(info->Name) + " (level " + std::to_string(MobLevel(info)) + "), about " +
        std::to_string(killsPerHour) + " kills per hour away, up to " + std::to_string(s.maxSeconds / HOUR) +
        "h counted.";
}

class IdleFarmPlayerScript : public PlayerScript
{
public:
    IdleFarmPlayerScript() : PlayerScript("IdleFarmPlayerScript", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT }) { }

    void OnPlayerLogout(Player* player) override
    {
        if (!sConfigMgr->GetOption<bool>("IdleFarm.Enable", true, false))
            return;
        if (player->InBattleground() || player->InArena())
            return;

        CharacterDatabase.Execute("UPDATE `character_idle_farm` SET `logout_time` = {} WHERE `guid` = {}",
            uint64(GameTime::GetGameTime().count()), player->GetGUID().GetCounter());
    }

    void OnPlayerLogin(Player* player) override
    {
        Settings const s = LoadSettings();
        if (!s.enabled)
            return;

        ObjectGuid::LowType const guid = player->GetGUID().GetCounter();
        QueryResult result = CharacterDatabase.Query(
            "SELECT `creature_entry`, `farm_zone`, `logout_time` FROM `character_idle_farm` WHERE `guid` = {}",
            guid);
        if (!result)
            return;

        Field* fields = result->Fetch();
        uint32 const entry = fields[0].Get<uint32>();
        uint32 const zone = fields[1].Get<uint32>();
        uint64 const logoutTime = fields[2].Get<uint64>();
        if (!logoutTime || (!entry && !zone))
            return;

        CharacterDatabase.Execute("UPDATE `character_idle_farm` SET `logout_time` = 0 WHERE `guid` = {}", guid);

        uint64 const now = uint64(GameTime::GetGameTime().count());
        if (now <= logoutTime || now - logoutTime < s.minSeconds)
            return;

        Settle(player, entry, zone, uint32(std::min<uint64>(now - logoutTime, 0xFFFFFFFF)), s);
    }
};

class IdleFarmCommandScript : public CommandScript
{
public:
    IdleFarmCommandScript() : CommandScript("IdleFarmCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable idleTable =
        {
            { "set",    HandleSet,    SEC_PLAYER, Console::No },
            { "zone",   HandleZone,   SEC_PLAYER, Console::No },
            { "clear",  HandleClear,  SEC_PLAYER, Console::No },
            { "status", HandleStatus, SEC_PLAYER, Console::No },
            { "",       HandleStatus, SEC_PLAYER, Console::No },
        };

        static ChatCommandTable commandTable =
        {
            { "idle", idleTable },
        };

        return commandTable;
    }

    static bool HandleSet(ChatHandler* handler)
    {
        Settings const s = LoadSettings();
        Player* player = handler->GetSession()->GetPlayer();
        if (!s.enabled)
        {
            handler->SendSysMessage("Idle farming is disabled on this realm.");
            return true;
        }

        Creature* creature = player->GetSelectedUnit() ? player->GetSelectedUnit()->ToCreature() : nullptr;
        if (!creature)
        {
            handler->SendSysMessage("Select the creature you want to farm first.");
            return true;
        }

        if (char const* problem = CreatureProblem(s, player, creature))
        {
            handler->SendSysMessage(std::string("You cannot farm that: ") + problem + ".");
            return true;
        }

        CreatureTemplate const* info = creature->GetCreatureTemplate();
        SetTarget(player, info->Entry, 0);
        handler->SendSysMessage(std::string(Prefix) + "Farming target set to " + info->Name +
            ". Log out and you will keep farming it while you are away.");
        return true;
    }

    static bool HandleZone(ChatHandler* handler)
    {
        Settings const s = LoadSettings();
        Player* player = handler->GetSession()->GetPlayer();
        if (!s.enabled)
        {
            handler->SendSysMessage("Idle farming is disabled on this realm.");
            return true;
        }

        uint32 const zone = player->GetZoneId();
        std::string const name = ZoneName(player, zone);
        std::vector<PoolMob> const mobs = UsableMobsInZone(s, player, zone);
        uint32 weight = 0;
        for (PoolMob const& mob : mobs)
            weight += mob.weight;
        if (name.empty() || weight < MinZoneSpawns)
        {
            handler->SendSysMessage("This zone has nothing suitable for you to farm.");
            return true;
        }

        SetTarget(player, 0, zone);
        handler->SendSysMessage(std::string(Prefix) + "Farming target set to the zone " + name +
            ". Log out and you will keep farming creatures here that suit your level while you are away.");
        return true;
    }

    static bool HandleClear(ChatHandler* handler)
    {
        ClearTarget(handler->GetSession()->GetPlayer());
        handler->SendSysMessage(std::string(Prefix) + "Idle farming target cleared.");
        return true;
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        handler->SendSysMessage(std::string(Prefix) +
            StatusText(LoadSettings(), handler->GetSession()->GetPlayer()));
        return true;
    }
};

constexpr uint32 SenderPickCreature = 1000;
constexpr uint32 SenderPickZone = 2000;
constexpr uint32 ActionMain = 1;
constexpr uint32 ActionCreatures = 2;
constexpr uint32 ActionZones = 3;
constexpr uint32 ActionStatus = 4;
constexpr uint32 ActionClear = 5;

struct Candidate
{
    uint32 entry;
    uint8 level;
    std::string name;
};

std::vector<Candidate> NearbyCandidates(Settings const& s, Player* player)
{
    std::list<Unit*> units;
    Acore::AnyUnitInObjectRangeCheck check(player, s.npcRadius);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(player, units, check);
    Cell::VisitObjects(player, searcher, s.npcRadius);

    std::map<uint32, Candidate> unique;
    for (Unit* unit : units)
    {
        Creature* creature = unit->ToCreature();
        if (!creature || CreatureProblem(s, player, creature))
            continue;

        CreatureTemplate const* info = creature->GetCreatureTemplate();
        unique.try_emplace(info->Entry, Candidate{ info->Entry, MobLevel(info), info->Name });
    }

    std::vector<Candidate> result;
    for (auto const& [entry, candidate] : unique)
        result.push_back(candidate);

    std::sort(result.begin(), result.end(), [](Candidate const& a, Candidate const& b)
    {
        return a.level != b.level ? a.level > b.level : a.name < b.name;
    });
    if (result.size() > MaxListed)
        result.resize(MaxListed);
    return result;
}

class IdleFarmMasterScript : public CreatureScript
{
public:
    IdleFarmMasterScript() : CreatureScript("npc_idle_farm_master") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ShowMain(player, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        LOG_INFO("module", "[IdleFarm] gossip select: player={} sender={} action={}", player->GetName(), sender,
            action);
        Settings const s = LoadSettings();
        if (!s.enabled)
        {
            CloseGossipMenuFor(player);
            Tell(player, "Idle farming is disabled on this realm.");
            return true;
        }

        if (sender == SenderPickCreature)
        {
            CloseGossipMenuFor(player);
            std::vector<Candidate> const candidates = NearbyCandidates(s, player);
            auto const found = std::find_if(candidates.begin(), candidates.end(),
                [action](Candidate const& c) { return c.entry == action; });
            if (found == candidates.end())
            {
                Tell(player, "That creature is no longer nearby.");
                return true;
            }

            SetTarget(player, found->entry, 0);
            Tell(player, "Farming target set to " + found->name +
                ". Log out and you will keep farming it while you are away.");
            return true;
        }

        if (sender == SenderPickZone)
        {
            CloseGossipMenuFor(player);
            std::vector<ZoneChoice> const choices = ZoneChoices(s, player);
            auto const found = std::find_if(choices.begin(), choices.end(),
                [action](ZoneChoice const& z) { return z.zone == action; });
            if (found == choices.end())
            {
                Tell(player, "That zone is no longer a good fit for you.");
                return true;
            }

            SetTarget(player, 0, found->zone);
            Tell(player, "Farming target set to the zone " + found->name +
                ". Log out and you will keep farming creatures there that suit your level while you are away.");
            return true;
        }

        switch (action)
        {
            case ActionMain:
                ShowMain(player, creature);
                break;
            case ActionCreatures:
                ShowCreatures(s, player, creature);
                break;
            case ActionZones:
                ShowZones(s, player, creature);
                break;
            case ActionClear:
                CloseGossipMenuFor(player);
                ClearTarget(player);
                Tell(player, "Idle farming target cleared.");
                break;
            default:
                CloseGossipMenuFor(player);
                Tell(player, StatusText(s, player));
                break;
        }
        return true;
    }

private:
    static void ShowMain(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Farm a zone for my level...", GOSSIP_SENDER_MAIN,
            ActionZones);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Farm a specific creature near me...", GOSSIP_SENDER_MAIN,
            ActionCreatures);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "What am I farming right now?", GOSSIP_SENDER_MAIN,
            ActionStatus);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Stop idle farming", GOSSIP_SENDER_MAIN, ActionClear);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature);
    }

    static void ShowCreatures(Settings const& s, Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        std::vector<Candidate> const candidates = NearbyCandidates(s, player);
        if (candidates.empty())
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "No suitable creatures nearby. Move closer to some.",
                GOSSIP_SENDER_MAIN, ActionMain);
        for (Candidate const& candidate : candidates)
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE,
                candidate.name + " (level " + std::to_string(candidate.level) + ")",
                SenderPickCreature, candidate.entry);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ActionMain);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature);
    }

    static void ShowZones(Settings const& s, Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        std::vector<ZoneChoice> const choices = ZoneChoices(s, player);
        if (choices.empty())
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "No zones have suitable creatures for your level.",
                GOSSIP_SENDER_MAIN, ActionMain);
        for (ZoneChoice const& choice : choices)
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE,
                choice.name + " (levels " + std::to_string(choice.minLevel) + "-" +
                std::to_string(choice.maxLevel) + ")", SenderPickZone, choice.zone);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ActionMain);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature);
    }
};
}

void AddSC_idle_farm()
{
    new IdleFarmPlayerScript();
    new IdleFarmCommandScript();
    new IdleFarmMasterScript();
}
