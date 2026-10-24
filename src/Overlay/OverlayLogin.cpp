// SPDX-License-Identifier: AGPL-3.0-or-later

// The login screen: a full-window scene drawn with the overlay instead of the classic
// treasure-chest gump. The gump still exists and owns the account/password/checkbox state;
// this screen reads and writes that same state, so Load/Save, auto login and launcher
// credentials keep working unchanged.

#include "Overlay.h"

#if defined(NEW_RENDERER_ENABLED) && defined(USE_GL3)

#include "OverlayPanels.h"
#include "OverlayKit.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <external/gfx/imgui/imgui.h>
#include <external/gfx/imgui/imgui_stdlib.h>
#include <external/tinyfiledialogs.h>
#include "../Application.h"
#include "../Config.h"
#include "../LoginServers.h"
#include "../ServerList.h"
#include "../CharacterList.h"
#include "../CharacterRoster.h"
#include "../Network/Packets.h"
#include "../ScreenStages/CharacterListScreen.h"
#include "../ScreenStages/ServerScreen.h"
#include "../Platform.h"
#include "../GUI/GUICheckbox.h"
#include "../Managers/FontsManager.h"
#include "../Managers/ScreenEffectManager.h"
#include "../Renderer/RenderAPI.h"
#include "../ScreenStages/MainScreen.h"
#include "../ScreenStages/ConnectionScreen.h"
#include "../Managers/ConnectionManager.h"
#include "../CrossUO.h"
#include "../TextEngine/EntryText.h"
#include "revision.h" // after Config.h: its CLIENT_VERSION macro clashes with an enum name


static const char *URL_ACCOUNT = "https://accounts.eamythic.com/";
static const char *URL_UOCOM = "https://uo.com";
static const char *URL_HELP = "https://github.com/ChristopherBrand/EmmexUO";
static const char *URL_CREDITS = "https://github.com/ChristopherBrand/EmmexUO/graphs/contributors";

// the classic screen accepted at most 16 characters per field
static const int FIELD_MAX = 16;

// backdrop layers: a looping cloud strip drifting behind the castle and mountains
static OverlayImage s_clouds;
static OverlayImage s_foreground;
static bool s_backdropTried = false;

static char s_account[FIELD_MAX + 1];
static char s_password[FIELD_MAX + 1];
static bool s_accountActive = false;
static bool s_passwordActive = false;
static bool s_wasShown = false;




void Overlay_LoginShutdown()
{
    Render_OverlayDestroyImage(s_clouds.texture);
    Render_OverlayDestroyImage(s_foreground.texture);
    s_clouds = {};
    s_foreground = {};
    s_backdropTried = false;
}

// Painted backdrop: the foreground covers the window with a slow drift and zoom; its sky is
// transparent, and a seamless cloud strip scrolls behind it.
static void draw_backdrop(ImDrawList *dl, const ImVec2 &size)
{
    if (!s_backdropTried)
    {
        s_backdropTried = true;
        s_clouds = Overlay_LoadImage("login/clouds.jpg");
        s_foreground = Overlay_LoadImage("login/foreground.jpg", "login/foreground_alpha.png");
    }
    dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(32, 28, 48, 255));
    const float t = float(ImGui::GetTime());
    const float pi = 3.14159f;

    // where the foreground lands: cover the window, zoomed and drifting a little
    float rectW = size.x;
    float rectH = size.y;
    ImVec2 rectMin(0, 0);
    if (s_foreground.texture != nullptr)
    {
        const float zoom = 1.f + 0.035f * (0.5f - 0.5f * cosf(t * 2.f * pi / 90.f));
        const float scale =
            std::max(size.x / s_foreground.width, size.y / s_foreground.height) * zoom;
        rectW = s_foreground.width * scale;
        rectH = s_foreground.height * scale;
        const float drift = (rectW - size.x) * 0.25f * sinf(t * 2.f * pi / 120.f);
        rectMin = ImVec2((size.x - rectW) / 2 + drift, (size.y - rectH) / 2);
    }
    // the painting's sea horizon sits a little above the middle
    const float horizonY = rectMin.y + rectH * 0.42f;

    // clouds: from the top of the window to just below the horizon, looping every ~6 minutes
    if (s_clouds.texture != nullptr)
    {
        const float h = horizonY + rectH * 0.06f;
        const float w = h * s_clouds.width / s_clouds.height;
        const float offset = fmodf(t * w / 360.f, w);
        for (float x = -offset; x < size.x; x += w)
        {
            // sub-pixel positions: rounding to whole points makes slow motion step
            dl->AddImage(s_clouds.texture, ImVec2(x, 0), ImVec2(x + w + 1, h));
        }
    }

    // the sun stays put low on the left while the clouds pass in front of its glow
    const ImVec2 sun(rectMin.x + rectW * 0.16f, horizonY - rectH * 0.08f);
    for (int i = 32; i > 0; i--)
    {
        const float r = rectH * 0.019f * i;
        dl->AddCircleFilled(sun, r, IM_COL32(255, 196, 128, 4), 96);
    }

    if (s_foreground.texture != nullptr)
    {
        dl->AddImage(
            s_foreground.texture, rectMin, ImVec2(rectMin.x + rectW, rectMin.y + rectH));
    }

    // darken the top a little and the bottom a lot, so text stays readable on any art
    const float h = size.y;
    dl->AddRectFilledMultiColor(
        ImVec2(0, 0),
        ImVec2(size.x, h * 0.35f),
        IM_COL32(0, 0, 0, 110),
        IM_COL32(0, 0, 0, 110),
        IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, 0));
    dl->AddRectFilledMultiColor(
        ImVec2(0, h * 0.45f),
        size,
        IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, 200),
        IM_COL32(0, 0, 0, 200));
    const float side = size.x * 0.22f;
    dl->AddRectFilledMultiColor(
        ImVec2(0, 0),
        ImVec2(side, h),
        IM_COL32(0, 0, 0, 120),
        IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, 120));
    dl->AddRectFilledMultiColor(
        ImVec2(size.x - side, 0),
        size,
        IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, 120),
        IM_COL32(0, 0, 0, 120),
        IM_COL32(0, 0, 0, 0));
}


static int filter_field_char(ImGuiInputTextCallbackData *data)
{
    // same rule as CMainScreen::OnTextInput: printable ASCII the UO fonts can draw
    const ImWchar c = data->EventChar;
    if (c < 0x20 || c > 0x7E || !g_FontManager.IsPrintASCII(uint8_t(c)))
    {
        return 1;
    }
    return 0;
}

static void sync_from_model()
{
    if (!s_accountActive && g_MainScreen.m_Account != nullptr)
    {
        const astr_t text = g_MainScreen.m_Account->GetTextA();
        snprintf(s_account, sizeof(s_account), "%s", text.c_str());
    }
    if (!s_passwordActive && g_MainScreen.m_Password != nullptr)
    {
        const astr_t text = g_MainScreen.m_Password->GetTextA();
        snprintf(s_password, sizeof(s_password), "%s", text.c_str());
    }
}



// The login options are preferences: keep them as soon as they change.
static void save_login_options()
{
    g_MainScreen.Save();
    SaveGlobalConfig();
    LoginServers_Save();
}

static bool busy()
{
    return g_MainScreen.SmoothScreenAction != 0 || g_ScreenEffectManager.Mode != SEM_NONE;
}

static void login()
{
    if (busy())
    {
        return;
    }
    if (LoginServers_NeedsRestart())
    {
        Overlay_Later(LoginServers_Restart);
        return;
    }
    g_ReconnectLogin = false; // a login the player started follows their own settings
    LoginServers_Save(); // remembers this server's account
    Overlay_Later([] { g_MainScreen.CreateSmoothAction(CMainScreen::ID_SMOOTH_MS_CONNECT); });
}






static void back_to_login()
{
    Overlay_Later([] {
        g_ConnectionScreen.CreateSmoothAction(CConnectionScreen::ID_SMOOTH_CS_GO_SCREEN_MAIN);
    });
}

