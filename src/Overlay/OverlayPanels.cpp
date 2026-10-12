// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Overlay.h"

#if defined(NEW_RENDERER_ENABLED) && defined(USE_GL3)

#include "OverlayPanels.h"
#include <algorithm>
#include <ctime>
#include <deque>
#include <string>
#include <vector>
#include <SDL.h>
#include <external/gfx/imgui/imgui.h>
#include <xuocore/enumlist.h>
#include "../Application.h"
#include "../Config.h"
#include "../GameWindow.h"
#include "../Globals.h"
#include "../Macro.h"
#include "../CrossUO.h"
#include "../GameObjects/GameCharacter.h"
#include "../GameObjects/GameWorld.h"
#include "../GameObjects/GamePlayer.h"
#include "../Managers/ColorManager.h"
#include "../Managers/ConfigManager.h"
#include "../Managers/MacroManager.h"
#include "../Renderer/RenderAPI.h"
#include <cmath>

bool &Overlay_Visible(OverlayWindow w);

// ImGui doesn't keep windows on screen; after a resize or scale change pull them back.
static void keep_on_screen()
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    ImVec2 p = pos;
    p.x = std::max(0.f, std::min(p.x, display.x - std::min(size.x, display.x)));
    p.y = std::max(0.f, std::min(p.y, display.y - std::min(size.y, display.y)));
    if (p.x != pos.x || p.y != pos.y)
    {
        ImGui::SetWindowPos(p);
    }
}

// ---------------------------------------------------------------------------------------
// Journal

struct JournalLine
{
    time_t time;
    int type;
    ImU32 color;
    std::string text;
};

static std::deque<JournalLine> s_journal;
static const size_t JOURNAL_MAX = 5000;
static bool s_journalScrollToBottom = true;

void Overlay_JournalAdd(int textType, uint16_t hue, const astr_t &utf8)
{
    ImU32 color = IM_COL32(220, 220, 220, 255);
    hue &= 0x3FFF;
    if (hue != 0 && hue != 0x3B2)
    {
        // a bright entry of the hue ramp reads well on the dark window background
        color = g_ColorManager.GetPolygoneColor(26, hue) | 0xFF000000;
    }
    s_journal.push_back({ time(nullptr), textType, color, utf8 });
    if (s_journal.size() > JOURNAL_MAX)
    {
        s_journal.pop_front();
    }
}

static bool contains_nocase(const std::string &hay, const char *needle)
{
    if (needle[0] == 0)
    {
        return true;
    }
    const auto it = std::search(
        hay.begin(), hay.end(), needle, needle + strlen(needle), [](char a, char b) {
            return tolower((unsigned char)a) == tolower((unsigned char)b);
        });
    return it != hay.end();
}

