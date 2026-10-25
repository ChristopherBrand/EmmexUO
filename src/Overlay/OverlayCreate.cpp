// SPDX-License-Identifier: AGPL-3.0-or-later

// Character creation as one screen over a looping forest video: professions as tabs on the
// left, the live paperdoll and everything about the character in the main panel, and the
// starting city as a final step. Replaces the classic profession, appearance and town
// screens; it fills the same globals they did and sends the same packet.

#include "Overlay.h"

#if defined(NEW_RENDERER_ENABLED) && defined(USE_GL3)

#include "OverlayKit.h"
#include "OverlayPanels.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>
#include <external/gfx/imgui/imgui.h>
#include <external/gfx/imgui/imgui_stdlib.h>
#include <xuocore/uodata.h>
#include "../BaseQueue.h"
#include "../CityList.h"
#include "../Config.h"
#include "../CrossUO.h"
#include "../Globals.h"
#include "../Profession.h"
#include "../Managers/ClilocManager.h"
#include "../Managers/ColorManager.h"
#include "../Managers/CreateCharacterManager.h"
#include "../Managers/FontsManager.h"
#include "../Managers/ProfessionManager.h"
#include "../Managers/SkillsManager.h"
#include "../Managers/UOFileReader.h"
#include "../Network/Packets.h"
#include "../ScreenStages/CreateCharacterScreen.h"
#include "../ScreenStages/SelectTownScreen.h"

static const int CHARACTER_NAME_MAX = 20; // what the classic screen accepted

static OverlayVideo s_video;
static OverlayImage s_still;
static std::map<std::string, OverlayImage> s_portraits;

static std::vector<CProfession *> s_professions;
static CProfession *s_profession = nullptr;
static std::string s_name;
static std::string s_error;
static bool s_started = false;

// Advanced profession: chosen skills (index into the skills list, -1 = none) and values
static int s_skill[4] = { -1, -1, -1, -1 };
static int s_skillValue[4] = {};
static int s_stat[3] = {}; // str, dex, int

static OverlayImage s_preview;
static std::string s_previewKey;
// A swatch the pointer is over: the preview shows it until the pointer moves on.
static uint16_t *s_hoverTarget = nullptr;
static uint16_t s_hoverValue = 0;
static uint16_t *s_hoverTargetNext = nullptr;
static uint16_t s_hoverValueNext = 0;

static bool s_cityOpen = false;
static int s_city = 0;
static OverlayImage s_map;
static int s_mapIndex = -1;

// ---------------------------------------------------------------------------------------
// Rules that depend on the client version (the same as the classic screens)

static int skill_slots()
{
    return g_Config.ProtocolClientVersion >= CV_70160 ? 4 : 3;
}

static int skill_total()
{
    return skill_slots() == 4 ? 120 : 100;
}

static void stat_rules(int &total, int &max)
{
    if (g_Config.ClientVersion >= CV_70160)
    {
        total = 90;
        max = 60;
    }
    else if (g_Config.ClientVersion >= CV_308Z)
    {
        total = 80;
        max = 60;
    }
    else
    {
        total = 65;
        max = 45;
    }
}

static bool is_advanced(const CProfession *p)
{
    return p != nullptr && p->DescriptionIndex == -1;
}

static bool is_blocked(const CProfession *p)
{
    // Paladin and Necromancer need the server's Age of Shadows feature
    return (p->NameClilocID == 1061177 || p->NameClilocID == 1061178) && !g_Pal_Necro_Creation;
}

static std::string profession_name(const CProfession *p)
{
    if (is_advanced(p))
    {
        return "Advanced";
    }
    std::string name = p->TrueName.empty() ? p->Name : p->TrueName;
    name.erase(std::remove(name.begin(), name.end(), '"'), name.end());
    for (size_t i = 0; i < name.size(); i++)
    {
        name[i] = char(i == 0 ? toupper(name[i]) : tolower(name[i]));
    }
    return name;
}

static std::string profession_description(const CProfession *p)
{
    if (is_advanced(p))
    {
        return "Shape your own path: choose your skills and how strong, nimble and clever you begin.";
    }
    std::string text = p->DescriptionClilocID != 0 ?
                           g_ClilocManager.Cliloc(g_Language)->GetA(p->DescriptionClilocID, false, "") :
                           std::string();
    if (text.empty())
    {
        text = wstr_to_utf8(p->Description);
    }
    // the classic descriptions carry the profession name as a heading line
    const auto nl = text.find('\n');
    if (nl != std::string::npos && nl < 40)
    {
        text = text.substr(nl + 1);
    }
    return text;
}

// Keeps a set of values within [lo, hi] with a fixed total: after one value changes, the
// difference is taken from (or given to) the others, the furthest from their limit first.
static void rebalance(int *values, int count, int changed, int lo, int hi, int total)
{
    int sum = 0;
    for (int i = 0; i < count; i++)
    {
        sum += values[i];
    }
    int diff = sum - total;
    while (diff != 0)
    {
        int best = -1;
        for (int i = 0; i < count; i++)
        {
            if (i == changed)
            {
                continue;
            }
            const bool can = diff > 0 ? values[i] > lo : values[i] < hi;
            if (can && (best < 0 || (diff > 0 ? values[i] > values[best] : values[i] < values[best])))
            {
                best = i;
            }
        }
        if (best < 0)
        {
            // nothing else can move: clamp the one that changed
            values[changed] -= diff;
            break;
        }
        values[best] += diff > 0 ? -1 : 1;
        diff += diff > 0 ? -1 : 1;
    }
}

static void reset_advanced()
{
    const int slots = skill_slots();
    for (int i = 0; i < 4; i++)
    {
        s_skill[i] = -1;
        s_skillValue[i] = i < slots ? skill_total() / slots : 0;
    }
    int total = 0, max = 0;
    stat_rules(total, max);
    s_stat[0] = total / 2;
    s_stat[1] = total / 4;
    s_stat[2] = total - s_stat[0] - s_stat[1];
}

// ---------------------------------------------------------------------------------------
// Images: portraits, and UO gumps (paperdoll, map) composed into textures