// The card while connecting to the login server: progress, or why it failed.
static void draw_status(const ImVec2 &pos, float width, float height, float pad, float u)
{
    ImFont *body = Overlay_LoginFont(LoginFont::Body);
    ImFont *small = Overlay_LoginFont(LoginFont::Small);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const auto type = g_ConnectionScreen.GetType();
    const bool lost = type == CST_CONLOST;
    // game-login messages are always answers to a request; delete has no waiting state here
    const bool failed = g_ConnectionScreen.GetConnectionFailed() || type == CST_GAME_LOGIN ||
                        type == CST_CHARACTER_LIST;
    const bool waiting = !lost && !failed;
    // past the shard: errors lead back to the character list rather than to the login
    const bool characterStep = type == CST_GAME || type == CST_GAME_LOGIN || type == CST_CHARACTER_LIST;
    const float innerW = width - pad * 2;

    // which server (or shard), at the top
    const auto &servers = LoginServers_List();
    const int active = LoginServers_Active();
    CServer *shard = g_GameState == GS_SERVER_CONNECT ? g_ServerList.GetSelectedServer() : nullptr;
    std::string character;
    if (g_GameState == GS_GAME_CONNECT)
    {
        character = g_CharacterList.LastCharacterName;
    }
    else if (g_GameState == GS_DELETE)
    {
        character = g_CharacterList.GetSelectedName();
    }
    if (!character.empty() || shard != nullptr || (active >= 0 && active < int(servers.size())))
    {
        std::string label = !character.empty() ? character :
                            shard != nullptr  ? shard->Name :
                                                LoginServers_DisplayName(servers[active]);
        std::transform(label.begin(), label.end(), label.begin(), ::toupper);
        Overlay_TextWrappedCentered(dl, small, pos.x + pad, innerW, pos.y + pad, COL_MUTED, label.c_str());
    }

    // icon and message, centred in the space above the buttons
    const float buttonH = Overlay_px(body) + 22.f * u;
    const float iconR = 22.f * u;
    const ImVec2 iconC(pos.x + width / 2, pos.y + height * 0.32f);
    if (waiting)
    {
        Overlay_Spinner(dl, iconC, iconR, 3.f * u);
    }
    else
    {
        Overlay_AlertIcon(dl, iconC, iconR, lost ? COL_MUTED : IM_COL32(248, 113, 113, 255), u);
    }
    const astr_t message = g_ConnectionScreen.Message();
    Overlay_TextWrappedCentered(
        dl, body, pos.x + pad, innerW, iconC.y + iconR + 18.f * u, COL_TEXT, message.c_str());

    // buttons along the bottom, where Log In was
    ImGui::PushFont(body);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f * u);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.f * u, 11.f * u));
    ImGui::SetCursorScreenPos(ImVec2(pos.x + pad, pos.y + height - pad - buttonH));
    const ImGuiIO &io = ImGui::GetIO();
    const bool enter = ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Enter], false);
    const bool escape = ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Escape], false);
    if (waiting)
    {
        if (Overlay_CardButton("Cancel", ImVec2(innerW, buttonH), false) || escape)
        {
            Overlay_Later([] {
                g_ConnectionManager.Disconnect();
                g_ConnectionScreen.CreateSmoothAction(CConnectionScreen::ID_SMOOTH_CS_GO_SCREEN_MAIN);
            });
        }
    }
    else if (characterStep)
    {
        if (Overlay_CardButton("Back to Characters", ImVec2(innerW, buttonH), true) || enter || escape)
        {
            Overlay_Later([] {
                g_ConnectionScreen.CreateSmoothAction(CConnectionScreen::ID_SMOOTH_CS_GO_SCREEN_CHARACTER);
            });
        }
    }
    else if (lost)
    {
        if (Overlay_CardButton("Back to Login", ImVec2(innerW, buttonH), true) || enter || escape)
        {
            back_to_login();
        }
    }
    else
    {
        const float gap = 10.f * u;
        const float half = (innerW - gap) / 2;
        if (Overlay_CardButton("Back", ImVec2(half, buttonH), false) || escape)
        {
            back_to_login();
        }
        ImGui::SameLine(0, gap);
        if (Overlay_CardButton("Try Again", ImVec2(half, buttonH), true) || enter)
        {
            Overlay_Later([] { g_Game.Connect(); });
        }
    }
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
}

static void draw_card(const ImVec2 &display, float x0, float contentW, float u)
{
    ImFont *body = Overlay_LoginFont(LoginFont::Body);
    ImFont *small = Overlay_LoginFont(LoginFont::Small);
    const float width = std::min(380.f * u, contentW - 32.f * u);
    const float pad = 28.f * u;
    const float fieldH = Overlay_px(body) + 22.f * u;
    const float height = pad * 2 + Overlay_px(small) * 2 + fieldH * 3 + 4.f * u + Overlay_px(body) +
                         62.f * u;
    const ImVec2 pos(
        floorf(x0 + (contentW - width) / 2), floorf(std::max(display.y * 0.42f, 190.f * u)));

    // frosted glass: translucent dark fill, a lighter top edge and a hairline border
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float r = 14.f * u;
    const ImVec2 end(pos.x + width, pos.y + height);
    dl->AddRectFilled(
        ImVec2(pos.x, pos.y + 6.f * u), ImVec2(end.x, end.y + 10.f * u), IM_COL32(0, 0, 0, 70), r);
    dl->AddRectFilled(pos, end, IM_COL32(18, 20, 28, 178), r);
    dl->AddRectFilledMultiColor(
        ImVec2(pos.x + r, pos.y + 1),
        ImVec2(end.x - r, pos.y + 40.f * u),
        IM_COL32(255, 255, 255, 14),
        IM_COL32(255, 255, 255, 14),
        IM_COL32(255, 255, 255, 0),
        IM_COL32(255, 255, 255, 0));
    dl->AddRect(pos, end, IM_COL32(255, 255, 255, 38), r, ImDrawCornerFlags_All, 1.f);

    if (g_GameState == GS_MAIN_CONNECT || g_GameState == GS_SERVER_CONNECT || g_GameState == GS_DELETE ||
        g_GameState == GS_GAME_CONNECT)
    {
        draw_status(pos, width, height, pad, u);
        return;
    }

    ImGui::SetCursorScreenPos(ImVec2(pos.x + pad, pos.y + pad));
    ImGui::BeginGroup();
    ImGui::PushItemWidth(width - pad * 2);
    ImGui::PushFont(body);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f * u);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.f * u, 11.f * u));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.f * u, 6.f * u));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_TEXT));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::ColorConvertU32ToFloat4(COL_MUTED));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.38f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.f, 0.f, 0.f, 0.48f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.f, 0.f, 0.f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImGui::ColorConvertU32ToFloat4(COL_GOLD));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.84f, 0.69f, 0.41f, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImGui::ColorConvertU32ToFloat4(COL_GOLD));

    sync_from_model();
    bool changed = false;
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackCharFilter |
                                      ImGuiInputTextFlags_EnterReturnsTrue |
                                      ImGuiInputTextFlags_AutoSelectAll;
    // first frame of the screen: type straight into the password if the account is known
    const bool focusAccount = !s_wasShown && s_account[0] == 0;
    const bool focusPassword = !s_wasShown && s_account[0] != 0;

    ImGui::PushFont(small);
    ImGui::TextDisabled("ACCOUNT");
    ImGui::PopFont();
    if (focusAccount)
    {
        ImGui::SetKeyboardFocusHere();
    }
    if (ImGui::InputTextWithHint(
            "##account", "Account name", s_account, sizeof(s_account), flags, filter_field_char))
    {
        login();
    }
    changed |= ImGui::IsItemEdited();
    s_accountActive = ImGui::IsItemActive();
    if (ImGui::IsItemHovered() && !s_accountActive)
    {
        ImGui::SetTooltip("Your Ultima Online account name");
    }

    ImGui::Dummy(ImVec2(0, 4.f * u));
    ImGui::PushFont(small);
    ImGui::TextDisabled("PASSWORD");
    ImGui::PopFont();
    if (focusPassword)
    {
        ImGui::SetKeyboardFocusHere();
    }
    if (ImGui::InputTextWithHint(
            "##password",
            "Password",
            s_password,
            sizeof(s_password),
            flags | ImGuiInputTextFlags_Password,
            filter_field_char))
    {
        login();
    }
    changed |= ImGui::IsItemEdited();
    s_passwordActive = ImGui::IsItemActive();

    if (changed)
    {
        g_MainScreen.SetAccounting(s_account, s_password);
    }

    ImGui::Dummy(ImVec2(0, 6.f * u));
    ImGui::PushFont(small);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.f * u, 3.f * u));
    if (g_MainScreen.m_SavePassword != nullptr)
    {
        if (ImGui::Checkbox("Save password", &g_MainScreen.m_SavePassword->Checked))
        {
            save_login_options();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Save your password in the config file");
        }
    }
    if (g_MainScreen.m_AutoLogin != nullptr)
    {
        ImGui::SameLine(width - pad * 2 - ImGui::CalcTextSize("Auto login").x - 26.f * u);
        if (ImGui::Checkbox("Auto login", &g_MainScreen.m_AutoLogin->Checked))
        {
            save_login_options();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Log in automatically, skipping the selection screens");
        }
    }
    ImGui::PopStyleVar();
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0, 10.f * u));
    const bool canLogin = !busy();
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(COL_GOLD));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.91f, 0.77f, 0.50f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.74f, 0.59f, 0.33f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.08f, 0.05f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.f, 0.f, 0.f, 0.f));
    const char *action = !canLogin                      ? "Connecting..." :
                         LoginServers_NeedsRestart() ? "Restart to Switch Server" :
                                                       "Log In";
    if (ImGui::Button(action, ImVec2(width - pad * 2, fieldH)) && canLogin)
    {
        login();
    }
    ImGui::PopStyleColor(5);

    ImGui::PopStyleColor(9);
    ImGui::PopStyleVar(4);
    ImGui::PopFont();
    ImGui::PopItemWidth();
    ImGui::EndGroup();

    // Enter anywhere on the screen logs in, as on the classic screen
    if (!ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGui::GetIO().KeyMap[ImGuiKey_Enter], false))
    {
        login();
    }
}

