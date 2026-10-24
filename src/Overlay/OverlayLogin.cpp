// SPDX-License-Identifier: AGPL-3.0-or-later

// The login screen: a full-window scene drawn with the overlay instead of the classic
// treasure-chest gump. The gump still exists and owns the account/password/checkbox state;
// this screen reads and writes that same state, so Load/Save, auto login and launcher
// credentials keep working unchanged.

#include "Overlay.h"

#if defined(NEW_RENDERER_ENABLED) && defined(USE_GL3)

#include "OverlayPanels.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <external/gfx/imgui/imgui.h>
#include <external/gfx/imgui/imgui_stdlib.h>
#include <external/tinyfiledialogs.h>
#include "../Application.h"
#include "../Config.h"
#include "../LoginServers.h"
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

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include <external/stb_image.h>
#pragma clang diagnostic pop

static const char *URL_ACCOUNT = "https://accounts.eamythic.com/";
static const char *URL_UOCOM = "https://uo.com";
static const char *URL_HELP = "https://github.com/ChristopherBrand/EmmexUO";
static const char *URL_CREDITS = "https://github.com/ChristopherBrand/EmmexUO/graphs/contributors";

// the classic screen accepted at most 16 characters per field
static const int FIELD_MAX = 16;

// backdrop layers: a looping cloud strip drifting behind the castle and mountains
struct LoginImage
{
    void *texture = nullptr;
    int width = 0;
    int height = 0;
};
static LoginImage s_clouds;
static LoginImage s_foreground;
static bool s_backdropTried = false;

static char s_account[FIELD_MAX + 1];
static char s_password[FIELD_MAX + 1];
static bool s_accountActive = false;
static bool s_passwordActive = false;
static bool s_wasShown = false;

// login fonts are rasterized at 2x (see Overlay.cpp); this maps them to the current layout
static float s_textScale = 0.5f;

static float px(const ImFont *font)
{
    return font->FontSize * s_textScale;
}

static const ImU32 COL_TEXT = IM_COL32(236, 230, 218, 255);
static const ImU32 COL_MUTED = IM_COL32(236, 230, 218, 150);
static const ImU32 COL_GOLD = IM_COL32(214, 176, 104, 255);

std::string Overlay_ResourcePath(const char *name)
{
    // next to the client in a build tree, or in Contents/Resources inside EmmexUO.app
    // (the executable's own folder: apps opened from Finder start in '/')
    const char *dirs[] = { "resources", "../Resources" };
    for (const auto dir : dirs)
    {
        const fs_path path = fs_path_join(fs_path_process(), dir, name);
        if (fs_path_exists(path))
        {
            return fs_path_ascii(path);
        }
    }
    return {};
}

// Loads an image, optionally taking its alpha channel from a separate greyscale file
// (keeps the painted foreground a small JPEG instead of a large RGBA PNG).
static LoginImage load_image(const char *name, const char *alphaName = nullptr)
{
    LoginImage image;
    const std::string path = Overlay_ResourcePath(name);
    int w = 0, h = 0, n = 0;
    uint8_t *pixels = path.empty() ? nullptr : stbi_load(path.c_str(), &w, &h, &n, 4);
    if (pixels == nullptr)
    {
        return image;
    }
    if (alphaName != nullptr)
    {
        const std::string alphaPath = Overlay_ResourcePath(alphaName);
        int aw = 0, ah = 0, an = 0;
        uint8_t *alpha = alphaPath.empty() ? nullptr : stbi_load(alphaPath.c_str(), &aw, &ah, &an, 1);
        if (alpha != nullptr && aw == w && ah == h)
        {
            for (int i = 0; i < w * h; i++)
            {
                pixels[i * 4 + 3] = alpha[i];
            }
        }
        if (alpha != nullptr)
        {
            stbi_image_free(alpha);
        }
    }
    image.texture = Render_OverlayCreateImage(w, h, pixels);
    image.width = w;
    image.height = h;
    stbi_image_free(pixels);
    return image;
}

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
        s_clouds = load_image("login/clouds.jpg");
        s_foreground = load_image("login/foreground.jpg", "login/foreground_alpha.png");
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