void Overlay_DrawJournal(bool *open, float scale)
{
    static char filter[128] = {};
    static bool showSystem = true, showObjects = true, showClient = true, showTime = false;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(420 * scale, 260 * scale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10 * scale, display.y - 270 * scale), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Journal", open))
    {
        ImGui::End();
        return;
    }
    keep_on_screen();

    ImGui::SetNextItemWidth(-60 * scale);
    ImGui::InputTextWithHint("##filter", "Search", filter, sizeof(filter));
    ImGui::SameLine();
    const bool copyAll = ImGui::Button("Copy");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Copy the lines shown below");
    }
    ImGui::Checkbox("System", &showSystem);
    ImGui::SameLine();
    ImGui::Checkbox("Speech", &showObjects);
    ImGui::SameLine();
    ImGui::Checkbox("Client", &showClient);
    ImGui::SameLine();
    ImGui::Checkbox("Time", &showTime);

    std::vector<int> visible;
    visible.reserve(s_journal.size());
    for (int i = 0; i < int(s_journal.size()); ++i)
    {
        const auto &l = s_journal[i];
        const bool typeOn = (l.type == TT_SYSTEM && showSystem) ||
                            (l.type == TT_OBJECT && showObjects) ||
                            (l.type == TT_CLIENT && showClient);
        if (typeOn && contains_nocase(l.text, filter))
        {
            visible.push_back(i);
        }
    }

    ImGui::Separator();
    ImGui::BeginChild("##lines", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.f;
    std::string clip;
    ImGuiListClipper clipper(int(visible.size()));
    while (clipper.Step())
    {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const auto &l = s_journal[visible[i]];
            ImGui::PushID(visible[i]);
            if (showTime)
            {
                char ts[16];
                strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&l.time));
                ImGui::TextDisabled("%s", ts);
                ImGui::SameLine();
            }
            ImGui::PushStyleColor(ImGuiCol_Text, l.color);
            ImGui::TextWrapped("%s", l.text.c_str());
            ImGui::PopStyleColor();
            if (ImGui::BeginPopupContextItem("##line"))
            {
                if (ImGui::MenuItem("Copy line"))
                {
                    ImGui::SetClipboardText(l.text.c_str());
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    if (copyAll)
    {
        for (int i : visible)
        {
            clip += s_journal[i].text;
            clip += '\n';
        }
        ImGui::SetClipboardText(clip.c_str());
    }
    // Follow new lines unless the user scrolled up to read.
    if (s_journalScrollToBottom || atBottom)
    {
        ImGui::SetScrollHereY(1.f);
    }
    s_journalScrollToBottom = false;
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------------------
// Settings

void Overlay_DrawSettings(bool *open, float scale)
{
    ImGui::SetNextWindowSize(ImVec2(360 * scale, 420 * scale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f),
        ImGuiCond_FirstUseEver,
        ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Settings", open))
    {
        ImGui::End();
        return;
    }
    keep_on_screen();
    auto &cfg = g_ConfigManager;

    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen))
    {
        float uiScale = g_GameWindow.UIScale;
        if (ImGui::SliderFloat("UI scale", &uiScale, 1.f, 4.f, "%.2fx"))
        {
            uiScale = std::round(uiScale * 4.f) / 4.f; // 0.25 steps
            if (uiScale != g_GameWindow.UIScale)
            {
                g_Config.UIScale = uiScale;
                g_GameWindow.SetUIScale(uiScale, g_GameState >= GS_GAME);
            }
        }
        if (ImGui::Checkbox("Smooth scaling", &g_Config.SmoothScaling))
        {
            Render_SetSmoothScaling(g_Config.SmoothScaling);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Softer, even scaling (best for 1.25x, 1.5x...). Off keeps crisp pixels.");
        }
        bool fullscreen = g_GameWindow.IsFullscreen();
        if (ImGui::Checkbox("Full screen", &fullscreen))
        {
            g_GameWindow.ToggleFullscreen();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(Cmd+Ctrl+F)");
        ImGui::Checkbox("Game view fills the window", &cfg.FitGameWindow);
        int fps = cfg.GetClientFPS();
        if (ImGui::SliderInt("FPS limit", &fps, 30, 240))
        {
            cfg.SetClientFPS(fps);
        }
        bool reduce = cfg.GetReduceFPSUnactiveWindow();
        if (ImGui::Checkbox("Lower FPS in the background", &reduce))
        {
            cfg.SetReduceFPSUnactiveWindow(reduce);
            cfg.SetClientFPS(cfg.GetClientFPS()); // re-apply both delays
        }
    }

    if (ImGui::CollapsingHeader("Sound", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool sound = cfg.GetSound();
        if (ImGui::Checkbox("Sound effects", &sound))
        {
            cfg.SetSound(sound);
        }
        int sv = cfg.GetSoundVolume();
        if (ImGui::SliderInt("Effects volume", &sv, 0, 255))
        {
            cfg.SetSoundVolume(uint8_t(sv));
        }
        bool music = cfg.GetMusic();
        if (ImGui::Checkbox("Music", &music))
        {
            cfg.SetMusic(music);
        }
        int mv = cfg.GetMusicVolume();
        if (ImGui::SliderInt("Music volume", &mv, 0, 255))
        {
            cfg.SetMusicVolume(uint8_t(mv));
        }
        ImGui::Checkbox("Footsteps", &cfg.FootstepsSound);
        ImGui::SameLine();
        ImGui::Checkbox("Combat music", &cfg.CombatMusic);
    }

    if (ImGui::CollapsingHeader("Gameplay", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Always run", &cfg.AlwaysRun);
        ImGui::Checkbox("Open doors automatically", &cfg.AutoOpenDoors);
        ImGui::Checkbox("Show names of people coming into view", &cfg.ShowIncomingNames);
        ImGui::Checkbox("Show object tooltips", &cfg.UseToolTips);
        ImGui::Checkbox("Pathfind on double-click", &cfg.EnablePathfind);
        ImGui::Checkbox("Hold Shift for context menus", &cfg.HoldShiftForContextMenus);
        ImGui::Checkbox("Gray out objects out of range", &cfg.GrayOutOfRangeObjects);
        ImGui::Checkbox("See-through circle around you", &cfg.UseCircleTrans);
        if (cfg.UseCircleTrans)
        {
            int r = cfg.CircleTransRadius;
            if (ImGui::SliderInt("Circle radius", &r, 32, 200))
            {
                cfg.CircleTransRadius = r;
            }
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Cmd+J journal  Cmd+K macros  Cmd+I target  Cmd+, settings");
    ImGui::End();
}

// ---------------------------------------------------------------------------------------
// Macros

static CMacro *s_selectedMacro = nullptr;
static bool s_capturing = false;

static std::string macro_key_label(const CMacro *m)
{
    std::string s;
    if (m->Ctrl)
    {
        s += "Ctrl+";
    }
    if (m->Alt)
    {
        s += "Option+";
    }
    if (m->Shift)
    {
        s += "Shift+";
    }
    const char *name = m->Key != 0 ? SDL_GetKeyName(SDL_Keycode(m->Key)) : "";
    s += (name && name[0]) ? name : "(no key)";
    return s;
}

static CMacro *macro_conflict(const CMacro *m)
{
    if (m->Key == 0)
    {
        return nullptr;
    }
    for (auto *it = (CMacro *)g_MacroManager.m_Items; it != nullptr; it = (CMacro *)it->m_Next)
    {
        if (it != m && it->Key == m->Key && it->Alt == m->Alt && it->Ctrl == m->Ctrl &&
            it->Shift == m->Shift)
        {
            return it;
        }
    }
    return nullptr;
}

bool Overlay_MacroCaptureEvent(const SDL_Event &e)
{
    if (!s_capturing || s_selectedMacro == nullptr || e.type != SDL_KEYDOWN)
    {
        return false;
    }
    const auto sym = e.key.keysym.sym;
    switch (sym)
    {
        // a lone modifier isn't a key yet; keep waiting
        case SDLK_LCTRL:
        case SDLK_RCTRL:
        case SDLK_LALT:
        case SDLK_RALT:
        case SDLK_LSHIFT:
        case SDLK_RSHIFT:
        case SDLK_LGUI:
        case SDLK_RGUI:
            return true;
        case SDLK_ESCAPE:
            s_capturing = false;
            return true;
        default:
            break;
    }
    const auto mod = e.key.keysym.mod;
    s_selectedMacro->Key = Keycode(sym);
    s_selectedMacro->Ctrl = (mod & KMOD_CTRL) != 0;
    s_selectedMacro->Alt = (mod & KMOD_ALT) != 0;
    s_selectedMacro->Shift = (mod & KMOD_SHIFT) != 0;
    s_capturing = false;
    return true;
}

static void sub_code_range(MACRO_CODE code, int &first, int &count)
{
    first = 0;
    count = 0;
    CMacro::GetBoundByCode(code, count, first);
}

static void draw_macro_actions(CMacro *macro, float scale)
{
    int index = 0;
    CMacroObject *remove = nullptr;
    CMacroObject *moveUp = nullptr;
    for (auto *obj = (CMacroObject *)macro->m_Items; obj != nullptr;
         obj = (CMacroObject *)obj->m_Next, ++index)
    {
        ImGui::PushID(index);
        ImGui::SetNextItemWidth(130 * scale);
        int code = int(obj->Code);
        if (ImGui::Combo(
                "##code",
                &code,
                [](void *, int i, const char **out) {
                    *out = s_MacroActionName[i];
                    return true;
                },
                nullptr,
                MC_COUNT,
                16))
        {
            if (code != int(obj->Code))
            {
                CMacroObject *replacement = CMacro::CreateMacro(MACRO_CODE(code));
                macro->ChangeObject(obj, replacement);
                obj = replacement;
            }
        }

        if (obj->HasSubMenu == 1)
        {
            int first = 0, count = 0;
            sub_code_range(obj->Code, first, count);
            int sub = int(obj->SubCode) - first;
            if (sub < 0 || sub >= count)
            {
                sub = 0;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120 * scale);
            struct Range
            {
                int first;
            } range{ first };
            if (ImGui::Combo(
                    "##sub",
                    &sub,
                    [](void *data, int i, const char **out) {
                        *out = s_MacroAction[((Range *)data)->first + i];
                        return true;
                    },
                    &range,
                    count,
                    16))
            {
                obj->SubCode = MACRO_SUB_CODE(first + sub);
            }
        }
        else if (obj->HasSubMenu == 2 && obj->HaveString())
        {
            auto *so = (CMacroObjectString *)obj;
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", so->m_String.c_str());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120 * scale);
            if (ImGui::InputText("##text", buf, sizeof(buf)))
            {
                so->m_String = buf;
            }
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Up") && obj->m_Prev != nullptr)
        {
            moveUp = obj;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("X"))
        {
            remove = obj;
        }
        ImGui::PopID();
    }

    if (moveUp != nullptr)
    {
        auto *prev = moveUp->m_Prev;
        macro->Unlink(moveUp);
        macro->Insert(prev->m_Prev, moveUp);
    }
    if (remove != nullptr)
    {
        macro->Delete(remove);
        if (macro->m_Items == nullptr)
        {
            macro->Add(CMacro::CreateMacro(MC_NONE));
        }
    }
    if (ImGui::Button("Add action"))
    {
        macro->Add(CMacro::CreateMacro(MC_SAY));
    }
}

void Overlay_DrawMacros(bool *open, float scale)
{
    ImGui::SetNextWindowSize(ImVec2(600 * scale, 320 * scale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f),
        ImGuiCond_FirstUseEver,
        ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Macros", open))
    {
        ImGui::End();
        return;
    }
    keep_on_screen();

    bool selectedExists = false;
    ImGui::BeginChild("##list", ImVec2(210 * scale, 0), true);
    int i = 0;
    for (auto *m = (CMacro *)g_MacroManager.m_Items; m != nullptr; m = (CMacro *)m->m_Next, ++i)
    {
        ImGui::PushID(i);
        std::string label = macro_key_label(m);
        if (auto *first = (CMacroObject *)m->m_Items)
        {
            label += ": ";
            label += s_MacroActionName[first->Code];
            if (first->HasSubMenu == 1 && first->SubCode != MSC_NONE)
            {
                label += " ";
                label += s_MacroAction[first->SubCode];
            }
        }
        if (ImGui::Selectable(label.c_str(), m == s_selectedMacro))
        {
            s_selectedMacro = m;
            s_capturing = false;
        }
        if (m == s_selectedMacro)
        {
            selectedExists = true;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (!selectedExists)
    {
        s_selectedMacro = nullptr;
        s_capturing = false;
    }

    ImGui::SameLine();
    ImGui::BeginGroup();
    if (ImGui::Button("New macro"))
    {
        auto *m = CMacro::CreateBlankMacro();
        g_MacroManager.Add(m);
        s_selectedMacro = m;
        s_capturing = true;
    }
    if (s_selectedMacro != nullptr)
    {
        ImGui::SameLine();
        if (ImGui::Button("Delete macro"))
        {
            g_MacroManager.Delete(s_selectedMacro);
            s_selectedMacro = nullptr;
            s_capturing = false;
        }
    }
    ImGui::Separator();

    if (s_selectedMacro != nullptr)
    {
        auto *m = s_selectedMacro;
        ImGui::Text("Key:");
        ImGui::SameLine();
        const auto label = s_capturing ? std::string("Press a key... (Esc cancels)") : macro_key_label(m);
        if (ImGui::Button(label.c_str()))
        {
            s_capturing = true;
        }
        if (auto *other = macro_conflict(m))
        {
            (void)other;
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Another macro uses this key; only one will run.");
        }
        ImGui::TextDisabled("Option = Alt. Cmd shortcuts are reserved for the app.");
        ImGui::Separator();
        draw_macro_actions(m, scale);
    }
    else
    {
        ImGui::TextWrapped("Pick a macro on the left, or create a new one.");
    }
    ImGui::EndGroup();
    ImGui::End();
}

// ---------------------------------------------------------------------------------------
// Target / vitals

static void vital_bar(const char *label, int value, int max, ImU32 color, float scale)
{
    const float f = max > 0 ? std::min(1.f, float(value) / float(max)) : 0.f;
    char text[64];
    snprintf(text, sizeof(text), "%s %d / %d", label, value, max);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
    ImGui::ProgressBar(f, ImVec2(-1, 16 * scale), text);
    ImGui::PopStyleColor();
}

static void character_block(CGameCharacter *c, const char *title, float scale)
{
    ImGui::TextDisabled("%s", title);
    if (c == nullptr)
    {
        ImGui::TextUnformatted("none");
        return;
    }
    const uint32_t rgb =
        g_ColorManager.GetPolygoneColor(16, g_ConfigManager.GetColorByNotoriety(c->Notoriety));
    ImGui::PushStyleColor(ImGuiCol_Text, rgb | 0xFF000000);
    ImGui::TextUnformatted(c->GetName().empty() ? "(unnamed)" : c->GetName().c_str());
    ImGui::PopStyleColor();
    if (g_Player != nullptr && c != g_Player)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%d tiles", GetDistance(g_Player, c));
    }
    vital_bar("HP", c->Hits, c->MaxHits, IM_COL32(200, 50, 50, 255), scale);
    if (c == g_Player)
    {
        vital_bar("Mana", c->Mana, c->MaxMana, IM_COL32(60, 90, 220, 255), scale);
        vital_bar("Stam", c->Stam, c->MaxStam, IM_COL32(220, 170, 40, 255), scale);
    }
}

void Overlay_DrawTarget(bool *open, float scale)
{
    ImGui::SetNextWindowSize(ImVec2(220 * scale, 210 * scale), ImGuiCond_FirstUseEver);
    // right edge, under where the paperdoll opens
    ImGui::SetNextWindowPos(
        ImVec2(ImGui::GetIO().DisplaySize.x - 230 * scale, 330 * scale), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Vitals", open))
    {
        ImGui::End();
        return;
    }
    keep_on_screen();
    character_block(g_Player, "You", scale);
    ImGui::Separator();
    CGameCharacter *target = nullptr;
    if (g_World != nullptr)
    {
        uint32_t serial = g_LastAttackObject != 0 ? g_LastAttackObject : g_LastTargetObject;
        if (serial != 0 && serial != g_PlayerSerial)
        {
            target = g_World->FindWorldCharacter(serial);
        }
    }
    character_block(target, g_LastAttackObject != 0 ? "Attacking" : "Last target", scale);
    ImGui::End();
}

// ---------------------------------------------------------------------------------------
// Persisted visibility

static fs_path state_path()
{
    return g_App.FilePath("overlay_windows.cfg");
}

void Overlay_LoadState()
{
    FILE *f = fs_open(state_path(), FS_READ);
    if (f == nullptr)
    {
        return;
    }
    int j = 0, s = 0, m = 0, t = 0;
    if (fscanf(f, "%d %d %d %d", &j, &s, &m, &t) == 4)
    {
        Overlay_Visible(OverlayWindow::Journal) = j != 0;
        Overlay_Visible(OverlayWindow::Macros) = m != 0;
        Overlay_Visible(OverlayWindow::Target) = t != 0;
        (void)s; // Settings always starts closed
    }
    fs_close(f);
}

void Overlay_SaveState()
{
    FILE *f = fs_open(state_path(), FS_WRITE);
    if (f == nullptr)
    {
        return;
    }
    fprintf(
        f,
        "%d %d %d %d\n",
        int(Overlay_Visible(OverlayWindow::Journal)),
        0,
        int(Overlay_Visible(OverlayWindow::Macros)),
        int(Overlay_Visible(OverlayWindow::Target)));
    fs_close(f);
}

#endif