static void draw_footer(const ImVec2 &display, float u)
{
    ImFont *small = Overlay_LoginFont(LoginFont::Small);
    ImFont *body = Overlay_LoginFont(LoginFont::Body);
    const float margin = 28.f * u;
    const float lineH = Overlay_px(small);

    // links, bottom left
    ImGui::PushFont(body);
    ImGui::SetCursorScreenPos(ImVec2(margin, display.y - margin - lineH - Overlay_px(body) - 10.f * u));
    if (Overlay_Link("Account", "Manage your account on the EA website"))
    {
        Platform::OpenBrowser(URL_ACCOUNT);
    }
    const char *sep = "  \xC2\xB7  ";
    ImGui::SameLine(0, 0);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_MUTED), "%s", sep);
    ImGui::SameLine(0, 0);
    if (Overlay_Link("UO.com", "Ultima Online news and guides"))
    {
        Platform::OpenBrowser(URL_UOCOM);
    }
    ImGui::SameLine(0, 0);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_MUTED), "%s", sep);
    ImGui::SameLine(0, 0);
    if (Overlay_Link("Help", "EmmexUO on GitHub"))
    {
        Platform::OpenBrowser(URL_HELP);
    }
    ImGui::SameLine(0, 0);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_MUTED), "%s", sep);
    ImGui::SameLine(0, 0);
    if (Overlay_Link("Credits", "The people who built this client"))
    {
        Platform::OpenBrowser(URL_CREDITS);
    }

    // quit, bottom right
    const char *quit = "Quit";
    const ImVec2 quitSize = ImGui::CalcTextSize(quit);
    ImGui::SetCursorScreenPos(ImVec2(
        display.x - margin - quitSize.x, display.y - margin - lineH - Overlay_px(body) - 10.f * u));
    if (Overlay_Link(quit, "Quit Ultima Online"))
    {
        Overlay_Later([] { g_MainScreen.ProcessSmoothAction(CMainScreen::ID_SMOOTH_MS_QUIT); });
    }
    ImGui::PopFont();

    // versions, along the very bottom
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const astr_t uo = "Ultima Online " + g_Config.ClientVersionString;
    dl->AddText(small, Overlay_px(small), ImVec2(margin, display.y - margin - lineH + 4.f * u),
                IM_COL32(236, 230, 218, 90), uo.c_str());
    const char *client = CLIENT_VERSION;
    const float cw = small->CalcTextSizeA(Overlay_px(small), FLT_MAX, 0.f, client).x;
    dl->AddText(small, Overlay_px(small),
                ImVec2(display.x - margin - cw, display.y - margin - lineH + 4.f * u),
                IM_COL32(236, 230, 218, 90), client);
}

// ---------------------------------------------------------------------------------------
// Server picker: a pill in the top left, and a panel over the left side to switch servers,
// edit them in place, and add new ones.

static bool s_panelOpen = false;
static float s_panelT = 0.f; // slide-in animation, 0 hidden .. 1 open
static std::vector<char> s_expanded;
static int s_focusName = -1;
static int s_confirmRemove = -1;
static double s_confirmRemoveUntil = 0;

static float panel_width(const ImVec2 &display, float u)
{
    return std::min(std::max(420.f * u, display.x * 0.36f), display.x - 40.f * u);
}

static ImU32 status_color(ServerStatus status)
{
    switch (status)
    {
        case ServerStatus::Online:
            return IM_COL32(74, 222, 128, 255);
        case ServerStatus::Offline:
            return IM_COL32(248, 113, 113, 255);
        default:
            return IM_COL32(160, 160, 160, 200);
    }
}

static const char *status_text(ServerStatus status)
{
    switch (status)
    {
        case ServerStatus::Online:
            return "Online";
        case ServerStatus::Offline:
            return "Offline: no response from the login server";
        default:
            return "Checking...";
    }
}

static void status_dot(ImDrawList *dl, const ImVec2 &c, float r, ServerStatus status)
{
    const ImU32 col = status_color(status);
    if (status == ServerStatus::Online)
    {
        // soft halo so "online" reads at a glance
        dl->AddCircleFilled(c, r * 2.f, (col & 0x00FFFFFF) | 0x30000000, 20);
    }
    dl->AddCircleFilled(c, r, col, 16);
}



static void draw_server_pill(float u)
{
    ImFont *body = Overlay_LoginFont(LoginFont::Body);
    const auto &servers = LoginServers_List();
    const int active = LoginServers_Active();
    const LoginServer *server = active >= 0 ? &servers[active] : nullptr;
    const char *name = server != nullptr ? LoginServers_DisplayName(*server) : "Choose a server";

    ImGui::PushFont(body);
    const ImVec2 textSize = ImGui::CalcTextSize(name);
    const float h = Overlay_px(body) + 18.f * u;
    const ImVec2 pos(24.f * u, 22.f * u);
    const ImVec2 size(textSize.x + 74.f * u, h);
    ImGui::SetCursorScreenPos(pos);
    const bool pressed = ImGui::InvisibleButton("##serverpill", size);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, end, hovered ? IM_COL32(18, 20, 28, 210) : IM_COL32(18, 20, 28, 160), h / 2);
    dl->AddRect(pos, end, IM_COL32(255, 255, 255, hovered ? 60 : 34), h / 2, ImDrawCornerFlags_All, 1.f);
    const float cy = pos.y + h / 2;
    const ServerStatus status = server != nullptr ? LoginServers_Status(*server) : ServerStatus::Unknown;
    status_dot(dl, ImVec2(pos.x + 18.f * u, cy), 4.5f * u, status);
    dl->AddText(ImVec2(pos.x + 32.f * u, cy - textSize.y / 2), COL_TEXT, name);
    Overlay_Chevron(dl, ImVec2(end.x - 20.f * u, cy), 4.f * u, Chevron::Down, hovered ? COL_GOLD : COL_MUTED, 1.6f * u);
    ImGui::PopFont();

    if (hovered)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (server != nullptr)
        {
            ImGui::SetTooltip("%s:%d  \xC2\xB7  %s", server->Host.c_str(), server->Port, status_text(status));
        }
    }
    if (pressed)
    {
        s_panelOpen = !s_panelOpen;
    }
}


static void draw_server_editor(int index, LoginServer &server, float width, float u)
{
    const bool isActive = index == LoginServers_Active();
    const float gap = 10.f * u;
    bool save = false;
    bool live = false;

    ImGui::Dummy(ImVec2(0, 2.f * u));
    const bool focus = s_focusName == index;
    s_focusName = focus ? -1 : s_focusName;
    save |= Overlay_Field("NAME", "Unnamed server", &server.Name, width, focus);

    // side-by-side fields: line both groups up on the same top edge
    const float portW = 86.f * u;
    float rowY = ImGui::GetCursorPosY();
    if (Overlay_Field("ADDRESS", "login.example.com", &server.Host, width - portW - gap))
    {
        live = true;
    }
    ImGui::SameLine(0, gap);
    ImGui::SetCursorPosY(rowY);
    std::string port = std::to_string(server.Port);
    if (Overlay_Field("PORT", "2593", &port, portW, false, ImGuiInputTextFlags_CharsDecimal))
    {
        server.Port = std::max(1, std::min(65535, atoi(port.c_str())));
        live = true;
    }

    save |= Overlay_Field("CLIENT VERSION", "7.0.33.1", &server.ClientVersion, width);

    const float browseW = 84.f * u;
    rowY = ImGui::GetCursorPosY();
    save |= Overlay_Field("GAME DATA FOLDER", "Folder with the UO data files", &server.DataPath, width - browseW - gap);
    ImGui::SameLine(0, gap);
    ImGui::SetCursorPosY(rowY);
    ImGui::BeginGroup();
    ImGui::PushFont(Overlay_LoginFont(LoginFont::Small));
    ImGui::TextDisabled(" ");
    ImGui::PopFont();
    if (ImGui::Button("Browse...", ImVec2(browseW, 0)))
    {
        const char *picked = tinyfd_selectFolderDialog(
            "Choose the Ultima Online data folder", server.DataPath.empty() ? nullptr : server.DataPath.c_str());
        if (picked != nullptr)
        {
            server.DataPath = picked;
            save = true;
        }
    }
    ImGui::EndGroup();

    ImGui::Dummy(ImVec2(0, 2.f * u));
    ImGui::PushFont(Overlay_LoginFont(LoginFont::Small));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.f * u, 3.f * u));
    save |= ImGui::Checkbox("Encrypted connection", &server.Crypt);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Only for servers that require an encrypted client");
    }
    ImGui::PopStyleVar();

    // remove, with a second click to confirm
    const bool confirming = s_confirmRemove == index && ImGui::GetTime() < s_confirmRemoveUntil;
    const char *removeLabel = confirming ? "Click again to remove" : "Remove server";
    ImGui::SameLine(width - ImGui::CalcTextSize(removeLabel).x);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.97f, 0.44f, 0.44f, confirming ? 1.f : 0.75f));
    if (ImGui::Selectable(removeLabel, false, 0, ImGui::CalcTextSize(removeLabel)))
    {
        if (confirming)
        {
            LoginServers_Remove(index);
            s_expanded.erase(s_expanded.begin() + index);
            s_confirmRemove = -1;
            ImGui::PopStyleColor();
            ImGui::PopFont();
            return;
        }
        s_confirmRemove = index;
        s_confirmRemoveUntil = ImGui::GetTime() + 3.0;
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 6.f * u));

    if (live)
    {
        if (isActive)
        {
            LoginServers_ApplyActive();
        }
        else
        {
            LoginServers_Save();
        }
        LoginServers_Recheck();
    }
    else if (save)
    {
        LoginServers_Save();
    }
}