static void text_centered(
    ImDrawList *dl, ImFont *font, float y, float x0, float width, ImU32 col, const char *text)
{
    const ImVec2 ts = font->CalcTextSizeA(px(font), FLT_MAX, 0.f, text);
    const ImVec2 pos(floorf(x0 + (width - ts.x) / 2), y);
    dl->AddText(font, px(font), ImVec2(pos.x + 1, pos.y + 2), IM_COL32(0, 0, 0, 160), text);
    dl->AddText(font, px(font), pos, col, text);
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

// Screen changes requested from the login scene run from the main loop, between frames:
// connecting renders a frame of its own, which can't happen inside an overlay frame.
static std::function<void()> s_pending;

static void later(std::function<void()> action)
{
    s_pending = std::move(action);
}

void Overlay_LoginRunPending()
{
    if (s_pending)
    {
        auto action = std::move(s_pending);
        s_pending = nullptr;
        action();
    }
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
        later(LoginServers_Restart);
        return;
    }
    LoginServers_Save(); // remembers this server's account
    later([] { g_MainScreen.CreateSmoothAction(CMainScreen::ID_SMOOTH_MS_CONNECT); });
}

// Text link: muted until hovered, then gold and underlined.
static bool link(const char *label, const char *tooltip)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::CalcTextSize(label);
    const bool pressed = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddText(pos, hovered ? COL_GOLD : COL_MUTED, label);
    if (hovered)
    {
        dl->AddLine(
            ImVec2(pos.x, pos.y + size.y), ImVec2(pos.x + size.x, pos.y + size.y), COL_GOLD);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (tooltip != nullptr)
        {
            ImGui::SetTooltip("%s", tooltip);
        }
    }
    return pressed;
}

// Draws text wrapped to a width, each line centred; returns the height used.
static float text_wrapped_centered(ImDrawList *dl, ImFont *font, float x0, float width, float y, ImU32 col, const char *text)
{
    const float size = px(font);
    const char *line = text;
    const char *end = text + strlen(text);
    float h = 0;
    while (line < end)
    {
        const char *wrap = font->CalcWordWrapPositionA(size / font->FontSize, line, end, width);
        if (wrap == line)
        {
            wrap++;
        }
        const char *next = wrap;
        while (next < end && (*next == ' ' || *next == '\n'))
        {
            next++;
        }
        const float lw = font->CalcTextSizeA(size, FLT_MAX, 0.f, line, wrap).x;
        dl->AddText(font, size, ImVec2(floorf(x0 + (width - lw) / 2), y + h), col, line, wrap);
        h += size * 1.35f;
        line = next;
    }
    return h;
}

static void spinner(ImDrawList *dl, const ImVec2 &c, float r, float thick)
{
    const float t = float(ImGui::GetTime());
    dl->AddCircle(c, r, IM_COL32(255, 255, 255, 30), 48, thick);
    const float a0 = t * 4.2f;
    const float sweep = 1.6f + 0.9f * sinf(t * 2.1f);
    dl->PathArcTo(c, r, a0, a0 + sweep, 32);
    dl->PathStroke(COL_GOLD, false, thick);
}

static void alert_icon(ImDrawList *dl, const ImVec2 &c, float r, ImU32 col, float u)
{
    dl->AddCircle(c, r, col, 48, 2.f * u);
    dl->AddLine(ImVec2(c.x, c.y - r * 0.45f), ImVec2(c.x, c.y + r * 0.12f), col, 2.4f * u);
    dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.42f), 1.8f * u, col, 12);
}

static bool card_button(const char *label, const ImVec2 &size, bool primary)
{
    if (primary)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(COL_GOLD));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.91f, 0.77f, 0.50f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.74f, 0.59f, 0.33f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.08f, 0.05f, 1.f));
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 1.f, 1.f, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 1.f, 1.f, 0.14f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.f, 1.f, 1.f, 0.20f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_TEXT));
    }
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