static const OverlayImage &portrait(const CProfession *p)
{
    std::string key = is_advanced(p) ? "advanced" : profession_name(p);
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    auto it = s_portraits.find(key);
    if (it == s_portraits.end())
    {
        OverlayImage image = Overlay_LoadImage(("create/classes/" + key + ".jpg").c_str());
        if (image.texture == nullptr)
        {
            image = Overlay_LoadImage("create/classes/advanced.jpg");
        }
        it = s_portraits.emplace(key, image).first;
    }
    return it->second;
}

static void update_preview()
{
    auto &m = g_CreateCharacterManager;
    // a hovered palette swatch stands in for its value
    auto shown = [](uint16_t &field) { return s_hoverTarget == &field ? s_hoverValue : field; };
    const uint16_t skin = shown(m.SkinTone);
    const uint16_t shirt = shown(m.ShirtColor);
    const uint16_t pants = shown(m.PantsColor);
    const uint16_t hair = shown(m.HairColor);
    const uint16_t beard = shown(m.BeardColor);
    char key[160];
    snprintf(key, sizeof(key), "%d %d %d %d %x %x %x %x %x", int(m.GetRace()), int(m.GetFemale()),
             m.HairStyle, m.BeardStyle, skin, shirt, pants, hair, beard);
    if (s_previewKey == key && s_preview.texture != nullptr)
    {
        return;
    }
    s_previewKey = key;
    Overlay_FreeImage(s_preview);
    std::vector<GumpLayer> layers = {
        { m.GetBodyGump(), skin },
        { m.GetBootsGump(), 0 },
    };
    if (m.GetRace() != RT_GARGOYLE)
    {
        layers.push_back({ m.GetPantsGump(), pants });
    }
    layers.push_back({ m.GetShirtGump(), shirt });
    layers.push_back({ m.GetHair(m.HairStyle).GumpID, hair });
    if (!m.GetFemale() && m.GetRace() != RT_ELF)
    {
        layers.push_back({ m.GetBeard(m.BeardStyle).GumpID, beard });
    }
    s_preview = Overlay_ComposeGumps(layers, true);
}

// ---------------------------------------------------------------------------------------
// Entering the screen

static void collect_professions(CBaseProfession *node)
{
    QFOR(item, node->m_Items, CBaseProfession *)
    {
        if (item->Type == PT_PROFESSION)
        {
            s_professions.push_back((CProfession *)item);
        }
        else
        {
            collect_professions(item);
        }
    }
}

static void start()
{
    s_started = true;
    s_professions.clear();
    auto *root = (CBaseProfession *)g_ProfessionManager.m_Items;
    if (root != nullptr)
    {
        collect_professions(root);
    }
    // Advanced last
    std::stable_partition(s_professions.begin(), s_professions.end(), [](CProfession *p) { return !is_advanced(p); });
    s_profession = nullptr;
    for (auto *p : s_professions)
    {
        if (!is_blocked(p))
        {
            s_profession = p;
            break;
        }
    }
    g_CreateCharacterManager.Clear();
    s_name.clear();
    s_error.clear();
    s_cityOpen = false;
    reset_advanced();
}

void Overlay_CreateHidden()
{
    s_started = false;
    s_cityOpen = false;
    Overlay_VideoClose(s_video);
    Overlay_FreeImage(s_preview);
    Overlay_FreeImage(s_map);
    s_previewKey.clear();
    s_mapIndex = -1;
}

// ---------------------------------------------------------------------------------------
// Pieces

static void section_label(const char *text)
{
    ImGui::PushFont(Overlay_LoginFont(LoginFont::Small));
    ImGui::TextDisabled("%s", text);
    ImGui::PopFont();
}

// Segmented control; returns the chosen index.
static int segmented(const char *id, const char *const *labels, int count, int current, float width, float u)
{
    ImGui::PushID(id);
    const float h = Overlay_px(Overlay_LoginFont(LoginFont::Body)) + 18.f * u;
    const float segW = width / count;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + h), IM_COL32(0, 0, 0, 90), h / 2);
    int chosen = current;
    for (int i = 0; i < count; i++)
    {
        ImGui::SetCursorScreenPos(ImVec2(pos.x + segW * i, pos.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(segW, h)))
        {
            chosen = i;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImVec2 a(pos.x + segW * i + 3.f * u, pos.y + 3.f * u);
        const ImVec2 b(pos.x + segW * (i + 1) - 3.f * u, pos.y + h - 3.f * u);
        if (i == current)
        {
            dl->AddRectFilled(a, b, COL_GOLD, (h - 6.f * u) / 2);
        }
        else if (hovered)
        {
            dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 18), (h - 6.f * u) / 2);
        }
        const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        dl->AddText(ImVec2(floorf(a.x + (b.x - a.x - ts.x) / 2), floorf(pos.y + (h - ts.y) / 2)),
                    i == current ? IM_COL32(26, 20, 12, 255) : COL_TEXT, labels[i]);
    }
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
    ImGui::Dummy(ImVec2(width, 0));
    ImGui::PopID();
    return chosen;
}

// "‹ value ›" stepper over a list; returns the new index.
static int stepper(const char *id, const char *value, int index, int count, float width, float u)
{
    ImGui::PushID(id);
    const float h = Overlay_px(Overlay_LoginFont(LoginFont::Body)) + 18.f * u;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + h), IM_COL32(0, 0, 0, 90), 8.f * u);
    dl->AddRect(pos, ImVec2(pos.x + width, pos.y + h), IM_COL32(255, 255, 255, 30), 8.f * u);
    int result = index;
    const float arrowW = h;
    if (ImGui::InvisibleButton("##prev", ImVec2(arrowW, h)))
    {
        result = (index + count - 1) % count;
    }
    const bool prevHovered = ImGui::IsItemHovered();
    ImGui::SameLine(0, 0);
    ImGui::Dummy(ImVec2(width - arrowW * 2, h));
    ImGui::SameLine(0, 0);
    if (ImGui::InvisibleButton("##next", ImVec2(arrowW, h)))
    {
        result = (index + 1) % count;
    }
    const bool nextHovered = ImGui::IsItemHovered();
    Overlay_Chevron(dl, ImVec2(pos.x + arrowW / 2, pos.y + h / 2), 4.f * u, Chevron::Left, prevHovered ? COL_GOLD : COL_MUTED, 1.6f * u);
    Overlay_Chevron(dl, ImVec2(pos.x + width - arrowW / 2, pos.y + h / 2), 4.f * u, Chevron::Right, nextHovered ? COL_GOLD : COL_MUTED, 1.6f * u);
    const ImVec2 ts = ImGui::CalcTextSize(value);
    dl->AddText(ImVec2(floorf(pos.x + (width - ts.x) / 2), floorf(pos.y + (h - ts.y) / 2)), COL_TEXT, value);
    ImGui::PopID();
    return result;
}