static void draw_server_row(int index, float width, float u)
{
    ImFont *body = Overlay_LoginFont(LoginFont::Body);
    ImFont *small = Overlay_LoginFont(LoginFont::Small);
    auto &server = LoginServers_List()[index];
    const bool isActive = index == LoginServers_Active();
    const bool expanded = s_expanded[index] != 0;
    const float rowH = Overlay_px(body) + Overlay_px(small) + 26.f * u;
    const float chevronW = 44.f * u;

    ImGui::PushID(index);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##select", ImVec2(width - chevronW, rowH));
    const bool rowHovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked() && !isActive)
    {
        LoginServers_Activate(index);
    }
    ImGui::SameLine(0, 0);
    if (ImGui::InvisibleButton("##expand", ImVec2(chevronW, rowH)))
    {
        s_expanded[index] = !expanded;
    }
    const bool chevronHovered = ImGui::IsItemHovered();

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 end(pos.x + width, pos.y + rowH);
    const float r = 10.f * u;
    if (isActive)
    {
        dl->AddRectFilled(pos, end, IM_COL32(214, 176, 104, 30), r);
        dl->AddRect(pos, end, IM_COL32(214, 176, 104, 90), r, ImDrawCornerFlags_All, 1.f);
    }
    else if (rowHovered || chevronHovered || expanded)
    {
        dl->AddRectFilled(pos, end, IM_COL32(255, 255, 255, 14), r);
    }
    const ServerStatus status = LoginServers_Status(server);
    const float textX = pos.x + 34.f * u;
    status_dot(dl, ImVec2(pos.x + 17.f * u, pos.y + rowH / 2), 4.5f * u, status);
    dl->AddText(body, Overlay_px(body), ImVec2(textX, pos.y + 11.f * u), COL_TEXT, LoginServers_DisplayName(server));
    char address[300];
    snprintf(
        address,
        sizeof(address),
        "%s:%d",
        server.Host.empty() ? "no address" : server.Host.c_str(),
        server.Port);
    dl->AddText(small, Overlay_px(small), ImVec2(textX, pos.y + 15.f * u + Overlay_px(body)), COL_MUTED, address);
    if (isActive)
    {
        const char *badge = "IN USE";
        const float bw = small->CalcTextSizeA(Overlay_px(small), FLT_MAX, 0.f, badge).x;
        dl->AddText(small, Overlay_px(small), ImVec2(end.x - chevronW - bw - 4.f * u, pos.y + 11.f * u + 2.f * u), COL_GOLD, badge);
    }
    Overlay_Chevron(
        dl,
        ImVec2(end.x - chevronW / 2, pos.y + rowH / 2),
        4.5f * u,
        expanded ? Chevron::Down : Chevron::Right,
        chevronHovered ? COL_GOLD : COL_MUTED,
        1.6f * u);
    if (rowHovered)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%s", isActive ? status_text(status) : "Use this server");
    }
    else if (chevronHovered)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip(expanded ? "Hide settings" : "Edit settings");
    }

    if (s_expanded[index] != 0)
    {
        ImGui::Indent(14.f * u);
        draw_server_editor(index, server, width - 28.f * u, u);
        ImGui::Unindent(14.f * u);
    }
    ImGui::PopID();
}

static void draw_server_panel(const ImVec2 &display, float panelW, float ease, float u)
{
    ImFont *body = Overlay_LoginFont(LoginFont::Body);
    ImFont *small = Overlay_LoginFont(LoginFont::Small);
    ImFont *title = Overlay_LoginFont(LoginFont::Title);
    auto &servers = LoginServers_List();
    s_expanded.resize(servers.size(), 0);

    const float x = -panelW * (1.f - ease);
    ImGui::SetNextWindowPos(ImVec2(x, 0));
    ImGui::SetNextWindowSize(ImVec2(panelW, display.y));
    ImGui::SetNextWindowBgAlpha(0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin(
        "##servers",
        nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar(2);
    ImGui::SetWindowFontScale(u / 2.f);

    // glass sheet over the left side, with a soft shadow on its right edge
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p0(x, 0);
    const ImVec2 p1(x + panelW, display.y);
    dl->AddRectFilled(p0, p1, IM_COL32(14, 16, 22, 250));
    dl->AddRectFilledMultiColor(
        ImVec2(p0.x, 0), ImVec2(p1.x, 120.f * u),
        IM_COL32(255, 255, 255, 10), IM_COL32(255, 255, 255, 10), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
    dl->AddLine(ImVec2(p1.x - 1, 0), ImVec2(p1.x - 1, display.y), IM_COL32(255, 255, 255, 34));

    const float pad = 26.f * u;
    const float innerW = panelW - pad * 2;

    // header: title and a close chevron
    const float headerY = 24.f * u;
    dl->AddText(title, Overlay_px(title) * 0.5f, ImVec2(x + pad, headerY), COL_TEXT, "Servers");
    const float closeSize = 32.f * u;
    ImGui::SetCursorScreenPos(ImVec2(x + panelW - pad - closeSize + 8.f * u, headerY - 2.f * u));
    if (ImGui::InvisibleButton("##close", ImVec2(closeSize, closeSize)))
    {
        s_panelOpen = false;
    }
    const bool closeHovered = ImGui::IsItemHovered();
    if (closeHovered)
    {
        ImGui::SetTooltip("Close");
    }
    Overlay_Chevron(
        dl,
        ImVec2(x + panelW - pad - closeSize / 2 + 8.f * u, headerY - 2.f * u + closeSize / 2),
        5.f * u,
        Chevron::Left,
        closeHovered ? COL_GOLD : COL_MUTED,
        1.8f * u);
    float y = headerY + Overlay_px(title) * 0.5f + 8.f * u;
    dl->AddText(small, Overlay_px(small), ImVec2(x + pad, y), COL_MUTED, "Click a server to use it, or the arrow to edit it.");
    y += Overlay_px(small) + 18.f * u;

    // restart banner when the server in use needs settings that load at startup
    ImGui::PushFont(body);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f * u);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f * u, 8.f * u));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f * u, 6.f * u));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_TEXT));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::ColorConvertU32ToFloat4(COL_MUTED));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.38f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.f, 0.f, 0.f, 0.48f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.f, 0.f, 0.f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImGui::ColorConvertU32ToFloat4(COL_GOLD));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 1.f, 1.f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 1.f, 1.f, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.f, 1.f, 1.f, 0.20f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.f, 1.f, 1.f, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1.f, 1.f, 1.f, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.84f, 0.69f, 0.41f, 0.35f));
    const int colors = 16;

    if (LoginServers_NeedsRestart())
    {
        const float bannerH = Overlay_px(body) * 2 + 34.f * u;
        const ImVec2 b0(x + pad, y);
        const ImVec2 b1(x + pad + innerW, y + bannerH);
        dl->AddRectFilled(b0, b1, IM_COL32(214, 176, 104, 34), 10.f * u);
        dl->AddRect(b0, b1, IM_COL32(214, 176, 104, 110), 10.f * u, ImDrawCornerFlags_All, 1.f);
        ImGui::SetCursorScreenPos(ImVec2(b0.x + 14.f * u, b0.y + 10.f * u));
        ImGui::BeginGroup();
        ImGui::TextUnformatted("Restart to apply");
        ImGui::PushFont(small);
        ImGui::TextDisabled("Game data, version or encryption changed.");
        ImGui::PopFont();
        ImGui::EndGroup();
        const char *restart = "Restart";
        const float rw = ImGui::CalcTextSize(restart).x + 28.f * u;
        ImGui::SetCursorScreenPos(ImVec2(b1.x - rw - 12.f * u, b0.y + (bannerH - Overlay_px(body) - 16.f * u) / 2));
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(COL_GOLD));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.91f, 0.77f, 0.50f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.74f, 0.59f, 0.33f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.08f, 0.05f, 1.f));
        if (ImGui::Button(restart, ImVec2(rw, 0)) && !busy())
        {
            Overlay_Later(LoginServers_Restart);
        }
        ImGui::PopStyleColor(4);
        y += bannerH + 14.f * u;
    }

    // the list scrolls; the bottom strip keeps the floating add button clear of it
    const float fab = 52.f * u;
    const float footerH = fab + 36.f * u;
    ImGui::SetCursorScreenPos(ImVec2(x + pad, y));
    ImGui::BeginChild("##serverlist", ImVec2(innerW + 12.f * u, display.y - y - footerH), false);
    if (servers.empty())
    {
        ImGui::Dummy(ImVec2(0, 20.f * u));
        ImGui::TextDisabled("No servers yet. Add one with the + button.");
    }
    const int scrollTo = s_focusName;
    for (int i = 0; i < int(servers.size()); i++)
    {
        draw_server_row(i, innerW, u);
        if (i == scrollTo)
        {
            ImGui::SetScrollHereY(1.f);
        }
        ImGui::Dummy(ImVec2(0, 4.f * u));
        if (i >= int(servers.size()))
        {
            break; // removed
        }
    }
    ImGui::EndChild();

    // floating add button
    const ImVec2 fc(x + panelW - pad - fab / 2, display.y - 24.f * u - fab / 2);
    ImGui::SetCursorScreenPos(ImVec2(fc.x - fab / 2, fc.y - fab / 2));
    if (ImGui::InvisibleButton("##add", ImVec2(fab, fab)))
    {
        const int index = LoginServers_Add();
        s_expanded.resize(servers.size(), 0);
        s_expanded[index] = 1;
        s_focusName = index;
    }
    const bool addHovered = ImGui::IsItemHovered();
    if (addHovered)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Add a server");
    }
    dl->AddCircleFilled(ImVec2(fc.x, fc.y + 3.f * u), fab / 2, IM_COL32(0, 0, 0, 90), 32);
    dl->AddCircleFilled(fc, fab / 2, addHovered ? IM_COL32(232, 196, 128, 255) : COL_GOLD, 32);
    const float arm = fab * 0.2f;
    const ImU32 ink = IM_COL32(26, 20, 12, 255);
    dl->AddLine(ImVec2(fc.x - arm, fc.y), ImVec2(fc.x + arm, fc.y), ink, 2.4f * u);
    dl->AddLine(ImVec2(fc.x, fc.y - arm), ImVec2(fc.x, fc.y + arm), ink, 2.4f * u);

    ImGui::PopStyleColor(colors);
    ImGui::PopStyleVar(4);
    ImGui::PopFont();
    ImGui::End();
}