static void back_to_login()
{
    later([] {
        g_ConnectionScreen.CreateSmoothAction(CConnectionScreen::ID_SMOOTH_CS_GO_SCREEN_MAIN);
    });
}

// The card while connecting to the login server: progress, or why it failed.
static void draw_status(const ImVec2 &pos, float width, float height, float pad, float u)
{
    ImFont *body = Overlay_LoginFont(LoginFont::Body);
    ImFont *small = Overlay_LoginFont(LoginFont::Small);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const bool lost = g_ConnectionScreen.GetType() == CST_CONLOST;
    const bool failed = g_ConnectionScreen.GetConnectionFailed();
    const bool waiting = !lost && !failed;
    const float innerW = width - pad * 2;

    // which server, at the top
    const auto &servers = LoginServers_List();
    const int active = LoginServers_Active();
    if (active >= 0 && active < int(servers.size()))
    {
        std::string label = LoginServers_DisplayName(servers[active]);
        std::transform(label.begin(), label.end(), label.begin(), ::toupper);
        text_wrapped_centered(dl, small, pos.x + pad, innerW, pos.y + pad, COL_MUTED, label.c_str());
    }

    // icon and message, centred in the space above the buttons
    const float buttonH = px(body) + 22.f * u;
    const float iconR = 22.f * u;
    const ImVec2 iconC(pos.x + width / 2, pos.y + height * 0.32f);
    if (waiting)
    {
        spinner(dl, iconC, iconR, 3.f * u);
    }
    else
    {
        alert_icon(dl, iconC, iconR, lost ? COL_MUTED : IM_COL32(248, 113, 113, 255), u);
    }
    const astr_t message = lost ? astr_t("Connection lost") : g_ConnectionScreen.LoginMessage();
    text_wrapped_centered(
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
        if (card_button("Cancel", ImVec2(innerW, buttonH), false) || escape)
        {
            later([] {
                g_ConnectionManager.Disconnect();
                g_ConnectionScreen.CreateSmoothAction(CConnectionScreen::ID_SMOOTH_CS_GO_SCREEN_MAIN);
            });
        }
    }
    else if (lost)
    {
        if (card_button("Back to Login", ImVec2(innerW, buttonH), true) || enter || escape)
        {
            back_to_login();
        }
    }
    else
    {
        const float gap = 10.f * u;
        const float half = (innerW - gap) / 2;
        if (card_button("Back", ImVec2(half, buttonH), false) || escape)
        {
            back_to_login();
        }
        ImGui::SameLine(0, gap);
        if (card_button("Try Again", ImVec2(half, buttonH), true) || enter)
        {
            later([] { g_Game.Connect(); });
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
    const float fieldH = px(body) + 22.f * u;
    const float height = pad * 2 + px(small) * 2 + fieldH * 3 + 4.f * u + px(body) +
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

    if (g_GameState == GS_MAIN_CONNECT)
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
        ImGui::Checkbox("Save password", &g_MainScreen.m_SavePassword->Checked);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Save your password in the config file");
        }
    }
    if (g_MainScreen.m_AutoLogin != nullptr)
    {
        ImGui::SameLine(width - pad * 2 - ImGui::CalcTextSize("Auto login").x - 26.f * u);
        ImGui::Checkbox("Auto login", &g_MainScreen.m_AutoLogin->Checked);
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
    const float lineH = px(small);

    // links, bottom left
    ImGui::PushFont(body);
    ImGui::SetCursorScreenPos(ImVec2(margin, display.y - margin - lineH - px(body) - 10.f * u));
    if (link("Account", "Manage your account on the EA website"))
    {
        Platform::OpenBrowser(URL_ACCOUNT);
    }
    const char *sep = "  \xC2\xB7  ";
    ImGui::SameLine(0, 0);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_MUTED), "%s", sep);
    ImGui::SameLine(0, 0);
    if (link("UO.com", "Ultima Online news and guides"))
    {
        Platform::OpenBrowser(URL_UOCOM);
    }
    ImGui::SameLine(0, 0);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_MUTED), "%s", sep);
    ImGui::SameLine(0, 0);
    if (link("Help", "EmmexUO on GitHub"))
    {
        Platform::OpenBrowser(URL_HELP);
    }
    ImGui::SameLine(0, 0);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_MUTED), "%s", sep);
    ImGui::SameLine(0, 0);
    if (link("Credits", "The people who built this client"))
    {
        Platform::OpenBrowser(URL_CREDITS);
    }

    // quit, bottom right
    const char *quit = "Quit";
    const ImVec2 quitSize = ImGui::CalcTextSize(quit);
    ImGui::SetCursorScreenPos(ImVec2(
        display.x - margin - quitSize.x, display.y - margin - lineH - px(body) - 10.f * u));
    if (link(quit, "Quit Ultima Online"))
    {
        later([] { g_MainScreen.ProcessSmoothAction(CMainScreen::ID_SMOOTH_MS_QUIT); });
    }
    ImGui::PopFont();

    // versions, along the very bottom
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const astr_t uo = "Ultima Online " + g_Config.ClientVersionString;
    dl->AddText(small, px(small), ImVec2(margin, display.y - margin - lineH + 4.f * u),
                IM_COL32(236, 230, 218, 90), uo.c_str());
    const char *client = CLIENT_VERSION;
    const float cw = small->CalcTextSizeA(px(small), FLT_MAX, 0.f, client).x;
    dl->AddText(small, px(small),
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

enum class Chevron
{
    Right,
    Down,
    Left,
};

static void chevron(ImDrawList *dl, const ImVec2 &c, float s, Chevron dir, ImU32 col, float thick)
{
    ImVec2 p[3];
    switch (dir)
    {
        case Chevron::Right:
            p[0] = ImVec2(c.x - s / 2, c.y - s);
            p[1] = ImVec2(c.x + s / 2, c.y);
            p[2] = ImVec2(c.x - s / 2, c.y + s);
            break;
        case Chevron::Down:
            p[0] = ImVec2(c.x - s, c.y - s / 2);
            p[1] = ImVec2(c.x, c.y + s / 2);
            p[2] = ImVec2(c.x + s, c.y - s / 2);
            break;
        case Chevron::Left:
            p[0] = ImVec2(c.x + s / 2, c.y - s);
            p[1] = ImVec2(c.x - s / 2, c.y);
            p[2] = ImVec2(c.x + s / 2, c.y + s);
            break;
    }
    dl->AddPolyline(p, 3, col, false, thick);
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
    const float h = px(body) + 18.f * u;
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
    chevron(dl, ImVec2(end.x - 20.f * u, cy), 4.f * u, Chevron::Down, hovered ? COL_GOLD : COL_MUTED, 1.6f * u);
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

// A labelled text field in the server editor; returns true once an edit is finished.
static bool field(const char *label, const char *hint, std::string *value, float width, bool focus = false,
                  ImGuiInputTextFlags flags = 0)
{
    ImFont *small = Overlay_LoginFont(LoginFont::Small);
    ImGui::BeginGroup();
    ImGui::PushFont(small);
    ImGui::TextDisabled("%s", label);
    ImGui::PopFont();
    ImGui::PushItemWidth(width);
    if (focus)
    {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputTextWithHint((std::string("##") + label).c_str(), hint, value, flags);
    const bool done = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopItemWidth();
    ImGui::EndGroup();
    return done;
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
    save |= field("NAME", "Unnamed server", &server.Name, width, focus);

    // side-by-side fields: line both groups up on the same top edge
    const float portW = 86.f * u;
    float rowY = ImGui::GetCursorPosY();
    if (field("ADDRESS", "login.example.com", &server.Host, width - portW - gap))
    {
        live = true;
    }
    ImGui::SameLine(0, gap);
    ImGui::SetCursorPosY(rowY);
    std::string port = std::to_string(server.Port);
    if (field("PORT", "2593", &port, portW, false, ImGuiInputTextFlags_CharsDecimal))
    {
        server.Port = std::max(1, std::min(65535, atoi(port.c_str())));
        live = true;
    }

    save |= field("CLIENT VERSION", "7.0.33.1", &server.ClientVersion, width);

    const float browseW = 84.f * u;
    rowY = ImGui::GetCursorPosY();
    save |= field("GAME DATA FOLDER", "Folder with the UO data files", &server.DataPath, width - browseW - gap);
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
    const float rowH = px(body) + px(small) + 26.f * u;
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
    dl->AddText(body, px(body), ImVec2(textX, pos.y + 11.f * u), COL_TEXT, LoginServers_DisplayName(server));
    char address[300];
    snprintf(
        address,
        sizeof(address),
        "%s:%d",
        server.Host.empty() ? "no address" : server.Host.c_str(),
        server.Port);
    dl->AddText(small, px(small), ImVec2(textX, pos.y + 15.f * u + px(body)), COL_MUTED, address);
    if (isActive)
    {
        const char *badge = "IN USE";
        const float bw = small->CalcTextSizeA(px(small), FLT_MAX, 0.f, badge).x;
        dl->AddText(small, px(small), ImVec2(end.x - chevronW - bw - 4.f * u, pos.y + 11.f * u + 2.f * u), COL_GOLD, badge);
    }
    chevron(
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
    dl->AddText(title, px(title) * 0.5f, ImVec2(x + pad, headerY), COL_TEXT, "Servers");
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
    chevron(
        dl,
        ImVec2(x + panelW - pad - closeSize / 2 + 8.f * u, headerY - 2.f * u + closeSize / 2),
        5.f * u,
        Chevron::Left,
        closeHovered ? COL_GOLD : COL_MUTED,
        1.8f * u);
    float y = headerY + px(title) * 0.5f + 8.f * u;
    dl->AddText(small, px(small), ImVec2(x + pad, y), COL_MUTED, "Click a server to use it, or the arrow to edit it.");
    y += px(small) + 18.f * u;

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
        const float bannerH = px(body) * 2 + 34.f * u;
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
        ImGui::SetCursorScreenPos(ImVec2(b1.x - rw - 12.f * u, b0.y + (bannerH - px(body) - 16.f * u) / 2));
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(COL_GOLD));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.91f, 0.77f, 0.50f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.74f, 0.59f, 0.33f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.08f, 0.05f, 1.f));
        if (ImGui::Button(restart, ImVec2(rw, 0)) && !busy())
        {
            later(LoginServers_Restart);
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

void Overlay_DrawLogin()
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    // layout unit, independent of the game's UI scale: 1 at 1280x800 points
    const float u = std::max(0.75f, std::min(2.f, std::min(display.x / 1280.f, display.y / 800.f)));
    s_textScale = ImGui::GetIO().FontGlobalScale * u / 2.f;

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

    // title
    ImFont *title = Overlay_LoginFont(LoginFont::Title);
    ImFont *small = Overlay_LoginFont(LoginFont::Small);
    const float titleY = std::max(display.y * 0.42f, 190.f * u) - px(title) - 58.f * u;
    text_centered(dl, title, titleY, x0, contentW, COL_TEXT, "ULTIMA ONLINE");
    // tagline between two short gold rules
    const char *tagline = "BRITANNIA AWAITS";
    const float ruleY = titleY + px(title) + 16.f * u;
    const float gap = small->CalcTextSizeA(px(small), FLT_MAX, 0.f, tagline).x / 2 + 16.f * u;
    const float ruleW = 64.f * u;
    const float mid = x0 + contentW / 2;
    const ImU32 ruleCol = IM_COL32(214, 176, 104, 140);
    dl->AddLine(ImVec2(mid - gap - ruleW, ruleY), ImVec2(mid - gap, ruleY), ruleCol);
    dl->AddLine(ImVec2(mid + gap, ruleY), ImVec2(mid + gap + ruleW, ruleY), ruleCol);
    text_centered(dl, small, ruleY - px(small) / 2 - 1.f, x0, contentW, COL_GOLD, tagline);

    draw_card(display, x0, contentW, u);
    draw_footer(display, u);
    if (g_GameState == GS_MAIN_CONNECT)
    {
        s_panelOpen = false; // no switching servers mid-connection
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
