// SPDX-License-Identifier: AGPL-3.0-or-later

#include "CharacterRoster.h"
#include <algorithm>
#include <map>
#include <common/fs.h>
#include <common/logging/logging.h>
#include <common/str.h>
#include <xuocore/uodata.h>
#include "Application.h"
#include "Config.h"
#include "Constants.h"
#include "CrossUO.h"
#include "Globals.h"
#include "ServerList.h"
#include "GameObjects/GameItem.h"
#include "GameObjects/GamePlayer.h"
#include "Managers/SkillsManager.h"
#include "ScreenStages/MainScreen.h"
#include "TextEngine/EntryText.h"

static std::vector<RosterCharacter> s_roster;
static bool s_loaded = false;
static std::string s_sessionKey; // empty when not in game
static uint32_t s_sessionTicks = 0; // last time play time was counted
static uint32_t s_nextCapture = 0;

static fs_path roster_file()
{
    return g_App.FilePath("characters.cfg");
}

static std::string trim(const std::string &s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
    {
        return {};
    }
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// The current login server, shard and account, plus a character name.
static std::string key_for(const std::string &name)
{
    const std::string account = g_MainScreen.m_Account != nullptr ? g_MainScreen.m_Account->GetTextA() : std::string();
    return g_Config.ServerAddress + ":" + std::to_string(g_Config.ServerPort) + "|" + g_ServerList.LastServerName + "|" +
           str_lower(account) + "|" + str_lower(name);
}

static void load()
{
    if (s_loaded)
    {
        return;
    }
    s_loaded = true;
    FILE *fp = fs_open(roster_file(), FS_READ);
    if (fp == nullptr)
    {
        return;
    }
    char buf[1024];
    RosterCharacter *cur = nullptr;
    while (fgets(buf, sizeof(buf), fp) != nullptr)
    {
        const std::string line = trim(buf);
        if (line == "[Character]")
        {
            s_roster.emplace_back();
            cur = &s_roster.back();
            continue;
        }
        const auto eq = line.find('=');
        if (cur == nullptr || eq == std::string::npos)
        {
            continue;
        }
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        if (k == "Key")
            cur->Key = v;
        else if (k == "Name")
            cur->Name = v;
        else if (k == "Profession")
            cur->Profession = v;
        else if (k == "Played")
            cur->PlayedSeconds = atoll(v.c_str());
        else if (k == "LastPlayed")
            cur->LastPlayed = time_t(atoll(v.c_str()));
        else if (k == "Skill")
        {
            const auto bar = v.rfind('|');
            if (bar != std::string::npos)
            {
                cur->TopSkills.push_back({ v.substr(0, bar), float(atof(v.c_str() + bar + 1)) });
            }
        }
        else if (k == "Layer")
        {
            RosterLayer l;
            int partial = 0;
            unsigned hue = 0;
            if (sscanf(v.c_str(), "%d,%u,%d", &l.Gump, &hue, &partial) == 3)
            {
                l.Hue = (unsigned short)hue;
                l.PartialHue = partial != 0;
                cur->Layers.push_back(l);
            }
        }
    }
    fs_close(fp);
}

static void save()
{
    FILE *fp = fs_open(roster_file(), FS_WRITE);
    if (fp == nullptr)
    {
        return;
    }
    for (const auto &c : s_roster)
    {
        fprintf(fp, "[Character]\nKey=%s\nName=%s\nProfession=%s\nPlayed=%lld\nLastPlayed=%lld\n", c.Key.c_str(),
                c.Name.c_str(), c.Profession.c_str(), c.PlayedSeconds, (long long)c.LastPlayed);
        for (const auto &s : c.TopSkills)
        {
            fprintf(fp, "Skill=%s|%.1f\n", s.Name.c_str(), s.Value);
        }
        for (const auto &l : c.Layers)
        {
            fprintf(fp, "Layer=%d,%u,%d\n", l.Gump, unsigned(l.Hue), l.PartialHue ? 1 : 0);
        }
        fprintf(fp, "\n");
    }
    fs_close(fp);
}

static RosterCharacter *find_key(const std::string &key)
{
    for (auto &c : s_roster)
    {
        if (c.Key == key)
        {
            return &c;
        }
    }
    return nullptr;
}

const RosterCharacter *Roster_Find(const std::string &characterName)
{
    load();
    return find_key(key_for(characterName));
}

void Roster_Forget(const std::string &characterName)
{
    load();
    const std::string key = key_for(characterName);
    const auto it = std::remove_if(s_roster.begin(), s_roster.end(), [&](const RosterCharacter &c) { return c.Key == key; });
    if (it != s_roster.end())
    {
        s_roster.erase(it, s_roster.end());
        save();
    }
}

std::string Roster_ProfessionTitle(const std::string &key)
{
    if (key.empty() || key == "advanced")
    {
        return "Adventurer";
    }
    std::string title = key;
    title[0] = char(toupper(title[0]));
    return title;
}

// The class whose signature skills hold the most points.
static std::string infer_profession()
{
    static const std::pair<const char *, const char *> groups[] = {
        { "magery", "mage" },           { "evaluating intelligence", "mage" }, { "meditation", "mage" },
        { "swordsmanship", "warrior" }, { "mace fighting", "warrior" },        { "fencing", "warrior" },
        { "tactics", "warrior" },       { "anatomy", "warrior" },              { "healing", "warrior" },
        { "blacksmithy", "blacksmith" }, { "mining", "blacksmith" },           { "tinkering", "blacksmith" },
        { "arms lore", "blacksmith" },  { "necromancy", "necromancer" },       { "spirit speak", "necromancer" },
        { "chivalry", "paladin" },      { "bushido", "samurai" },              { "ninjitsu", "ninja" },
        { "stealth", "ninja" },         { "hiding", "ninja" },
    };
    std::map<std::string, float> score;
    for (uint32_t i = 0; i < g_SkillsManager.Count; i++)
    {
        CSkill *skill = g_SkillsManager.Get(i);
        if (skill == nullptr || skill->Value <= 0.f)
        {
            continue;
        }
        const std::string name = str_lower(skill->Name);
        for (const auto &g : groups)
        {
            if (name == g.first)
            {
                score[g.second] += skill->Value;
            }
        }
    }
    std::string best = "advanced";
    float bestScore = 60.f; // below this nothing stands out yet
    for (const auto &s : score)
    {
        if (s.second > bestScore)
        {
            best = s.first;
            bestScore = s.second;
        }
    }
    return best;
}

// The same layers the paperdoll gump draws (CGumpPaperdoll::UpdateContent).
static std::vector<RosterLayer> capture_layers(CGameCharacter *obj)
{
    static const int usedLayers[] = { OL_CLOAK,    OL_RING,  OL_SHIRT,  OL_ARMS,  OL_PANTS,  OL_SHOES,    OL_LEGS,    OL_TORSO,
                                      OL_BRACELET, OL_15,    OL_GLOVES, OL_TUNIC, OL_SKIRT,  OL_NECKLACE, OL_HAIR,    OL_ROBE,
                                      OL_EARRINGS, OL_BEARD, OL_HELMET, OL_WAIST, OL_1_HAND, OL_2_HAND,   OL_TALISMAN };
    std::vector<RosterLayer> layers;
    const uint16_t color = obj->Color & 0x3FFF;
    int body = 0x000C;
    switch (obj->Graphic)
    {
        case 0x0191:
        case 0x0193:
            body = 0x000D;
            break;
        case 0x025D:
        case 0x025F:
            body = 0x000E;
            break;
        case 0x025E:
        case 0x0260:
            body = 0x000F;
            break;
        case 0x029A:
        case 0x02B6:
            body = 0x029A;
            break;
        case 0x029B:
        case 0x02B7:
            body = 0x0299;
            break;
        case 0x04E5:
            body = 0xC835;
            break;
        default:
            break;
    }
    if (obj->Graphic == 0x03DB)
    {
        layers.push_back({ 0x000C, 0x03EA, true });
        layers.push_back({ 0xC72B, color, false }); // GM robe
    }
    else
    {
        layers.push_back({ body, color, true });
    }

    const int gumpOffset = obj->Gender == GENDER_FEMALE ? FEMALE_GUMP_OFFSET : MALE_GUMP_OFFSET;
    const auto bodyConv = uo_get_equip_body_conv(obj->Graphic);
    for (const int layer : usedLayers)
    {
        CGameItem *item = obj->FindLayer(layer);
        if (item == nullptr)
        {
            continue;
        }
        uint16_t id = item->AnimID;
        const auto conv = uo_get_equipconv(bodyConv, id);
        if (conv != nullptr)
        {
            id = conv->Gump;
        }
        if (id == 0)
        {
            continue;
        }
        int offset = gumpOffset;
        if (obj->Gender == GENDER_FEMALE && g_Game.ExecuteGump(uint16_t(id + offset)) == nullptr)
        {
            offset = MALE_GUMP_OFFSET;
        }
        layers.push_back({ id + offset, uint16_t(item->Color & 0x3FFF), item->IsPartialHue() });
    }
    return layers;
}

void Roster_BeginSession()
{
    load();
    if (g_Player == nullptr)
    {
        return;
    }
    s_sessionKey = key_for(g_Player->GetName());
    s_sessionTicks = g_Ticks;
    s_nextCapture = g_Ticks + 5000; // skills arrive just after entering
    if (find_key(s_sessionKey) == nullptr)
    {
        RosterCharacter c;
        c.Key = s_sessionKey;
        c.Name = g_Player->GetName();
        s_roster.push_back(c);
    }
    find_key(s_sessionKey)->LastPlayed = time(nullptr);
    save();
}

void Roster_Capture()
{
    if (s_sessionKey.empty() || g_Player == nullptr)
    {
        return;
    }
    RosterCharacter *c = find_key(s_sessionKey);
    if (c == nullptr)
    {
        return;
    }
    // play time since the last capture
    c->PlayedSeconds += (g_Ticks - s_sessionTicks) / 1000;
    s_sessionTicks = g_Ticks - (g_Ticks - s_sessionTicks) % 1000;
    c->LastPlayed = time(nullptr);
    c->Name = g_Player->GetName();

    std::vector<RosterSkill> skills;
    for (uint32_t i = 0; i < g_SkillsManager.Count; i++)
    {
        CSkill *skill = g_SkillsManager.Get(i);
        if (skill != nullptr && skill->Value > 0.f && !skill->Name.empty())
        {
            skills.push_back({ skill->Name, skill->Value });
        }
    }
    if (!skills.empty())
    {
        std::sort(skills.begin(), skills.end(), [](const RosterSkill &a, const RosterSkill &b) { return a.Value > b.Value; });
        skills.resize(std::min<size_t>(3, skills.size()));
        c->TopSkills = skills;
        c->Profession = infer_profession();
    }
    auto layers = capture_layers(g_Player);
    if (layers.size() > 1)
    {
        c->Layers = std::move(layers);
    }
    save();
}

void Roster_Tick()
{
    if (!s_sessionKey.empty() && g_GameState >= GS_GAME && g_Ticks >= s_nextCapture)
    {
        Roster_Capture();
        s_nextCapture = g_Ticks + 30000;
    }
}

void Roster_EndSession()
{
    Roster_Capture();
    s_sessionKey.clear();
}