// ---------------------------------------------------------------------------------------
// Shard choice: after signing in, one glass card per shard in place of the login card.

enum class ShardSort
{
    Recommended,
    Name,
    Fullness,
};
static ShardSort s_shardSort = ShardSort::Recommended;
static int s_shardSelected = -1; // position in g_ServerList
static double s_shardLastClick = 0;
static int s_shardLastClickIndex = -1;

static const char *fullness_word(int percent)
{
    if (percent >= 95)
        return "Full";
    if (percent >= 70)
        return "Busy";
    if (percent >= 35)
        return "Moderate";
    return "Quiet";
}

static int login_latency()
{
    const auto &servers = LoginServers_List();
    const int active = LoginServers_Active();
    return active >= 0 && active < int(servers.size()) ? LoginServers_LatencyMs(servers[active]) : -1;
}

// Signal bars: four bars, more of them lit the quicker the connection.
static void signal_bars(ImDrawList *dl, const ImVec2 &p, float h, int ms, float u)
{
    const int lit = ms < 0 ? 0 : ms < 60 ? 4 : ms < 120 ? 3 : ms < 220 ? 2 : 1;
    const float bw = 3.f * u;
    for (int i = 0; i < 4; i++)
    {
        const float bh = h * (0.35f + 0.65f * (i + 1) / 4.f);
        const ImVec2 a(p.x + i * (bw + 2.f * u), p.y + h - bh);
        dl->AddRectFilled(a, ImVec2(a.x + bw, p.y + h), i < lit ? COL_GOLD : IM_COL32(255, 255, 255, 40), 1.f);
    }
}

static void enter_shard(int position)
{
    if (busy())
    {
        return;
    }
    Overlay_Later([position] {
        g_ServerScreen.SelectionServerTempValue = position;
        g_ServerScreen.CreateSmoothAction(CServerScreen::ID_SMOOTH_SS_SELECT_SERVER);
    });
}

static void draw_shards(const ImVec2 &display, float u)
{
    ImFont *title = Overlay_LoginFont(LoginFont::Title);
    ImFont *body = Overlay_LoginFont(LoginFont::Body);
    ImFont *small = Overlay_LoginFont(LoginFont::Small);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const int count = g_ServerList.ServersCount();
    if (s_shardSelected < 0 || s_shardSelected >= count)
    {
        s_shardSelected = std::max(0, std::min<int>(int(g_ServerList.LastServerIndex), count - 1));
    }

    // order: last played first, then the least full (or by name / fullness)
    std::vector<int> order(count);
    for (int i = 0; i < count; i++)
    {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [](int a, int b) {
        CServer *sa = g_ServerList.GetServer(a);
        CServer *sb = g_ServerList.GetServer(b);
        switch (s_shardSort)
        {
            case ShardSort::Name:
                return sa->Name < sb->Name;
            case ShardSort::Fullness:
                return sa->FullPercent < sb->FullPercent;
            default:
                if ((a == int(g_ServerList.LastServerIndex)) != (b == int(g_ServerList.LastServerIndex)))
                {
                    return a == int(g_ServerList.LastServerIndex);
                }
                return sa->FullPercent < sb->FullPercent;
        }
    });

    // heading, under the title
    const float top = std::max(display.y * 0.42f, 190.f * u);
    const char *heading = "Choose your shard";
    const float hs = Overlay_px(title) * 0.5f;
    const float hw = title->CalcTextSizeA(hs, FLT_MAX, 0.f, heading).x;
    dl->AddText(title, hs, ImVec2(floorf((display.x - hw) / 2), top), COL_TEXT, heading);
    const std::string who = "Signed in as " + g_MainScreen.m_Account->GetTextA();
    Overlay_TextCentered(dl, small, top + hs + 8.f * u, 0, display.x, COL_MUTED, who.c_str());

    // sort chips
    static const char *sorts[3] = { "Recommended", "Name", "Fullness" };
    float chipsW = 0;
    for (auto label : sorts)
    {
        chipsW += small->CalcTextSizeA(Overlay_px(small), FLT_MAX, 0.f, label).x + 28.f * u;
    }
    chipsW += 16.f * u;
    float cx = floorf((display.x - chipsW) / 2);
    const float chipsY = top + hs + Overlay_px(small) + 22.f * u;
    const float chipH = Overlay_px(small) + 12.f * u;
    for (int i = 0; i < 3; i++)
    {
        const float w = small->CalcTextSizeA(Overlay_px(small), FLT_MAX, 0.f, sorts[i]).x + 20.f * u;
        ImGui::SetCursorScreenPos(ImVec2(cx, chipsY));
        ImGui::PushID(500 + i);
        if (ImGui::InvisibleButton("##sort", ImVec2(w, chipH)))
        {
            s_shardSort = ShardSort(i);
        }
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool on = int(s_shardSort) == i;
        dl->AddRectFilled(ImVec2(cx, chipsY), ImVec2(cx + w, chipsY + chipH),
                          on ? IM_COL32(214, 176, 104, 60) : (hov ? IM_COL32(255, 255, 255, 24) : IM_COL32(0, 0, 0, 80)), chipH / 2);
        dl->AddRect(ImVec2(cx, chipsY), ImVec2(cx + w, chipsY + chipH), on ? IM_COL32(214, 176, 104, 160) : IM_COL32(255, 255, 255, 30), chipH / 2);
        dl->AddText(small, Overlay_px(small), ImVec2(cx + 10.f * u, chipsY + 6.f * u), on ? COL_GOLD : COL_TEXT, sorts[i]);
        cx += w + 8.f * u;
    }

    // the cards, centred, wrapping into rows
    const float cardW = 250.f * u;
    const float cardH = 150.f * u;
    const float gap = 16.f * u;
    const float footerH = Overlay_px(body) + 22.f * u;
    const float areaTop = chipsY + chipH + 22.f * u;
    const float areaBottom = display.y - 90.f * u - footerH;
    const int perRow = std::max(1, std::min(count, int((display.x - 80.f * u + gap) / (cardW + gap))));
    const float rowW = perRow * cardW + (perRow - 1) * gap;
    const float areaX = floorf((display.x - rowW) / 2);
    ImGui::SetCursorScreenPos(ImVec2(areaX, areaTop));
    ImGui::BeginChild("##shards", ImVec2(rowW + 14.f * u, std::max(cardH + 8.f * u, areaBottom - areaTop)), false);
    const int latency = login_latency();
    for (int k = 0; k < count; k++)
    {
        const int i = order[k];
        CServer *shard = g_ServerList.GetServer(i);
        if (shard == nullptr)
        {
            continue;
        }
        if (k % perRow != 0)
        {
            ImGui::SameLine(0, gap);
        }
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##shard", ImVec2(cardW, cardH)))
        {
            const double now = ImGui::GetTime();
            if (s_shardLastClickIndex == i && now - s_shardLastClick < 0.4)
            {
                enter_shard(i); // double click
            }
            s_shardSelected = i;
            s_shardLastClick = now;
            s_shardLastClickIndex = i;
        }
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        ImDrawList *cl = ImGui::GetWindowDrawList();
        const ImVec2 e(p.x + cardW, p.y + cardH);
        const bool sel = i == s_shardSelected;
        Overlay_GlassPanel(cl, p, e, 12.f * u, u);
        if (sel)
        {
            cl->AddRectFilled(p, e, IM_COL32(214, 176, 104, 28), 12.f * u);
            cl->AddRect(p, e, IM_COL32(214, 176, 104, 200), 12.f * u, ImDrawCornerFlags_All, 1.5f);
        }
        else if (hov)
        {
            cl->AddRect(p, e, IM_COL32(255, 255, 255, 90), 12.f * u, ImDrawCornerFlags_All, 1.f);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        const float pad = 16.f * u;
        float y = p.y + pad;
        if (i == int(g_ServerList.LastServerIndex) && !g_ServerList.LastServerName.empty())
        {
            const char *badge = "LAST PLAYED";
            const float bw = small->CalcTextSizeA(Overlay_px(small) * 0.85f, FLT_MAX, 0.f, badge).x + 12.f * u;
            const ImVec2 b0(e.x - pad - bw, y);
            cl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + Overlay_px(small) + 4.f * u), IM_COL32(214, 176, 104, 220), 4.f * u);
            cl->AddText(small, Overlay_px(small) * 0.85f, ImVec2(b0.x + 6.f * u, b0.y + 3.f * u), IM_COL32(26, 20, 12, 255), badge);
        }
        cl->AddText(title, Overlay_px(title) * 0.42f, ImVec2(p.x + pad, y + 18.f * u), sel ? COL_GOLD : COL_TEXT, shard->Name.c_str());
        y += 18.f * u + Overlay_px(title) * 0.42f + 14.f * u;

        // fullness
        char full[48];
        snprintf(full, sizeof(full), "%s  \xC2\xB7  %d%% full", fullness_word(shard->FullPercent), int(shard->FullPercent));
        cl->AddText(small, Overlay_px(small), ImVec2(p.x + pad, y), COL_MUTED, full);
        y += Overlay_px(small) + 6.f * u;
        const float barW = cardW - pad * 2;
        cl->AddRectFilled(ImVec2(p.x + pad, y), ImVec2(p.x + pad + barW, y + 4.f * u), IM_COL32(255, 255, 255, 30), 2.f);
        cl->AddRectFilled(ImVec2(p.x + pad, y), ImVec2(p.x + pad + barW * std::min(100, int(shard->FullPercent)) / 100.f, y + 4.f * u),
                          shard->FullPercent >= 95 ? IM_COL32(248, 113, 113, 255) : COL_GOLD, 2.f);
        y += 4.f * u + 14.f * u;

        // latency (shard ping when the client has it, else the login server's) and time zone
        const int ms = shard->Ping >= 0 ? shard->Ping : latency;
        signal_bars(cl, ImVec2(p.x + pad, y), Overlay_px(small), ms, u);
        char info[64];
        if (ms >= 0)
            snprintf(info, sizeof(info), "%d ms", ms);
        else
            snprintf(info, sizeof(info), "measuring...");
        cl->AddText(small, Overlay_px(small), ImVec2(p.x + pad + 24.f * u, y), COL_MUTED, info);
        const int tz = int(int8_t(shard->Timezone));
        if (tz != 0)
        {
            char zone[24];
            snprintf(zone, sizeof(zone), "GMT%+d", tz);
            const float zw = small->CalcTextSizeA(Overlay_px(small), FLT_MAX, 0.f, zone).x;
            cl->AddText(small, Overlay_px(small), ImVec2(e.x - pad - zw, y), COL_MUTED, zone);
        }
        if (hov)
        {
            ImGui::SetTooltip("Double-click to enter %s", shard->Name.c_str());
        }
    }
    ImGui::EndChild();

    // keyboard: arrows move through the cards in display order, Enter plays, Esc goes back
    const ImGuiIO &io = ImGui::GetIO();
    const int pos = int(std::find(order.begin(), order.end(), s_shardSelected) - order.begin());
    if (count > 0 && (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_RightArrow]) || ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_DownArrow])))
    {
        s_shardSelected = order[std::min(count - 1, pos + 1)];
    }
    if (count > 0 && (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_LeftArrow]) || ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_UpArrow])))
    {
        s_shardSelected = order[std::max(0, pos - 1)];
    }

    // footer: back and enter
    const float by = display.y - 90.f * u - footerH + 20.f * u;
    const float backW = 140.f * u;
    CServer *chosen = g_ServerList.GetServer(s_shardSelected);
    const std::string go = chosen != nullptr ? "Enter " + chosen->Name : std::string("Enter");
    const float goW = std::max(220.f * u, body->CalcTextSizeA(Overlay_px(body), FLT_MAX, 0.f, go.c_str()).x + 60.f * u);
    const float total = backW + 12.f * u + goW;
    ImGui::PushFont(body);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f * u);
    ImGui::SetCursorScreenPos(ImVec2(floorf((display.x - total) / 2), by));
    if (Overlay_CardButton("Back", ImVec2(backW, footerH), false) || ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Escape], false))
    {
        Overlay_Later([] { g_ServerScreen.CreateSmoothAction(CServerScreen::ID_SMOOTH_SS_GO_SCREEN_MAIN); });
    }
    ImGui::SameLine(0, 12.f * u);
    if ((Overlay_CardButton(go.c_str(), ImVec2(goW, footerH), true) || ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Enter], false)) &&
        chosen != nullptr)
    {
        enter_shard(s_shardSelected);
    }
    ImGui::PopStyleVar();
    ImGui::PopFont();
}