// Which step of a hue's 32-step ramp a gump's pixels read as (the game hues a pixel by its red
// channel), so a swatch looks like the coloured art rather than the middle of the ramp.
static int ramp_step(int gump)
{
    static std::map<int, int> cache;
    if (gump <= 0 || gump >= MAX_GUMP_DATA_INDEX_COUNT || g_Index.m_Gump[gump].Address == 0)
    {
        return 16;
    }
    auto it = cache.find(gump);
    if (it != cache.end())
    {
        return it->second;
    }
    int histogram[32] = {};
    int total = 0;
    for (const uint16_t c : g_UOFileReader.GetGumpPixels(g_Index.m_Gump[gump]))
    {
        if (c != 0)
        {
            histogram[(c >> 10) & 0x1F]++;
            total++;
        }
    }
    // the 70th percentile: outlines and shadows pull the median down, while the eye reads a
    // surface by its lit areas
    int step = 16;
    for (int i = 0, seen = 0; i < 32 && total > 0; i++)
    {
        seen += histogram[i];
        if (seen * 10 >= total * 7)
        {
            step = i;
            break;
        }
    }
    cache[gump] = step;
    return step;
}

static ImU32 hue_swatch(uint16_t hue, int step = 16)
{
    return (g_ColorManager.GetPolygoneColor(uint16_t(step), hue) & 0x00FFFFFF) | 0xFF000000;
}


// A colour well that opens a palette; palette entries are hue indices stored +1.
// A colour well that opens a palette; palette entries are hue indices stored +1. `step` is
// the ramp step the coloured art uses (see ramp_step); `sorted` orders light to dark.
static bool color_picker(const char *id, uint16_t &value, const uint16_t *table, int count, int columns,
                         uint16_t minValue, uint16_t maxValue, float u, int step = 16, bool sorted = false)
{
    ImGui::PushID(id);
    bool changed = false;
    const float well = 34.f * u;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##well", ImVec2(well * 2.2f, well)))
    {
        ImGui::OpenPopup("##palette");
    }
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + well * 2.2f, pos.y + well), hue_swatch(value, step), 8.f * u);
    dl->AddRect(pos, ImVec2(pos.x + well * 2.2f, pos.y + well), hovered ? COL_GOLD : IM_COL32(255, 255, 255, 60), 8.f * u, ImDrawCornerFlags_All, hovered ? 2.f : 1.f);

    ImGui::SetNextWindowPos(ImVec2(pos.x, pos.y + well + 6.f * u), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f * u, 10.f * u));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.f * u);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.08f, 0.11f, 0.97f));
    if (ImGui::BeginPopup("##palette"))
    {
        // the values a player can actually get (servers clamp human tones), once each
        std::vector<uint16_t> entries;
        for (int i = 0; i < count; i++)
        {
            const uint16_t stored = std::max(minValue, std::min(maxValue, uint16_t((table != nullptr ? table[i] : i) + 1)));
            if (std::find(entries.begin(), entries.end(), stored) == entries.end())
            {
                entries.push_back(stored);
            }
        }
        if (sorted)
        {
            auto luminance = [step](uint16_t hue) {
                const ImU32 c = hue_swatch(hue, step);
                return 0.299f * (c & 0xFF) + 0.587f * ((c >> 8) & 0xFF) + 0.114f * ((c >> 16) & 0xFF);
            };
            std::stable_sort(entries.begin(), entries.end(), [&](uint16_t a, uint16_t b) { return luminance(a) > luminance(b); });
        }
        const float cell = entries.size() > 200 ? 13.f * u : (sorted ? 28.f * u : 24.f * u);
        ImDrawList *pl = ImGui::GetWindowDrawList();
        for (size_t i = 0; i < entries.size(); i++)
        {
            const uint16_t stored = entries[i];
            if (i % columns != 0)
            {
                ImGui::SameLine(0, 2.f * u);
            }
            ImGui::PushID(int(i));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##c", ImVec2(cell, cell)))
            {
                value = stored;
                changed = true;
                ImGui::CloseCurrentPopup();
            }
            const bool h = ImGui::IsItemHovered();
            if (h)
            {
                s_hoverTargetNext = &value;
                s_hoverValueNext = stored;
            }
            pl->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell), hue_swatch(stored, step), 3.f * u);
            if (stored == value || h)
            {
                pl->AddRect(p, ImVec2(p.x + cell, p.y + cell), h ? COL_GOLD : IM_COL32(255, 255, 255, 220), 3.f * u, ImDrawCornerFlags_All, 2.f);
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    return changed;
}

// Clothing hues: the classic 47x20 grid of dyeable colours.
static std::vector<uint16_t> &clothing_palette()
{
    static std::vector<uint16_t> table;
    if (table.empty())
    {
        for (int y = 1; y < 48; y++)
        {
            uint16_t start = uint16_t((1 << (y % 4)) * 100 + 1 + y / 4);
            for (int x = 0; x < 20; x++)
            {
                table.push_back(start);
                start += 5;
            }
        }
    }
    return table;
}

// ---------------------------------------------------------------------------------------
// The profession tabs

