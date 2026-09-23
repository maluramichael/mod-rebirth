/*
 * mod-rebirth
 *
 * A "rebirth" / prestige-reset module for the mod-playerbots AzerothCore fork
 * (WotLK 3.3.5a). A real player who has reached the server level cap can talk to
 * the Rebirth NPC and be reborn: their level drops back to 1 (Death Knights to
 * 55), talents and skills reset, but ALL gear, gold, bags, bank, mounts, recipes
 * and quest progress are KEPT. In exchange each rebirth grants:
 *
 *   1. A permanent, stacking experience bonus (+X% per rebirth) applied to every
 *      XP gain, so re-levelling gets faster the more often you have been reborn.
 *   2. Rebirth Tokens - a currency spent at the same NPC's token shop, which is
 *      driven entirely by the `rebirth_shop` world-DB table so rewards (gold,
 *      items, learned spells/mounts) can be added or tuned WITHOUT recompiling.
 *
 * Bots never use the NPC and never get rebirth rows, so the XP bonus only ever
 * applies to real players. Schema and the NPC row are created programmatically
 * at startup (a failing module SQL file aborts the whole worldserver boot on
 * this fork, so we tolerate DB failure at runtime instead).
 *
 * Released under GNU GPL v2; redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "World.h"

// Playerbots fork headers, so we can tell a real player from a bot.
#include "Playerbots.h"
#include "PlayerbotAI.h"

#include <string>
#include <unordered_map>

namespace Rebirth
{
    struct Config
    {
        bool   Enable            = true;
        uint32 NpcEntry          = 800100;
        uint32 RequiredLevel     = 0;   // 0 = use the server's max player level
        uint32 TokensPerRebirth  = 5;
        uint32 XpBonusPctPerReb  = 10;  // permanent XP bonus added per rebirth
        uint32 MaxXpBonusPct     = 0;   // 0 = uncapped total XP bonus
        uint32 MaxRebirths       = 0;   // 0 = unlimited
        bool   AnnounceWorld     = true;
    };

    Config& GetConfig()
    {
        static Config cfg;
        return cfg;
    }

    struct Data
    {
        uint32 count  = 0;
        uint32 tokens = 0;
    };

    // guidLow -> data. Populated on login and mutated on the world thread only
    // (all script hooks used here run on the single world thread), so no lock.
    std::unordered_map<uint32, Data>& Cache()
    {
        static std::unordered_map<uint32, Data> cache;
        return cache;
    }

    // The effective level a rebirth requires (config override, else server cap).
    uint32 RequiredLevel()
    {
        uint32 req = GetConfig().RequiredLevel;
        if (req == 0)
            req = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
        return req;
    }

    bool IsRealPlayer(Player* player)
    {
        return player && GET_PLAYERBOT_AI(player) == nullptr;
    }

    Data GetData(uint32 guidLow)
    {
        auto it = Cache().find(guidLow);
        if (it != Cache().end())
            return it->second;
        return Data{};
    }

    void Store(uint32 guidLow, Data const& d)
    {
        Cache()[guidLow] = d;
        CharacterDatabase.Execute(
            "INSERT INTO `character_rebirth` (`guid`, `rebirth_count`, `tokens`) VALUES ({}, {}, {}) "
            "ON DUPLICATE KEY UPDATE `rebirth_count` = {}, `tokens` = {}",
            guidLow, d.count, d.tokens, d.count, d.tokens);
    }

    void LoadFromDB(uint32 guidLow)
    {
        Data d;
        if (QueryResult res = CharacterDatabase.Query(
                "SELECT `rebirth_count`, `tokens` FROM `character_rebirth` WHERE `guid` = {}", guidLow))
        {
            Field* f = res->Fetch();
            d.count  = f[0].Get<uint32>();
            d.tokens = f[1].Get<uint32>();
        }
        Cache()[guidLow] = d;
    }

    // Total permanent XP bonus percent for a given rebirth count, respecting the
    // optional cap.
    uint32 XpBonusPct(uint32 rebirthCount)
    {
        uint64 bonus = static_cast<uint64>(rebirthCount) * GetConfig().XpBonusPctPerReb;
        uint32 cap = GetConfig().MaxXpBonusPct;
        if (cap != 0 && bonus > cap)
            bonus = cap;
        return static_cast<uint32>(bonus);
    }

    void EnsureSchema()
    {
        // Per-character rebirth state (characters DB).
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `character_rebirth` ("
            "`guid` INT UNSIGNED NOT NULL, "
            "`rebirth_count` INT UNSIGNED NOT NULL DEFAULT 0, "
            "`tokens` INT UNSIGNED NOT NULL DEFAULT 0, "
            "PRIMARY KEY (`guid`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");

        // Data-driven token shop (world DB). Michael can add/tune rows freely.
        WorldDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `rebirth_shop` ("
            "`id` INT UNSIGNED NOT NULL, "
            "`enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1, "
            "`name` VARCHAR(100) NOT NULL, "
            "`cost` INT UNSIGNED NOT NULL, "
            "`reward_type` VARCHAR(16) NOT NULL, "     // 'gold' | 'item' | 'spell'
            "`reward_value` INT UNSIGNED NOT NULL, "   // copper | item entry | spell id
            "`reward_count` INT UNSIGNED NOT NULL DEFAULT 1, "
            "`sort` INT NOT NULL DEFAULT 0, "
            "PRIMARY KEY (`id`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");

        // Seed the default shop. Uses INSERT IGNORE keyed on id, so re-runs and
        // Michael's own edits/added rows are preserved (to hide a default reward,
        // set its `enabled` = 0 rather than deleting it, or it is re-seeded on the
        // next boot). Item ids below are verified against this fork's world DB.
        WorldDatabase.DirectExecute(
            "INSERT IGNORE INTO `rebirth_shop` (`id`,`enabled`,`name`,`cost`,`reward_type`,`reward_value`,`reward_count`,`sort`) VALUES "
            // Gold tiers (always valid).
            "(1,1,'500 Gold',1,'gold',5000000,1,10), "
            "(2,1,'5000 Gold',8,'gold',50000000,1,20), "
            "(3,1,'25000 Gold',30,'gold',250000000,1,30), "
            // Heirloom weapons - scale 1-80, OP damage from level 1.
            "(10,1,'Heirloom: Bloodied Arcanite Reaper (2H-Schwert)',10,'item',42943,1,100), "
            "(11,1,'Heirloom: Balanced Heartseeker (Dolch)',10,'item',42944,1,110), "
            "(12,1,'Heirloom: Dal''Rend''s Sacred Charge (1H-Schwert)',10,'item',42945,1,120), "
            "(13,1,'Heirloom: Charmed Ancient Bone Bow (Bogen)',10,'item',42946,1,130), "
            // Heirloom chest - +10% XP, one per armor type (stacks with rebirth XP bonus).
            "(20,1,'Heirloom-Brust: Tattered Dreadmist Robe (Stoff, +10% XP)',8,'item',48691,1,200), "
            "(21,1,'Heirloom-Brust: Stained Shadowcraft Tunic (Leder, +10% XP)',8,'item',48689,1,210), "
            "(22,1,'Heirloom-Brust: Mystical Vest of Elements (Kette, +10% XP)',8,'item',48683,1,220), "
            "(23,1,'Heirloom-Brust: Polished Breastplate of Valor (Platte, +10% XP)',8,'item',48685,1,230), "
            // OP mounts.
            "(30,1,'Mount: Swift Zhevra',10,'item',37719,1,300), "
            "(31,1,'Mount: Big Love Rocket',15,'item',50250,1,310), "
            "(32,1,'Mount: Turbo-Charged Flying Machine (episch, fliegend)',25,'item',34061,1,320), "
            "(33,1,'Mount: Grand Black War Mammoth (3 Sitze)',25,'item',44083,1,330);");

        // The Rebirth NPC itself (world DB). ON DUPLICATE keeps the script name,
        // display and flags correct even if the row already exists.
        uint32 entry = GetConfig().NpcEntry;
        WorldDatabase.DirectExecute(
            "INSERT INTO `creature_template` "
            "(`entry`,`name`,`subname`,`gossip_menu_id`,`minlevel`,`maxlevel`,`faction`,`npcflag`,`unit_class`,`type`,`RegenHealth`,`ScriptName`) "
            "VALUES ({},'Aevum','Hueter der Wiedergeburt',0,80,80,35,1,1,7,1,'npc_rebirth_master') "
            "ON DUPLICATE KEY UPDATE `ScriptName`=VALUES(`ScriptName`), `name`=VALUES(`name`), "
            "`subname`=VALUES(`subname`), `npcflag`=VALUES(`npcflag`), `faction`=VALUES(`faction`)",
            entry);

        WorldDatabase.DirectExecute(
            "INSERT INTO `creature_template_model` (`CreatureID`,`Idx`,`CreatureDisplayID`,`DisplayScale`,`Probability`,`VerifiedBuild`) "
            "VALUES ({},0,5233,1,1,12340) "
            "ON DUPLICATE KEY UPDATE `CreatureDisplayID`=VALUES(`CreatureDisplayID`)",
            entry);
    }

    // Perform the actual character reset. Keeps gear/gold/bags/quests; resets
    // level, XP, talents and (via GiveLevel) the level-capped skills.
    void DoReset(Player* player)
    {
        uint8 startLevel = (player->getClass() == CLASS_DEATH_KNIGHT) ? 55 : 1;

        // GiveLevel handles level-DOWN too (negative stat/health deltas, skill
        // caps, talent re-init, full heal). We follow it with an explicit XP and
        // talent wipe.
        player->GiveLevel(startLevel);
        player->SetUInt32Value(PLAYER_XP, 0);
        player->resetTalents(true);
        player->InitTalentForLevel();
        player->UpdateSkillsForLevel();
        player->SaveToDB(false, false);
    }
}

using Rebirth::GetConfig;

// =====================================================================
//  Gossip actions
// =====================================================================
namespace
{
    enum RebirthAction : uint32
    {
        ACTION_REBIRTH_CONFIRM = 1,
        ACTION_REBIRTH_DO      = 2,
        ACTION_SHOP            = 3,
        ACTION_MAIN            = 4,
        ACTION_SHOP_BASE       = 1000, // shop item action = base + shop id
    };
}

// =====================================================================
//  CreatureScript: the Rebirth NPC (ScriptName 'npc_rebirth_master')
// =====================================================================
class npc_rebirth_master : public CreatureScript
{
public:
    npc_rebirth_master() : CreatureScript("npc_rebirth_master") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        Rebirth::Config const& cfg = GetConfig();
        if (!cfg.Enable)
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Die Wiedergeburt ist derzeit deaktiviert.");
            CloseGossipMenuFor(player);
            return true;
        }

        uint32 guidLow = player->GetGUID().GetCounter();
        Rebirth::Data d = Rebirth::GetData(guidLow);

        // Status line as a (non-actionable) header item.
        std::string status = Acore::StringFormat(
            "Wiedergeburten: {}  |  XP-Bonus: +{}%  |  Token: {}",
            d.count, Rebirth::XpBonusPct(d.count), d.tokens);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, status, GOSSIP_SENDER_MAIN, ACTION_MAIN);

        uint32 req = Rebirth::RequiredLevel();
        bool capReached = cfg.MaxRebirths != 0 && d.count >= cfg.MaxRebirths;

        if (!Rebirth::IsRealPlayer(player))
        {
            // Bots can't reach here, but be defensive.
        }
        else if (capReached)
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "Du hast das Maximum an Wiedergeburten erreicht.", GOSSIP_SENDER_MAIN, ACTION_MAIN);
        }
        else if (player->GetLevel() >= req)
        {
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE,
                Acore::StringFormat("Wiedergeburt durchfuehren (Level {} -> 1)", req),
                GOSSIP_SENDER_MAIN, ACTION_REBIRTH_CONFIRM);
        }
        else
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                Acore::StringFormat("Erreiche Level {}, um wiedergeboren zu werden.", req),
                GOSSIP_SENDER_MAIN, ACTION_MAIN);
        }

        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "Token-Shop oeffnen", GOSSIP_SENDER_MAIN, ACTION_SHOP);

        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        Rebirth::Config const& cfg = GetConfig();
        ClearGossipMenuFor(player);

        if (!cfg.Enable)
        {
            CloseGossipMenuFor(player);
            return true;
        }

        // Token shop items live in a numeric range above ACTION_SHOP_BASE.
        if (action >= ACTION_SHOP_BASE)
        {
            HandleShopBuy(player, action - ACTION_SHOP_BASE);
            return OnGossipHello(player, creature); // reopen main menu
        }

        switch (action)
        {
            case ACTION_REBIRTH_CONFIRM:
                ShowConfirm(player, creature);
                break;
            case ACTION_REBIRTH_DO:
                HandleRebirth(player, creature);
                break;
            case ACTION_SHOP:
                ShowShop(player, creature);
                break;
            case ACTION_MAIN:
            default:
                return OnGossipHello(player, creature);
        }
        return true;
    }

private:
    void ShowConfirm(Player* player, Creature* creature)
    {
        Rebirth::Config const& cfg = GetConfig();
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "Level & Talente werden auf 1 zurueckgesetzt. Ausruestung, Gold, Taschen,", GOSSIP_SENDER_MAIN, ACTION_MAIN);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "Bank, Reittiere und Quests BLEIBEN erhalten.", GOSSIP_SENDER_MAIN, ACTION_MAIN);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE,
            Acore::StringFormat("JA - wiedergeboren werden (+{}% XP, {} Token)",
                cfg.XpBonusPctPerReb, cfg.TokensPerRebirth),
            GOSSIP_SENDER_MAIN, ACTION_REBIRTH_DO);
        AddGossipItemFor(player, GOSSIP_ICON_TALK, "Abbrechen", GOSSIP_SENDER_MAIN, ACTION_MAIN);
        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
    }

    void HandleRebirth(Player* player, Creature* creature)
    {
        Rebirth::Config const& cfg = GetConfig();

        if (!Rebirth::IsRealPlayer(player))
        {
            CloseGossipMenuFor(player);
            return;
        }

        uint32 guidLow = player->GetGUID().GetCounter();
        Rebirth::Data d = Rebirth::GetData(guidLow);

        // Re-validate against tampering / stale menus.
        if (cfg.MaxRebirths != 0 && d.count >= cfg.MaxRebirths)
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Du hast das Maximum an Wiedergeburten erreicht.");
            CloseGossipMenuFor(player);
            return;
        }
        if (player->GetLevel() < Rebirth::RequiredLevel())
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Du hast das erforderliche Level noch nicht erreicht.");
            CloseGossipMenuFor(player);
            return;
        }
        if (player->IsInCombat())
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Nicht im Kampf moeglich.");
            CloseGossipMenuFor(player);
            return;
        }

        Rebirth::DoReset(player);

        d.count  += 1;
        d.tokens += cfg.TokensPerRebirth;
        Rebirth::Store(guidLow, d);

        ChatHandler(player->GetSession()).PSendSysMessage(
            "Wiedergeburt #{}! Du erhaeltst {} Token und dauerhaft +{}% XP (gesamt +{}%).",
            d.count, cfg.TokensPerRebirth, cfg.XpBonusPctPerReb, Rebirth::XpBonusPct(d.count));

        if (cfg.AnnounceWorld)
            ChatHandler(nullptr).SendWorldText(
                "|cff00ccff[Wiedergeburt]|r {} wurde zum {}. Mal wiedergeboren!",
                player->GetName(), d.count);

        CloseGossipMenuFor(player);
    }

    void ShowShop(Player* player, Creature* creature)
    {
        uint32 guidLow = player->GetGUID().GetCounter();
        Rebirth::Data d = Rebirth::GetData(guidLow);

        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG,
            Acore::StringFormat("Deine Token: {}", d.tokens), GOSSIP_SENDER_MAIN, ACTION_SHOP);

        if (QueryResult res = WorldDatabase.Query(
                "SELECT `id`,`name`,`cost` FROM `rebirth_shop` WHERE `enabled` = 1 ORDER BY `sort`, `id`"))
        {
            do
            {
                Field* f = res->Fetch();
                uint32 id   = f[0].Get<uint32>();
                std::string name = f[1].Get<std::string>();
                uint32 cost = f[2].Get<uint32>();
                AddGossipItemFor(player, GOSSIP_ICON_VENDOR,
                    Acore::StringFormat("{} ({} Token)", name, cost),
                    GOSSIP_SENDER_MAIN, ACTION_SHOP_BASE + id);
            } while (res->NextRow());
        }

        AddGossipItemFor(player, GOSSIP_ICON_TALK, "Zurueck", GOSSIP_SENDER_MAIN, ACTION_MAIN);
        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
    }

    void HandleShopBuy(Player* player, uint32 shopId)
    {
        QueryResult res = WorldDatabase.Query(
            "SELECT `name`,`cost`,`reward_type`,`reward_value`,`reward_count` "
            "FROM `rebirth_shop` WHERE `id` = {} AND `enabled` = 1", shopId);
        if (!res)
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Dieser Eintrag ist nicht verfuegbar.");
            return;
        }

        Field* f = res->Fetch();
        std::string name       = f[0].Get<std::string>();
        uint32 cost            = f[1].Get<uint32>();
        std::string rewardType = f[2].Get<std::string>();
        uint32 rewardValue     = f[3].Get<uint32>();
        uint32 rewardCount     = f[4].Get<uint32>();
        if (rewardCount == 0)
            rewardCount = 1;

        uint32 guidLow = player->GetGUID().GetCounter();
        Rebirth::Data d = Rebirth::GetData(guidLow);

        if (d.tokens < cost)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "Nicht genug Token: {} benoetigt, du hast {}.", cost, d.tokens);
            return;
        }

        // Deliver the reward first; only charge tokens if delivery succeeded.
        bool delivered = false;
        if (rewardType == "gold")
        {
            player->ModifyMoney(static_cast<int32>(rewardValue));
            delivered = true;
        }
        else if (rewardType == "item")
        {
            if (player->AddItem(rewardValue, rewardCount))
                delivered = true;
            else
                ChatHandler(player->GetSession()).PSendSysMessage("Inventar voll - Kauf abgebrochen.");
        }
        else if (rewardType == "spell")
        {
            player->learnSpell(rewardValue, false);
            delivered = true;
        }
        else
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Unbekannter Belohnungstyp: {}.", rewardType);
        }

        if (!delivered)
            return;

        d.tokens -= cost;
        Rebirth::Store(guidLow, d);
        ChatHandler(player->GetSession()).PSendSysMessage(
            "Gekauft: {}. Verbleibende Token: {}.", name, d.tokens);
    }
};

// =====================================================================
//  PlayerScript: cache load + the permanent XP bonus
// =====================================================================
class RebirthPlayerScript : public PlayerScript
{
public:
    RebirthPlayerScript() : PlayerScript("Rebirth_PlayerScript") { }

    void OnPlayerLogin(Player* player) override
    {
        if (!GetConfig().Enable || !player)
            return;
        Rebirth::LoadFromDB(player->GetGUID().GetCounter());
    }

    void OnPlayerLogout(Player* player) override
    {
        if (player)
            Rebirth::Cache().erase(player->GetGUID().GetCounter());
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        if (!GetConfig().Enable || !player || amount == 0)
            return;

        uint32 count = Rebirth::GetData(player->GetGUID().GetCounter()).count;
        if (count == 0)
            return;

        uint32 bonus = Rebirth::XpBonusPct(count);
        if (bonus == 0)
            return;

        amount = static_cast<uint32>((static_cast<uint64>(amount) * (100 + bonus)) / 100);
    }
};

// =====================================================================
//  WorldScript: config load + schema/NPC creation
// =====================================================================
class RebirthWorldScript : public WorldScript
{
public:
    RebirthWorldScript() : WorldScript("Rebirth_WorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        Rebirth::Config& cfg = GetConfig();
        cfg.Enable           = sConfigMgr->GetOption<bool>("Rebirth.Enable", true);
        cfg.NpcEntry         = sConfigMgr->GetOption<uint32>("Rebirth.NpcEntry", 800100);
        cfg.RequiredLevel    = sConfigMgr->GetOption<uint32>("Rebirth.RequiredLevel", 0);
        cfg.TokensPerRebirth = sConfigMgr->GetOption<uint32>("Rebirth.TokensPerRebirth", 5);
        cfg.XpBonusPctPerReb = sConfigMgr->GetOption<uint32>("Rebirth.XpBonusPctPerRebirth", 10);
        cfg.MaxXpBonusPct    = sConfigMgr->GetOption<uint32>("Rebirth.MaxXpBonusPct", 0);
        cfg.MaxRebirths      = sConfigMgr->GetOption<uint32>("Rebirth.MaxRebirths", 0);
        cfg.AnnounceWorld    = sConfigMgr->GetOption<bool>("Rebirth.AnnounceWorld", true);
    }

    void OnStartup() override
    {
        if (GetConfig().Enable)
            Rebirth::EnsureSchema();
    }
};

// =====================================================================
//  Registration
// =====================================================================
void AddRebirthScripts()
{
    new npc_rebirth_master();
    new RebirthPlayerScript();
    new RebirthWorldScript();
}