// ---------------------------------------------------------------------------------------
// Character choice: one card per character (and "New character" for empty slots). The server
// only sends names; class, time played and appearance come from EmmexUO's own roster.

static int s_charSelected = -1; // slot in g_CharacterList
static double s_charLastClick = 0;
static int s_charLastClickSlot = -1;
static int s_charDeleting = -1; // slot being confirmed for deletion
static std::string s_charDeleteTyped;
static std::string s_charPendingDelete; // asked the server; forget it locally once it's gone
static std::map<std::string, OverlayImage> s_charDolls; // by name + layers
static std::map<std::string, OverlayImage> s_charPortraits;

static const OverlayImage &class_portrait(const std::string &key)
{
    const std::string k = key.empty() ? "advanced" : key;
    auto it = s_charPortraits.find(k);
    if (it == s_charPortraits.end())
    {
        OverlayImage image = Overlay_LoadImage(("create/classes/" + k + ".jpg").c_str());
        if (image.texture == nullptr)
        {
            image = Overlay_LoadImage("create/classes/advanced.jpg");
        }
        it = s_charPortraits.emplace(k, image).first;
    }
    return it->second;
}

static const OverlayImage *character_doll(const RosterCharacter &c)
{
    if (c.Layers.empty())
    {
        return nullptr;
    }
    std::string key = c.Name;
    for (const auto &l : c.Layers)
    {
        key += "," + std::to_string(l.Gump) + ":" + std::to_string(l.Hue) + (l.PartialHue ? "p" : "");
    }
    auto it = s_charDolls.find(key);
    if (it == s_charDolls.end())
    {
        std::vector<GumpLayer> layers;
        for (const auto &l : c.Layers)
        {
            layers.push_back({ l.Gump, l.Hue, l.PartialHue });
        }
        it = s_charDolls.emplace(key, Overlay_ComposeGumps(layers, true)).first;
    }
    return it->second.texture != nullptr ? &it->second : nullptr;
}

static std::string played_text(const RosterCharacter &c)
{
    char buf[96];
    const long long h = c.PlayedSeconds / 3600;
    const long long m = (c.PlayedSeconds / 60) % 60;
    std::string played = h > 0 ? std::to_string(h) + "h " + std::to_string(m) + "m" : std::to_string(m) + "m";
    const long long ago = c.LastPlayed > 0 ? (long long)(time(nullptr) - c.LastPlayed) : -1;
    std::string when;
    if (ago < 0)
        when = "";
    else if (ago < 3600)
        when = "just now";
    else if (ago < 86400)
        when = std::to_string(ago / 3600) + "h ago";
    else if (ago < 86400 * 2)
        when = "yesterday";
    else
        when = std::to_string(ago / 86400) + " days ago";
    snprintf(buf, sizeof(buf), "Played %s%s%s", played.c_str(), when.empty() ? "" : "  \xC2\xB7  ", when.c_str());
    return buf;
}

static void play_character(int slot)
{
    if (busy())
    {
        return;
    }
    g_CharacterList.Selected = slot;
    Overlay_Later([] { g_CharacterListScreen.CreateSmoothAction(CCharacterListScreen::SelectCharacter); });
}