static void draw_tabs(const ImVec2 &pos, const ImVec2 &size, float u)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    Overlay_GlassPanel(dl, pos, ImVec2(pos.x + size.x, pos.y + size.y), 14.f * u, u);
    ImFont *body = Overlay_LoginFont(LoginFont::Body);
    ImFont *small = Overlay_LoginFont(LoginFont::Small);
    const float pad = 14.f * u;
    dl->AddText(small, Overlay_px(small), ImVec2(pos.x + pad + 4.f * u, pos.y + pad), COL_MUTED, "PROFESSION");

    ImGui::SetCursorScreenPos(ImVec2(pos.x + pad, pos.y + pad + Overlay_px(small) + 10.f * u));
    ImGui::BeginChild("##tabs", ImVec2(size.x - pad * 2, size.y - pad * 2 - Overlay_px(small) - 10.f * u), false, ImGuiWindowFlags_NoScrollbar);
    const float rowW = size.x - pad * 2;
    const float rowH = 66.f * u;
    for (size_t i = 0; i < s_professions.size(); i++)
    {
        CProfession *p = s_professions[i];
        const bool selected = p == s_profession;
        const bool blocked = is_blocked(p);
        ImGui::PushID(int(i));
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##tab", ImVec2(rowW, rowH)) && !blocked && !selected)
        {
            s_profession = p;
            s_error.clear();
        }
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList *cl = ImGui::GetWindowDrawList();
        const ImVec2 re(rp.x + rowW, rp.y + rowH);
        if (selected)
        {
            cl->AddRectFilled(rp, re, IM_COL32(214, 176, 104, 36), 10.f * u);
            cl->AddRect(rp, re, IM_COL32(214, 176, 104, 120), 10.f * u, ImDrawCornerFlags_All, 1.f);
        }
        else if (hovered && !blocked)
        {
            cl->AddRectFilled(rp, re, IM_COL32(255, 255, 255, 14), 10.f * u);
        }
        const float img = rowH - 12.f * u;
        const OverlayImage &pic = portrait(p);
        const ImU32 tint = blocked ? IM_COL32(255, 255, 255, 70) : IM_COL32_WHITE;
        if (pic.texture != nullptr)
        {
            cl->AddImageRounded(pic.texture, ImVec2(rp.x + 6.f * u, rp.y + 6.f * u), ImVec2(rp.x + 6.f * u + img, rp.y + 6.f * u + img),
                                ImVec2(0, 0), ImVec2(1, 1), tint, 8.f * u);
        }
        const std::string name = profession_name(p);
        cl->AddText(body, Overlay_px(body), ImVec2(rp.x + img + 20.f * u, rp.y + (rowH - Overlay_px(body)) / 2),
                    blocked ? IM_COL32(236, 230, 218, 80) : (selected ? COL_GOLD : COL_TEXT), name.c_str());
        if (hovered)
        {
            ImGui::SetMouseCursor(blocked ? ImGuiMouseCursor_Arrow : ImGuiMouseCursor_Hand);
            if (blocked)
            {
                ImGui::SetTooltip("This server doesn't offer the %s", name.c_str());
            }
        }
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0, 2.f * u));
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------------------
// The main panel

static const char *skill_name(int index)
{
    CSkill *skill = index >= 0 ? g_SkillsManager.Get(uint32_t(index)) : nullptr;
    return skill != nullptr ? skill->Name.c_str() : "Choose a skill";
}