static void draw_character_card(int slot, const ImVec2 &p, float cardW, float cardH, float u)
{
    ImFont *title = Overlay_LoginFont(LoginFont::Title);
    ImFont *body = Overlay_LoginFont(LoginFont::Body);
    ImFont *small = Overlay_LoginFont(LoginFont::Small);
    ImDrawList *cl = ImGui::GetWindowDrawList();
    const std::string name = g_CharacterList.GetName(slot);
    const bool sel = slot == s_charSelected;
    const ImVec2 e(p.x + cardW, p.y + cardH);
    const float pad = 16.f * u;

    ImGui::SetCursorScreenPos(p);
    ImGui::PushID(slot);
    const bool deleting = s_charDeleting == slot;
    if (!deleting && ImGui::InvisibleButton("##char", ImVec2(cardW, cardH)))
    {
        const double now = ImGui::GetTime();
        if (s_charLastClickSlot == slot && now - s_charLastClick < 0.4)
        {
            play_character(slot);
        }
        s_charSelected = slot;
        s_charLastClick = now;
        s_charLastClickSlot = slot;
    }
    const bool hov = !deleting && ImGui::IsItemHovered();
    Overlay_GlassPanel(cl, p, e, 12.f * u, u);
    if (sel)
    {
        cl->AddRectFilled(p, e, IM_COL32(214, 176, 104, 24), 12.f * u);
        cl->AddRect(p, e, IM_COL32(214, 176, 104, 200), 12.f * u, ImDrawCornerFlags_All, 1.5f);
    }
    else if (hov)
    {
        cl->AddRect(p, e, IM_COL32(255, 255, 255, 90), 12.f * u, ImDrawCornerFlags_All, 1.f);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    const RosterCharacter *record = Roster_Find(name);
    // the character on a pool of light, with their class portrait in the corner
    const float stageH = cardH * 0.5f;
    const ImVec2 floorC(p.x + cardW / 2, p.y + stageH);
    for (int i = 8; i > 0; i--)
    {
        cl->AddCircleFilled(ImVec2(floorC.x, floorC.y - stageH * 0.35f), cardW * 0.045f * i, IM_COL32(255, 214, 150, 5), 40);
    }
    const OverlayImage *doll = record != nullptr ? character_doll(*record) : nullptr;
    if (doll != nullptr)
    {
        const float scale = std::min((stageH - pad) / doll->height, (cardW * 0.6f) / doll->width);
        const float w = doll->width * scale;
        const float h = doll->height * scale;
        cl->AddImage(doll->texture, ImVec2(floorC.x - w / 2, floorC.y - h), ImVec2(floorC.x + w / 2, floorC.y));
    }
    const OverlayImage &portrait = class_portrait(record != nullptr ? record->Profession : std::string());
    const float pr = doll != nullptr ? 44.f * u : stageH - pad * 2;
    const ImVec2 pp = doll != nullptr ? ImVec2(p.x + pad, p.y + pad) : ImVec2(floorC.x - pr / 2, p.y + pad);
    if (portrait.texture != nullptr)
    {
        cl->AddImageRounded(portrait.texture, pp, ImVec2(pp.x + pr, pp.y + pr), ImVec2(0, 0), ImVec2(1, 1),
                            record != nullptr ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 120), 10.f * u);
        cl->AddRect(pp, ImVec2(pp.x + pr, pp.y + pr), IM_COL32(255, 255, 255, 40), 10.f * u);
    }

    // name, class, skills, time
    float y = p.y + stageH + 10.f * u;
    const float ts = Overlay_px(title) * 0.46f;
    const float nw = title->CalcTextSizeA(ts, FLT_MAX, 0.f, name.c_str()).x;
    cl->AddText(title, ts, ImVec2(floorf(p.x + (cardW - nw) / 2), y), sel ? COL_GOLD : COL_TEXT, name.c_str());
    y += ts + 8.f * u;
    if (deleting)
    {
        // confirm: type the name
        const std::string prompt = "Type " + name + " to delete";
        Overlay_TextCentered(cl, small, y, p.x, cardW, IM_COL32(248, 113, 113, 255), prompt.c_str());
        y += Overlay_px(small) + 8.f * u;
        ImGui::SetCursorScreenPos(ImVec2(p.x + pad, y));
        ImGui::PushItemWidth(cardW - pad * 2);
        ImGui::PushFont(body);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f * u);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f * u, 8.f * u));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_TEXT));
        if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive())
        {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputTextWithHint("##confirm", name.c_str(), &s_charDeleteTyped);
        const float bh = Overlay_px(body) + 16.f * u;
        const float half = (cardW - pad * 2 - 8.f * u) / 2;
        ImGui::SetCursorScreenPos(ImVec2(p.x + pad, e.y - pad - bh));
        if (Overlay_CardButton("Cancel", ImVec2(half, bh), false) ||
            ImGui::IsKeyPressed(ImGui::GetIO().KeyMap[ImGuiKey_Escape], false))
        {
            s_charDeleting = -1;
        }
        ImGui::SameLine(0, 8.f * u);
        const bool match = s_charDeleteTyped == name;
        ImGui::PushStyleColor(ImGuiCol_Button, match ? ImVec4(0.86f, 0.30f, 0.30f, 1.f) : ImVec4(0.5f, 0.2f, 0.2f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, match ? ImVec4(0.93f, 0.38f, 0.38f, 1.f) : ImVec4(0.5f, 0.2f, 0.2f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.24f, 0.24f, 1.f));
        if (ImGui::Button("Delete", ImVec2(half, bh)) && match)
        {
            s_charDeleting = -1;
            s_charPendingDelete = name;
            g_CharacterList.Selected = slot;
            Overlay_Later([slot] { CPacketDeleteCharacter(slot).Send(); });
        }
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(2);
        ImGui::PopFont();
        ImGui::PopItemWidth();
        ImGui::PopID();
        return;
    }
    if (record != nullptr && !record->TopSkills.empty())
    {
        const std::string cls = Roster_ProfessionTitle(record->Profession);
        Overlay_TextCentered(cl, body, y, p.x, cardW, COL_GOLD, cls.c_str());
        y += Overlay_px(body) + 6.f * u;
        std::string skills;
        for (size_t i = 0; i < record->TopSkills.size(); i++)
        {
            char one[64];
            snprintf(one, sizeof(one), "%s%s %.0f", i ? "  \xC2\xB7  " : "", record->TopSkills[i].Name.c_str(), record->TopSkills[i].Value);
            skills += one;
        }
        Overlay_TextWrappedCentered(cl, small, p.x + pad, cardW - pad * 2, y, COL_MUTED, skills.c_str());
    }
    const std::string footer = record != nullptr ? played_text(*record) : std::string("Not played on this Mac yet");
    Overlay_TextCentered(cl, small, e.y - pad - Overlay_px(small), p.x, cardW, COL_MUTED, footer.c_str());

    // delete, on the selected card
    if (sel)
    {
        const char *del = "Delete";
        const float dw = small->CalcTextSizeA(Overlay_px(small), FLT_MAX, 0.f, del).x;
        const ImVec2 dp(e.x - pad - dw, p.y + pad);
        ImGui::SetCursorScreenPos(dp);
        if (ImGui::InvisibleButton("##del", ImVec2(dw, Overlay_px(small))))
        {
            s_charDeleting = slot;
            s_charDeleteTyped.clear();
        }
        const bool dh = ImGui::IsItemHovered();
        cl->AddText(small, Overlay_px(small), dp, dh ? IM_COL32(248, 113, 113, 255) : IM_COL32(248, 113, 113, 150), del);
        if (dh)
        {
            ImGui::SetTooltip("Permanently delete %s", name.c_str());
        }
    }
    ImGui::PopID();
}

static void draw_new_card(const ImVec2 &p, float cardW, float cardH, float u)
{
    ImFont *body = Overlay_LoginFont(LoginFont::Body);
    ImDrawList *cl = ImGui::GetWindowDrawList();
    const ImVec2 e(p.x + cardW, p.y + cardH);
    ImGui::SetCursorScreenPos(p);
    const bool pressed = ImGui::InvisibleButton("##newchar", ImVec2(cardW, cardH));
    const bool hov = ImGui::IsItemHovered();
    cl->AddRectFilled(p, e, hov ? IM_COL32(18, 20, 28, 150) : IM_COL32(18, 20, 28, 100), 12.f * u);
    // dashed outline
    const ImU32 col = hov ? COL_GOLD : IM_COL32(255, 255, 255, 70);
    const float dash = 8.f * u;
    for (float x = p.x + 12.f * u; x < e.x - 12.f * u; x += dash * 2)
    {
        cl->AddLine(ImVec2(x, p.y), ImVec2(std::min(x + dash, e.x - 12.f * u), p.y), col);
        cl->AddLine(ImVec2(x, e.y), ImVec2(std::min(x + dash, e.x - 12.f * u), e.y), col);
    }
    for (float y = p.y + 12.f * u; y < e.y - 12.f * u; y += dash * 2)
    {
        cl->AddLine(ImVec2(p.x, y), ImVec2(p.x, std::min(y + dash, e.y - 12.f * u)), col);
        cl->AddLine(ImVec2(e.x, y), ImVec2(e.x, std::min(y + dash, e.y - 12.f * u)), col);
    }
    const ImVec2 c(p.x + cardW / 2, p.y + cardH * 0.42f);
    const float r = 28.f * u;
    cl->AddCircleFilled(c, r, hov ? IM_COL32(232, 196, 128, 255) : COL_GOLD, 40);
    const ImU32 ink = IM_COL32(26, 20, 12, 255);
    cl->AddLine(ImVec2(c.x - r * 0.4f, c.y), ImVec2(c.x + r * 0.4f, c.y), ink, 2.6f * u);
    cl->AddLine(ImVec2(c.x, c.y - r * 0.4f), ImVec2(c.x, c.y + r * 0.4f), ink, 2.6f * u);
    Overlay_TextCentered(cl, body, c.y + r + 16.f * u, p.x, cardW, hov ? COL_GOLD : COL_TEXT, "New character");
    if (hov)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (pressed && !busy())
    {
        Overlay_Later([] { g_CharacterListScreen.CreateSmoothAction(CCharacterListScreen::GotoScreenProfession); });
    }
}