static void draw_profession_details(float width, float u)
{
    ImFont *title = Overlay_LoginFont(LoginFont::Title);
    ImFont *small = Overlay_LoginFont(LoginFont::Small);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const std::string name = profession_name(s_profession);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddText(title, Overlay_px(title) * 0.62f, p, COL_TEXT, name.c_str());
    ImGui::Dummy(ImVec2(width, Overlay_px(title) * 0.62f + 4.f * u));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_MUTED));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::PushFont(Overlay_LoginFont(LoginFont::Small));
    ImGui::TextUnformatted(profession_description(s_profession).c_str());
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 8.f * u));

    static const char *statNames[3] = { "Strength", "Dexterity", "Intelligence" };
    const float half = width;

    if (!is_advanced(s_profession))
    {
        // fixed profession: show what it starts with
        const int stats[3] = { s_profession->Str, s_profession->Dex, s_profession->Int };
        int statTotal = 0, statMax = 0;
        stat_rules(statTotal, statMax);
        ImGui::BeginGroup();
        section_label("ATTRIBUTES");
        for (int i = 0; i < 3; i++)
        {
            const ImVec2 bp = ImGui::GetCursorScreenPos();
            ImGui::Text("%s", statNames[i]);
            ImGui::SameLine(half - 30.f * u);
            ImGui::Text("%d", stats[i]);
            const float y = bp.y + Overlay_px(Overlay_LoginFont(LoginFont::Body)) + 6.f * u;
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(bp.x, y), ImVec2(bp.x + half, y + 3.f * u), IM_COL32(255, 255, 255, 24), 2.f);
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(bp.x, y), ImVec2(bp.x + half * std::min(1.f, stats[i] / float(statMax)), y + 3.f * u), COL_GOLD, 2.f);
            ImGui::Dummy(ImVec2(0, 8.f * u));
        }
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0, 4.f * u));
        ImGui::BeginGroup();
        section_label("SKILLS");
        for (int i = 0; i < skill_slots(); i++)
        {
            const uint8_t index = s_profession->GetSkillIndex(i);
            if (index == 0xFF)
            {
                continue;
            }
            ImGui::Text("%s", skill_name(index));
            ImGui::SameLine(half - 30.f * u);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_GOLD), "%d", s_profession->GetSkillValue(i));
            ImGui::Dummy(ImVec2(0, 8.f * u));
        }
        ImGui::EndGroup();
        return;
    }

    // Advanced: attributes and skills, each with a fixed total
    int statTotal = 0, statMax = 0;
    stat_rules(statTotal, statMax);
    ImGui::BeginGroup();
    section_label("ATTRIBUTES");
    for (int i = 0; i < 3; i++)
    {
        ImGui::PushID(i);
        ImGui::PushItemWidth(half);
        int v = s_stat[i];
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(statNames[i]);
        ImGui::SameLine(width * 0.42f);
        ImGui::PushItemWidth(width * 0.58f);
        const bool moved = ImGui::SliderInt("##stat", &v, 10, statMax, "%d");
        ImGui::PopItemWidth();
        if (moved)
        {
            s_stat[i] = v;
            rebalance(s_stat, 3, i, 10, statMax, statTotal);
        }
        ImGui::PopItemWidth();
        ImGui::PopID();
    }
    ImGui::PushFont(small);
    ImGui::TextDisabled("%d points to share", statTotal);
    ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, 4.f * u));
    ImGui::BeginGroup();
    section_label("SKILLS");
    const int slots = skill_slots();
    for (int i = 0; i < slots; i++)
    {
        ImGui::PushID(100 + i);
        ImGui::PushItemWidth(half * 0.62f);
        if (ImGui::BeginCombo("##skill", skill_name(s_skill[i])))
        {
            for (uint32_t k = 0; k < g_SkillsManager.Count; k++)
            {
                const int index = g_SkillsManager.GetSortedIndex(k);
                CSkill *skill = g_SkillsManager.Get(uint32_t(index));
                if (skill == nullptr || skill->Name.empty())
                {
                    continue;
                }
                bool taken = false;
                for (int j = 0; j < slots; j++)
                {
                    taken |= j != i && s_skill[j] == index;
                }
                if (taken)
                {
                    continue; // each skill once
                }
                if (ImGui::Selectable(skill->Name.c_str(), s_skill[i] == index))
                {
                    s_skill[i] = index;
                    s_error.clear();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        ImGui::SameLine(0, 6.f * u);
        ImGui::PushItemWidth(half * 0.38f - 6.f * u);
        int v = s_skillValue[i];
        if (ImGui::SliderInt("##value", &v, 0, 50))
        {
            s_skillValue[i] = v;
            rebalance(s_skillValue, slots, i, 0, 50, skill_total());
        }
        ImGui::PopItemWidth();
        ImGui::PopID();
    }
    ImGui::PushFont(small);
    ImGui::TextDisabled("%d skill points to share", skill_total());
    ImGui::PopFont();
    ImGui::EndGroup();
}

// A labelled style stepper with its colour well beside it, filling the row.
static void style_row(const char *label, const char *id, uint8_t &style, const char *styleName, int count,
                      const char *colorId, uint16_t &color, const uint16_t *palette, int paletteCount,
                      uint16_t lo, uint16_t hi, int gump, float width, float u)
{
    section_label(label);
    const float well = 34.f * u * 2.2f;
    style = uint8_t(stepper(id, styleName, style, count, width - well - 10.f * u, u));
    ImGui::SameLine(0, 10.f * u);
    color_picker(colorId, color, palette, paletteCount, 6, lo, hi, u, ramp_step(gump), true);
    ImGui::Dummy(ImVec2(0, 4.f * u));
}

static void draw_appearance(float width, float u)
{
    auto &m = g_CreateCharacterManager;
    const float gap = 14.f * u;

    section_label("NAME");
    ImGui::PushItemWidth(width);
    ImGui::InputTextWithHint(
        "##name", "What are you called?", &s_name, ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_CallbackAlways,
        [](ImGuiInputTextCallbackData *data) -> int {
            if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter)
            {
                const ImWchar c = data->EventChar;
                return (c < 0x20 || c > 0x7E || !g_FontManager.IsPrintASCII(uint8_t(c))) ? 1 : 0;
            }
            if (data->BufTextLen > CHARACTER_NAME_MAX)
            {
                data->DeleteChars(CHARACTER_NAME_MAX, data->BufTextLen - CHARACTER_NAME_MAX);
            }
            return 0;
        });
    ImGui::PopItemWidth();
    ImGui::Dummy(ImVec2(0, 4.f * u));

    section_label("GENDER");
    static const char *genders[2] = { "Male", "Female" };
    const int g = segmented("gender", genders, 2, m.GetFemale() ? 1 : 0, width, u);
    if ((g == 1) != m.GetFemale())
    {
        m.SetFemale(g == 1);
        m.HairStyle = uint8_t(std::min<int>(m.HairStyle, m.GetCurrentHairCount() - 1));
    }
    ImGui::Dummy(ImVec2(0, 4.f * u));

    if (g_Config.ClientVersion >= CV_4011D)
    {
        section_label("RACE");
        static const char *races[3] = { "Human", "Elf", "Gargoyle" };
        const int raceCount = g_Config.ClientVersion >= CV_60144 ? 3 : 2;
        const int r = segmented("race", races, raceCount, int(m.GetRace()) - 1, width, u);
        if (r + 1 != int(m.GetRace()))
        {
            m.SetRace(RACE_TYPE(r + 1));
            m.HairStyle = uint8_t(std::min<int>(m.HairStyle, m.GetCurrentHairCount() - 1));
            m.BeardStyle = 0;
        }
        ImGui::Dummy(ImVec2(0, 4.f * u));
    }

    const RACE_TYPE race = m.GetRace();
    const int skinCount = race == RT_ELF ? CCreateCharacterManager::ELF_SKIN_TONE_COUNT :
                          race == RT_GARGOYLE ? CCreateCharacterManager::GARGOYLE_SKIN_TONE_COUNT :
                                                CCreateCharacterManager::HUMAN_SKIN_TONE_COUNT;
    const int hairCount = race == RT_ELF ? CCreateCharacterManager::ELF_HAIR_COLOR_COUNT :
                          race == RT_GARGOYLE ? CCreateCharacterManager::GARGOYLE_HAIR_COLOR_COUNT :
                                                CCreateCharacterManager::HUMAN_HAIR_COLOR_COUNT;
    const bool human = race == RT_HUMAN;
    const uint16_t hairLo = human ? 0x044E : 0;
    const uint16_t hairHi = human ? 0x047C : 0xFFFF;

    style_row(race == RT_GARGOYLE ? "HORNS" : "HAIR", "hair", m.HairStyle, m.GetHair(m.HairStyle).Name.c_str(),
              m.GetCurrentHairCount(), "haircolor", m.HairColor, m.GetHairColorPtr(), hairCount, hairLo, hairHi,
              m.GetHair(m.HairStyle).GumpID, width, u);
    if (!m.GetFemale() && race != RT_ELF)
    {
        style_row(race == RT_GARGOYLE ? "FACIAL HORNS" : "FACIAL HAIR", "beard", m.BeardStyle,
                  m.GetBeard(m.BeardStyle).Name.c_str(), m.GetCurrentFacialHairCount(), "beardcolor", m.BeardColor,
                  m.GetHairColorPtr(), hairCount, hairLo, hairHi, m.GetBeard(m.BeardStyle).GumpID, width, u);
    }

    // skin and clothing colours side by side
    auto &dyes = clothing_palette();
    const float third = (width - gap * 2) / 3;
    ImGui::BeginGroup();
    section_label("SKIN");
    color_picker("skin", m.SkinTone, m.GetSkinTonePtr(), skinCount, 8, human ? 0x03EA : 0, human ? 0x0422 : 0xFFFF,
                 u, ramp_step(m.GetBodyGump()), true);
    ImGui::EndGroup();
    ImGui::SameLine(third + gap);
    ImGui::BeginGroup();
    section_label(race == RT_GARGOYLE ? "ROBE" : "SHIRT");
    color_picker("shirt", m.ShirtColor, dyes.data(), int(dyes.size()), 20, 0, 0xFFFF, u, ramp_step(m.GetShirtGump()));
    ImGui::EndGroup();
    if (race != RT_GARGOYLE)
    {
        ImGui::SameLine((third + gap) * 2);
        ImGui::BeginGroup();
        section_label(m.GetFemale() ? "SKIRT" : "PANTS");
        color_picker("pants", m.PantsColor, dyes.data(), int(dyes.size()), 20, 0, 0xFFFF, u, ramp_step(m.GetPantsGump()));
        ImGui::EndGroup();
    }

    if (human && g_Config.ClientVersion >= CV_4011D)
    {
        // servers only accept flesh tones for humans; the fantasy ones belong to other races
        ImGui::Dummy(ImVec2(0, 4.f * u));
        ImGui::PushFont(Overlay_LoginFont(LoginFont::Small));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextDisabled(g_Config.ClientVersion >= CV_60144 ?
                                "Paler, blue or green skin? Elves and gargoyles have fantasy tones." :
                                "Paler or silvery skin? Elves have fantasy tones.");
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }
}

static bool validate()
{
    if (s_name.size() < 2)
    {
        s_error = "Give your character a name (at least 2 letters).";
        return false;
    }
    if (is_advanced(s_profession))
    {
        for (int i = 0; i < skill_slots(); i++)
        {
            if (s_skill[i] < 0)
            {
                s_error = "Choose all of your starting skills.";
                return false;
            }
        }
    }
    s_error.clear();
    return true;
}

// A scrolling column; items inside are laid out at `width` (the scrollbar sits outside it).
static void begin_column(const char *id, const ImVec2 &pos, float width, float height)
{
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginChild(id, ImVec2(width + ImGui::GetStyle().ScrollbarSize + 4.f, height), false);
    ImGui::PushItemWidth(width);
}

static void end_column()
{
    ImGui::PopItemWidth();
    ImGui::EndChild();
}

static void draw_main(const ImVec2 &pos, const ImVec2 &size, float u)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    Overlay_GlassPanel(dl, pos, ImVec2(pos.x + size.x, pos.y + size.y), 14.f * u, u);
    const float pad = 26.f * u;
    const float gap = 28.f * u;
    ImFont *body = Overlay_LoginFont(LoginFont::Body);
    const float bh = Overlay_px(body) + 22.f * u;
    const float footerH = bh + pad;
    const float inner = size.x - pad * 2;
    const float scroll = ImGui::GetStyle().ScrollbarSize + 4.f;

    // three columns: the character, how they look, what they do
    const float previewW = std::floor(inner * 0.25f);
    const float lookW = std::floor((inner - previewW - gap * 2) * 0.58f) - scroll;
    const float detailW = inner - previewW - lookW - gap * 2 - scroll * 2;
    const float colH = size.y - pad - footerH;
    const float top = pos.y + pad;

    // the character, standing in a pool of warm light
    const ImVec2 stage(pos.x + pad, top);
    const ImVec2 stageEnd(stage.x + previewW, top + colH);
    const ImVec2 floorC((stage.x + stageEnd.x) / 2, stageEnd.y - 16.f * u);
    for (int i = 10; i > 0; i--)
    {
        dl->AddCircleFilled(ImVec2(floorC.x, floorC.y - colH * 0.35f), previewW * 0.055f * i, IM_COL32(255, 214, 150, 5), 48);
    }
    for (int i = 6; i > 0; i--)
    {
        // soft shadow on the ground
        const float rx = previewW * 0.07f * i;
        dl->PathClear();
        for (int k = 0; k <= 40; k++)
        {
            const float a = k / 40.f * 6.2832f;
            dl->PathLineTo(ImVec2(floorC.x + cosf(a) * rx, floorC.y + sinf(a) * rx * 0.18f));
        }
        dl->PathFillConvex(IM_COL32(0, 0, 0, 22));
    }
    s_hoverTarget = s_hoverTargetNext; // from last frame's palettes
    s_hoverValue = s_hoverValueNext;
    s_hoverTargetNext = nullptr;
    update_preview();
    if (s_preview.texture != nullptr)
    {
        const float maxH = colH - 30.f * u;
        const float scale = std::min(maxH / s_preview.height, (previewW - 20.f * u) / s_preview.width);
        const float w = s_preview.width * scale;
        const float h = s_preview.height * scale;
        const ImVec2 a(floorC.x - w / 2, floorC.y + 4.f * u - h);
        dl->AddImage(s_preview.texture, a, ImVec2(a.x + w, a.y + h));
    }

    // how they look
    const float lookX = stage.x + previewW + gap;
    begin_column("##look", ImVec2(lookX, top), lookW, colH);
    draw_appearance(lookW, u);
    end_column();
    const float divX = lookX + lookW + scroll + gap / 2;
    dl->AddLine(ImVec2(divX, top), ImVec2(divX, top + colH), IM_COL32(255, 255, 255, 22));

    // what they do
    const float detailX = lookX + lookW + scroll + gap;
    begin_column("##details", ImVec2(detailX, top), detailW, colH);
    draw_profession_details(detailW, u);
    end_column();

    // footer: back, error, continue
    const float by = pos.y + size.y - pad - bh;
    ImGui::SetCursorScreenPos(ImVec2(pos.x + pad, by));
    if (Overlay_CardButton("Back", ImVec2(120.f * u, bh), false))
    {
        Overlay_Later([] { g_Game.InitScreen(GS_CHARACTER); });
    }
    const float goW = 240.f * u;
    if (!s_error.empty())
    {
        const ImVec2 ts = ImGui::CalcTextSize(s_error.c_str());
        dl->AddText(ImVec2(pos.x + size.x - pad - goW - 16.f * u - ts.x, by + (bh - ts.y) / 2), IM_COL32(248, 113, 113, 255), s_error.c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(pos.x + size.x - pad - goW, by));
    if (Overlay_CardButton("Choose Starting City", ImVec2(goW, bh), true) && validate())
    {
        s_cityOpen = true;
    }
}

// ---------------------------------------------------------------------------------------
// Starting city

static CCityItem *city_at(int i)
{
    return g_CityList.GetCity(i);
}

static void send_character()
{
    CCityItem *city = city_at(s_city);
    if (city == nullptr || s_profession == nullptr)
    {
        return;
    }
    if (is_advanced(s_profession))
    {
        s_profession->Str = uint8_t(s_stat[0]);
        s_profession->Dex = uint8_t(s_stat[1]);
        s_profession->Int = uint8_t(s_stat[2]);
        for (int i = 0; i < 4; i++)
        {
            s_profession->SetSkillIndex(i, i < skill_slots() ? uint8_t(s_skill[i]) : 0xFF);
            s_profession->SetSkillValue(i, uint8_t(s_skillValue[i]));
        }
    }
    g_ProfessionManager.Selected = s_profession;
    g_CreateCharacterScreen.Name = s_name;
    g_SelectTownScreen.m_City = city;
    Overlay_Later([] {
        CPacketCreateCharacter(g_CreateCharacterScreen.Name).Send();
        g_Game.InitScreen(GS_GAME_CONNECT);
    });
}

static void draw_city_overlay(const ImVec2 &display, float u)
{
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##city", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::SetWindowFontScale(u / 2.f);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), display, IM_COL32(6, 8, 12, 150));
    ImGui::SetCursorScreenPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##blocker", display); // clicks outside the card do nothing

    ImFont *title = Overlay_LoginFont(LoginFont::Title);
    ImFont *body = Overlay_LoginFont(LoginFont::Body);
    ImFont *small = Overlay_LoginFont(LoginFont::Small);
    const float w = std::min(display.x - 60.f * u, 980.f * u);
    const float h = std::min(display.y - 60.f * u, 600.f * u);
    const ImVec2 p0((display.x - w) / 2, (display.y - h) / 2);
    const ImVec2 p1(p0.x + w, p0.y + h);
    Overlay_GlassPanel(dl, p0, p1, 16.f * u, u);
    dl->AddRectFilled(p0, p1, IM_COL32(10, 12, 18, 120), 16.f * u);
    const float pad = 26.f * u;
    dl->AddText(title, Overlay_px(title) * 0.62f, ImVec2(p0.x + pad, p0.y + pad), COL_TEXT, "Where will you begin?");

    const int count = int(g_CityList.CityCount());
    s_city = std::max(0, std::min(s_city, count - 1));
    CCityItem *selected = city_at(s_city);

    // map with the cities on it (newer clients send coordinates)
    const float top = p0.y + pad + Overlay_px(title) * 0.62f + 18.f * u;
    const float bottomH = Overlay_px(body) + 22.f * u;
    const float mapSize = std::min(h - (top - p0.y) - pad * 2 - bottomH, w * 0.5f);
    const ImVec2 m0(p0.x + pad, top);
    if (selected != nullptr && selected->IsNewCity())
    {
        const int map = std::min<int>(5, ((CCityItemNew *)selected)->MapIndex);
        if (map != s_mapIndex)
        {
            Overlay_FreeImage(s_map);
            s_map = Overlay_ComposeGumps({ { uint16_t(0x15D9 + map), 0 } });
            s_mapIndex = map;
        }
        if (s_map.texture != nullptr)
        {
            dl->AddImageRounded(s_map.texture, m0, ImVec2(m0.x + mapSize, m0.y + mapSize), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 10.f * u);
            dl->AddRect(m0, ImVec2(m0.x + mapSize, m0.y + mapSize), IM_COL32(255, 255, 255, 40), 10.f * u);
            for (int i = 0; i < count; i++)
            {
                CCityItem *c = city_at(i);
                if (c == nullptr || !c->IsNewCity() || std::min<int>(5, ((CCityItemNew *)c)->MapIndex) != map)
                {
                    continue;
                }
                auto *nc = (CCityItemNew *)c;
                const float fx = float(nc->X) / std::max(1, g_MapSize[map].Width - 2048) * 383.f / s_map.width;
                const float fy = float(nc->Y) / std::max(1, g_MapSize[map].Height) * 384.f / s_map.height;
                const ImVec2 dot(m0.x + mapSize * std::min(1.f, fx), m0.y + mapSize * std::min(1.f, fy));
                const bool isSel = i == s_city;
                dl->AddCircleFilled(dot, (isSel ? 7.f : 5.f) * u, isSel ? COL_GOLD : IM_COL32(236, 230, 218, 200), 20);
                dl->AddCircle(dot, (isSel ? 7.f : 5.f) * u, IM_COL32(0, 0, 0, 160), 20, 1.5f);
                ImGui::SetCursorScreenPos(ImVec2(dot.x - 10.f * u, dot.y - 10.f * u));
                ImGui::PushID(1000 + i);
                if (ImGui::InvisibleButton("##dot", ImVec2(20.f * u, 20.f * u)))
                {
                    s_city = i;
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", c->Name.c_str());
                }
                ImGui::PopID();
            }
        }
    }

    // the list and the chosen city's description
    const float lx = (s_map.texture != nullptr && selected != nullptr && selected->IsNewCity()) ? m0.x + mapSize + pad : p0.x + pad;
    const float lw = p1.x - pad - lx;
    ImGui::SetCursorScreenPos(ImVec2(lx, top));
    ImGui::BeginChild("##cities", ImVec2(lw, (mapSize) * 0.48f), false);
    for (int i = 0; i < count; i++)
    {
        CCityItem *c = city_at(i);
        if (c == nullptr)
        {
            continue;
        }
        ImGui::PushID(i);
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        const float rh = Overlay_px(body) + 14.f * u;
        if (ImGui::InvisibleButton("##city", ImVec2(lw - 12.f * u, rh)))
        {
            s_city = i;
        }
        const bool hov = ImGui::IsItemHovered();
        ImDrawList *cl = ImGui::GetWindowDrawList();
        if (i == s_city)
        {
            cl->AddRectFilled(rp, ImVec2(rp.x + lw - 12.f * u, rp.y + rh), IM_COL32(214, 176, 104, 36), 8.f * u);
        }
        else if (hov)
        {
            cl->AddRectFilled(rp, ImVec2(rp.x + lw - 12.f * u, rp.y + rh), IM_COL32(255, 255, 255, 14), 8.f * u);
        }
        cl->AddText(body, Overlay_px(body), ImVec2(rp.x + 12.f * u, rp.y + 7.f * u), i == s_city ? COL_GOLD : COL_TEXT, c->Name.c_str());
        const float aw = small->CalcTextSizeA(Overlay_px(small), FLT_MAX, 0.f, c->Area.c_str()).x;
        cl->AddText(small, Overlay_px(small), ImVec2(rp.x + lw - 24.f * u - aw, rp.y + 9.f * u), COL_MUTED, c->Area.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (selected != nullptr)
    {
        wstr_t description = selected->m_City.Description;
        if (selected->IsNewCity())
        {
            description = g_ClilocManager.Cliloc(g_Language)->GetW(((CCityItemNew *)selected)->Cliloc);
        }
        std::string text = wstr_to_utf8(description);
        // the descriptions are small HTML documents; keep the words
        std::string plain;
        bool tag = false;
        for (char ch : text)
        {
            if (ch == '<')
                tag = true;
            else if (ch == '>')
            {
                tag = false;
                plain += ' ';
            }
            else if (!tag)
                plain += ch;
        }
        ImGui::SetCursorScreenPos(ImVec2(lx, top + mapSize * 0.48f + 12.f * u));
        ImGui::BeginChild("##citytext", ImVec2(lw, mapSize * 0.52f - 12.f * u), false);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_MUTED));
        ImGui::PushTextWrapPos(lw - ImGui::GetStyle().ScrollbarSize - 8.f * u);
        const auto first = plain.find_first_not_of(' ');
        ImGui::TextUnformatted(first == std::string::npos ? "" : plain.c_str() + first);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::EndChild();
    }

    // buttons
    const float by = p1.y - pad - bottomH;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, by));
    if (Overlay_CardButton("Back", ImVec2(120.f * u, bottomH), false) ||
        ImGui::IsKeyPressed(ImGui::GetIO().KeyMap[ImGuiKey_Escape], false))
    {
        s_cityOpen = false;
    }
    const float goW = 260.f * u;
    ImGui::SetCursorScreenPos(ImVec2(p1.x - pad - goW, by));
    if (Overlay_CardButton("Begin Your Journey", ImVec2(goW, bottomH), true) && selected != nullptr)
    {
        send_character();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------------------

void Overlay_DrawCreate()
{
    if (!s_started)
    {
        start();
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float u = std::max(0.75f, std::min(2.f, std::min(display.x / 1280.f, display.y / 800.f)));
    Overlay_SetTextScale(u);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##create", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar(2);
    ImGui::SetWindowFontScale(u / 2.f);

    // the glade: a looping video, or its first frame
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), display, IM_COL32(14, 18, 14, 255));
    if (!s_video.tried)
    {
        Overlay_VideoOpen(s_video, "create/forest.mpg");
        if (s_still.texture == nullptr)
        {
            s_still = Overlay_LoadImage("create/forest.jpg");
        }
    }
    void *frame = Overlay_VideoUpdate(s_video, ImGui::GetIO().DeltaTime);
    OverlayImage bg = s_still;
    if (frame != nullptr)
    {
        bg = { frame, s_video.width, s_video.height };
    }
    Overlay_DrawCover(dl, bg, ImVec2(0, 0), display);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(display.x * 0.45f, display.y), IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 150));
    dl->AddRectFilledMultiColor(ImVec2(0, display.y * 0.6f), display, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 160), IM_COL32(0, 0, 0, 160));

    // header
    ImFont *title = Overlay_LoginFont(LoginFont::Title);
    ImFont *body = Overlay_LoginFont(LoginFont::Body);
    const float margin = 28.f * u;
    dl->AddText(title, Overlay_px(title) * 0.7f, ImVec2(margin, margin - 4.f * u), COL_TEXT, "Create Your Character");
    const float headerH = Overlay_px(title) * 0.7f + 22.f * u;

    // style shared by the panels' widgets
    ImGui::PushFont(body);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f * u);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.f * u, 9.f * u));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.f * u, 6.f * u));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.f * u);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 14.f * u);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_TEXT));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::ColorConvertU32ToFloat4(COL_MUTED));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.38f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.f, 0.f, 0.f, 0.48f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.f, 0.f, 0.f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImGui::ColorConvertU32ToFloat4(COL_GOLD));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.91f, 0.77f, 0.50f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.84f, 0.69f, 0.41f, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.f, 1.f, 1.f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1.f, 1.f, 1.f, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.08f, 0.11f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.84f, 0.69f, 0.41f, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 1.f, 1.f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 1.f, 1.f, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.f, 1.f, 1.f, 0.20f));
    const int colors = 18;

    const float tabsW = std::min(300.f * u, display.x * 0.26f);
    const ImVec2 tabsPos(margin, margin + headerH);
    const ImVec2 tabsSize(tabsW, display.y - tabsPos.y - margin);
    if (s_profession != nullptr)
    {
        draw_tabs(tabsPos, tabsSize, u);
        const ImVec2 mainPos(margin + tabsW + 20.f * u, tabsPos.y);
        draw_main(mainPos, ImVec2(display.x - mainPos.x - margin, tabsSize.y), u);
    }

    ImGui::PopStyleColor(colors);
    ImGui::PopStyleVar(6);
    ImGui::PopFont();
    ImGui::End();

    if (s_cityOpen)
    {
        ImGui::PushFont(body);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f * u);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.f * u, 9.f * u));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
        draw_city_overlay(display, u);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        ImGui::PopFont();
    }
}

#endif