static void draw_characters(const ImVec2 &display, float u)
{
    ImFont *title = Overlay_LoginFont(LoginFont::Title);
    ImFont *body = Overlay_LoginFont(LoginFont::Body);
    ImFont *small = Overlay_LoginFont(LoginFont::Small);
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // slots with characters, then one "New character" card if there's room
    std::vector<int> slots;
    bool freeSlot = false;
    for (int i = 0; i < g_CharacterList.Count; i++)
    {
        if (!g_CharacterList.GetName(i).empty())
        {
            slots.push_back(i);
        }
        else
        {
            freeSlot = true;
        }
    }
    // a deletion the server accepted: the name is gone from the list
    if (!s_charPendingDelete.empty())
    {
        bool still = false;
        for (int i : slots)
        {
            still |= g_CharacterList.GetName(i) == s_charPendingDelete;
        }
        if (!still)
        {
            Roster_Forget(s_charPendingDelete);
            s_charPendingDelete.clear();
        }
    }
    const bool canCreate = freeSlot && !g_CharacterList.SingleSlot;
    if (std::find(slots.begin(), slots.end(), s_charSelected) == slots.end())
    {
        s_charSelected = slots.empty() ? -1 : slots[0];
        for (int i : slots)
        {
            if (g_CharacterList.GetName(i) == g_CharacterList.LastCharacterName)
            {
                s_charSelected = i;
            }
        }
    }

    // heading
    const float top = std::max(display.y * 0.14f, 80.f * u);
    const char *heading = "Choose your character";
    const float hs = Overlay_px(title) * 0.5f;
    const float hw = title->CalcTextSizeA(hs, FLT_MAX, 0.f, heading).x;
    dl->AddText(title, hs, ImVec2(floorf((display.x - hw) / 2), top), COL_TEXT, heading);
    std::string where = g_ServerList.LastServerName;
    const auto &servers = LoginServers_List();
    const int active = LoginServers_Active();
    if (active >= 0 && active < int(servers.size()))
    {
        const std::string server = LoginServers_DisplayName(servers[active]);
        where = where.empty() || where == server ? server : server + "  \xC2\xB7  " + where;
    }
    Overlay_TextCentered(dl, small, top + hs + 8.f * u, 0, display.x, COL_MUTED, where.c_str());

    // cards
    const float cardW = 260.f * u;
    const float cardH = 330.f * u;
    const float gap = 16.f * u;
    const int count = int(slots.size()) + (canCreate ? 1 : 0);
    const float footerH = Overlay_px(body) + 22.f * u;
    const float areaTop = top + hs + Overlay_px(small) + 28.f * u;
    const float areaBottom = display.y - 70.f * u - footerH;
    const int perRow = std::max(1, std::min(count, int((display.x - 80.f * u + gap) / (cardW + gap))));
    const float rowW = perRow * cardW + (perRow - 1) * gap;
    const float areaX = floorf((display.x - rowW) / 2);
    ImGui::SetCursorScreenPos(ImVec2(areaX, areaTop));
    ImGui::BeginChild("##characters", ImVec2(rowW + 14.f * u, std::max(cardH + 8.f * u, areaBottom - areaTop)), false);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    for (int k = 0; k < count; k++)
    {
        const ImVec2 p(origin.x + (k % perRow) * (cardW + gap), origin.y + (k / perRow) * (cardH + gap));
        if (k < int(slots.size()))
        {
            draw_character_card(slots[k], p, cardW, cardH, u);
        }
        else
        {
            draw_new_card(p, cardW, cardH, u);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + ((count + perRow - 1) / perRow) * (cardH + gap)));
    ImGui::Dummy(ImVec2(rowW, 0));
    ImGui::EndChild();

    // keyboard (not while typing a confirmation)
    const ImGuiIO &io = ImGui::GetIO();
    if (s_charDeleting < 0 && !slots.empty())
    {
        const int pos = int(std::find(slots.begin(), slots.end(), s_charSelected) - slots.begin());
        if (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_RightArrow]) || ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_DownArrow]))
        {
            s_charSelected = slots[std::min(int(slots.size()) - 1, pos + 1)];
        }
        if (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_LeftArrow]) || ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_UpArrow]))
        {
            s_charSelected = slots[std::max(0, pos - 1)];
        }
    }

    // footer: back and play
    const float by = display.y - 70.f * u - footerH + 20.f * u;
    const float backW = 140.f * u;
    const std::string go = s_charSelected >= 0 ? "Play " + g_CharacterList.GetName(s_charSelected) : std::string("Play");
    const float goW = std::max(220.f * u, body->CalcTextSizeA(Overlay_px(body), FLT_MAX, 0.f, go.c_str()).x + 60.f * u);
    const float total = backW + 12.f * u + goW;
    ImGui::PushFont(body);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f * u);
    ImGui::SetCursorScreenPos(ImVec2(floorf((display.x - total) / 2), by));
    const bool typing = s_charDeleting >= 0;
    if (Overlay_CardButton("Back", ImVec2(backW, footerH), false) ||
        (!typing && ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Escape], false)))
    {
        // back to the shards: the client signs in again to list them
        Overlay_Later([] { g_CharacterListScreen.CreateSmoothAction(CCharacterListScreen::Connect); });
    }
    ImGui::SameLine(0, 12.f * u);
    if ((Overlay_CardButton(go.c_str(), ImVec2(goW, footerH), true) ||
         (!typing && ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Enter], false))) &&
        s_charSelected >= 0)
    {
        play_character(s_charSelected);
    }
    ImGui::PopStyleVar();
    ImGui::PopFont();
}

void Overlay_DrawLogin()
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    // layout unit, independent of the game's UI scale: 1 at 1280x800 points
    const float u = std::max(0.75f, std::min(2.f, std::min(display.x / 1280.f, display.y / 800.f)));
    Overlay_SetTextScale(u);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin(
        "##login",
        nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar(2);
    ImGui::SetWindowFontScale(u / 2.f);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    draw_backdrop(dl, display);

    // the server panel slides in from the left; the title and card move into the space left
    const float dt = ImGui::GetIO().DeltaTime;
    s_panelT = std::max(0.f, std::min(1.f, s_panelT + (s_panelOpen ? dt : -dt) * 6.f));
    const float ease = 1.f - (1.f - s_panelT) * (1.f - s_panelT) * (1.f - s_panelT);
    const float panelW = panel_width(display, u);
    const float x0 = panelW * ease;
    const float contentW = display.x - x0;

    // title (the character step needs the room for its taller cards)
    ImFont *title = Overlay_LoginFont(LoginFont::Title);
    ImFont *small = Overlay_LoginFont(LoginFont::Small);
    const bool showTitle = g_GameState != GS_CHARACTER;
    const float titleY = std::max(display.y * 0.42f, 190.f * u) - Overlay_px(title) - 58.f * u;
    if (showTitle)
    {
    Overlay_TextCentered(dl, title, titleY, x0, contentW, COL_TEXT, "ULTIMA ONLINE");
    // tagline between two short gold rules
    const char *tagline = "BRITANNIA AWAITS";
    const float ruleY = titleY + Overlay_px(title) + 16.f * u;
    const float gap = small->CalcTextSizeA(Overlay_px(small), FLT_MAX, 0.f, tagline).x / 2 + 16.f * u;
    const float ruleW = 64.f * u;
    const float mid = x0 + contentW / 2;
    const ImU32 ruleCol = IM_COL32(214, 176, 104, 140);
    dl->AddLine(ImVec2(mid - gap - ruleW, ruleY), ImVec2(mid - gap, ruleY), ruleCol);
    dl->AddLine(ImVec2(mid + gap, ruleY), ImVec2(mid + gap + ruleW, ruleY), ruleCol);
    Overlay_TextCentered(dl, small, ruleY - Overlay_px(small) / 2 - 1.f, x0, contentW, COL_GOLD, tagline);
    }

    if (g_GameState == GS_SERVER)
    {
        draw_shards(display, u);
    }
    else if (g_GameState == GS_CHARACTER)
    {
        draw_characters(display, u);
    }
    else
    {
        draw_card(display, x0, contentW, u);
    }
    draw_footer(display, u);
    if (g_GameState != GS_MAIN)
    {
        s_panelOpen = false; // no switching servers once connecting or signed in
    }
    else if (s_panelT < 0.5f)
    {
        draw_server_pill(u);
    }

    // clicking outside the open panel, or Escape, closes it
    if (s_panelOpen && ImGui::IsMouseClicked(0) && ImGui::GetIO().MousePos.x > x0)
    {
        s_panelOpen = false;
    }

    ImGui::End();

    if (s_panelT > 0.f)
    {
        draw_server_panel(display, panelW, ease, u);
    }
    if (s_panelOpen && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGui::GetIO().KeyMap[ImGuiKey_Escape], false))
    {
        s_panelOpen = false;
    }
    s_wasShown = true;
}

void Overlay_LoginHidden()
{
    // the next time the screen opens, focus goes to the right field again
    s_wasShown = false;
    s_accountActive = false;
    s_passwordActive = false;
}

#endif
